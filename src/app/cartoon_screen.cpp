#include "cartoon_screen.h"

#include "asset_manager.h"
#include "camera_capture.h"
#include "linux_input.h"
#include "logger.h"
#include "scan_session.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"

#include <csignal>
#include <cstdio>
#include <utility>
#include <vector>

namespace cartoon {
namespace {

volatile std::sig_atomic_t interrupted = 0;

void interrupt(int) {
    interrupted = 1;
}

class ScannerScreen {
public:
    explicit ScannerScreen(app::AssetManager& assets) {
        root_ = lv_obj_create(nullptr);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, 320, 170);
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x101820), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        auto* text_font = assets.load_standard_font(14, app::StandardFontWeight::Regular);
        lv_obj_set_style_text_font(root_, text_font ? text_font : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(root_, lv_color_hex(0xf0f4f8), 0);

        title_ = label(root_, 6, 1, 308, "Cartoon | Starting camera...");
        auto* small_font = assets.load_standard_font(11, app::StandardFontWeight::Regular);
        footer_ = label(root_, 5, 151, 310, "5 Pause/Resume       ESC Exit");
        lv_obj_set_style_text_font(footer_, small_font ? small_font : &lv_font_montserrat_12, 0);

        preview_ = lv_image_create(root_);
        lv_obj_set_pos(preview_, 80, 25);
        lv_obj_set_size(preview_, kPreviewWidth, kPreviewHeight);
        guide_ = lv_obj_create(root_);
        lv_obj_remove_style_all(guide_);
        lv_obj_set_pos(guide_, 106, 39);
        lv_obj_set_size(guide_, 108, 92);
        lv_obj_set_style_border_width(guide_, 1, 0);
        lv_obj_set_style_border_color(guide_, lv_color_hex(0x45d6b3), 0);
        lv_obj_remove_flag(guide_, LV_OBJ_FLAG_SCROLLABLE);
        hint_ = label(root_, 15, 73, 290, "Aim camera at a QR code");
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);

        body_ = lv_obj_create(root_);
        lv_obj_remove_style_all(body_);
        lv_obj_set_pos(body_, 5, 24);
        lv_obj_set_size(body_, 310, 120);
        lv_obj_set_scroll_dir(body_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_AUTO);
        result_ = label(body_, 0, 0, 300, "");
        lv_label_set_long_mode(result_, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(body_, LV_OBJ_FLAG_HIDDEN);

        image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        image_.header.cf = LV_COLOR_FORMAT_RGB565;
        image_.header.w = kPreviewWidth;
        image_.header.h = kPreviewHeight;
        image_.header.stride = kPreviewWidth * sizeof(uint16_t);
        image_.data_size = kPreviewWidth * kPreviewHeight * sizeof(uint16_t);
        platform::set_global_key_listener(key_listener, this);
        lv_screen_load(root_);
        camera_.start();
    }

    ~ScannerScreen() {
        camera_.stop();
        platform::clear_global_key_listener(key_listener, this);
        lv_image_cache_drop(&image_);
        lv_obj_delete(root_);
    }

    bool running() const { return running_; }

    void update() {
        auto next = camera_.take_update();
        const auto previous = session_.state();
        if (!next.error.empty()) {
            session_.failed(std::move(next.error));
            show_state();
            return;
        }
        if (next.preview.empty() ||
            (previous != ScanState::Starting && previous != ScanState::Scanning)) {
            return;
        }
        lv_image_cache_drop(&image_);
        pixels_ = std::move(next.preview);
        image_.data = reinterpret_cast<const uint8_t*>(pixels_.data());
        lv_image_set_src(preview_, &image_);
        lv_obj_add_flag(hint_, LV_OBJ_FLAG_HIDDEN);
        session_.frame_received();
        if (next.result.found) {
            session_.decoded(std::move(next.result));
            std::fprintf(stderr, "Cartoon: QR result ready (%zu bytes); waiting for OK.\n",
                         session_.result().text.size());
        } else if (previous == ScanState::Starting) {
            std::fprintf(stderr, "Cartoon: camera preview active.\n");
        }
        if (session_.state() != previous) {
            show_state();
        }
    }

private:
    static lv_obj_t* label(lv_obj_t* parent, int x, int y, int width, const char* text) {
        auto* object = lv_label_create(parent);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_width(object, width);
        lv_label_set_text(object, text);
        return object;
    }

    void show_state() {
        const auto state = session_.state();
        const bool reading = state == ScanState::Result || state == ScanState::Error;
        if (reading) {
            lv_obj_add_flag(preview_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(guide_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(hint_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(body_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_scroll_to_y(body_, 0, LV_ANIM_OFF);
            lv_label_set_text(footer_, "UP/DOWN Scroll    OK Scan again    ESC Exit");
            if (state == ScanState::Error) {
                lv_label_set_text(title_, "Camera error | OK Retry");
                lv_label_set_text(result_, session_.error().c_str());
            } else if (!session_.result().supported) {
                lv_label_set_text(title_, "QR found | Unsupported content");
                lv_label_set_text(result_, "This QR code is not displayable UTF-8 text.\nPress OK to scan again.");
            } else {
                const auto& text = session_.result().text;
                const bool link = text.rfind("https://", 0) == 0 || text.rfind("http://", 0) == 0;
                lv_label_set_text(title_, link ? "QR found | Link" : "QR found | Text");
                lv_label_set_text(result_, text.empty() ? "(Empty QR text)" : text.c_str());
            }
        } else {
            lv_obj_add_flag(body_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(preview_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(guide_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(footer_, "5 Pause/Resume       ESC Exit");
            lv_label_set_text(title_, state == ScanState::Paused ? "Cartoon | Paused" :
                                      state == ScanState::Starting ? "Cartoon | Starting camera..." :
                                                                    "Cartoon | Scanning");
        }
    }

    bool key(uint32_t key, bool held) {
        if (held) {
            return true;
        }
        ScanKey action_key;
        switch (key) {
            case LV_KEY_ENTER: action_key = ScanKey::Ok; break;
            case '5': action_key = ScanKey::Pause; break;
            case LV_KEY_UP: action_key = ScanKey::Up; break;
            case LV_KEY_DOWN: action_key = ScanKey::Down; break;
            case LV_KEY_ESC: action_key = ScanKey::Escape; break;
            default: return false;
        }
        const auto previous = session_.state();
        const auto action = session_.press(action_key);
        switch (action) {
            case ScanAction::Quit:
                running_ = false;
                break;
            case ScanAction::Pause:
                camera_.pause();
                break;
            case ScanAction::Resume:
                if (previous == ScanState::Result) {
                    std::fprintf(stderr, "Cartoon: OK pressed; starting a new scan.\n");
                    camera_.start();
                } else {
                    camera_.resume();
                }
                break;
            case ScanAction::Retry:
                camera_.start();
                break;
            case ScanAction::ScrollUp:
                lv_obj_scroll_by(body_, 0, 48, LV_ANIM_OFF);
                break;
            case ScanAction::ScrollDown:
                lv_obj_scroll_by(body_, 0, -48, LV_ANIM_OFF);
                break;
            case ScanAction::None:
                break;
        }
        if (session_.state() != previous) {
            show_state();
        }
        return true;
    }

    static bool key_listener(uint32_t key, const char*, bool held, void* context) {
        return static_cast<ScannerScreen*>(context)->key(key, held);
    }

    CameraCapture camera_;
    ScanSession session_;
    bool running_{true};
    lv_obj_t* root_{nullptr};
    lv_obj_t* title_{nullptr};
    lv_obj_t* footer_{nullptr};
    lv_obj_t* preview_{nullptr};
    lv_obj_t* guide_{nullptr};
    lv_obj_t* hint_{nullptr};
    lv_obj_t* body_{nullptr};
    lv_obj_t* result_{nullptr};
    lv_image_dsc_t image_{};
    std::vector<uint16_t> pixels_;
};

} // namespace

int run() {
    logger::Logger::init();
    logger::Logger::set_tag("cartoon");
    std::signal(SIGTERM, interrupt);
    std::signal(SIGINT, interrupt);
    lv_init();
    auto* display = lv_linux_fbdev_create();
    if (!display || lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        LOG_ERROR("Cannot open display {}", APP_FRAMEBUFFER_DEVICE);
        return 1;
    }
    platform::init_key_input(display);
    {
        app::AssetManager assets;
        ScannerScreen screen(assets);
        while (screen.running() && !interrupted) {
            screen.update();
            lv_timer_handler();
            lv_delay_ms(10);
        }
    }
    lv_display_delete(display);
    std::fprintf(stderr, "Cartoon: exited; camera released.\n");
    return 0;
}

} // namespace cartoon
