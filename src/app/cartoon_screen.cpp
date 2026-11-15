#include "cartoon_screen.h"

#include "asset_manager.h"
#include "audio_player.h"
#include "camera_capture.h"
#include "linux_input.h"
#include "logger.h"
#include "scan_session.h"
#include "scan_settings.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cartoon {
namespace {

volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }

std::string time_text(double seconds) {
    const auto value = static_cast<unsigned>(std::clamp(seconds, 0.0, 359999.0));
    char text[24];
    std::snprintf(text, sizeof(text), "%02u:%02u", value / 60, value % 60);
    return text;
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
        auto* font = assets.load_standard_font(12, app::StandardFontWeight::Regular);
        auto* small = assets.load_standard_font(10, app::StandardFontWeight::Regular);
        lv_obj_set_style_text_font(root_, font ? font : &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(root_, lv_color_hex(0xf0f4f8), 0);
        title_ = label(root_, 4, 1, 70, "Cartoon");
        auto* shortcuts = label(root_, 75, 2, 163, u8"R 重播  ↑↓ 音量  ESC 退出");
        lv_obj_set_style_text_font(shortcuts, small ? small : &lv_font_montserrat_12, 0);
        volume_ = label(root_, 240, 1, 77, "");

        camera_box_ = lv_obj_create(root_);
        lv_obj_remove_style_all(camera_box_);
        lv_obj_set_pos(camera_box_, 0, 22);
        lv_obj_set_size(camera_box_, 160, 120);
        lv_obj_set_style_bg_color(camera_box_, lv_color_hex(0x38414b), 0);
        lv_obj_set_style_bg_opa(camera_box_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(camera_box_, LV_OBJ_FLAG_SCROLLABLE);
        preview_ = lv_image_create(camera_box_);
        lv_obj_set_size(preview_, kPreviewWidth, kPreviewHeight);
        guide_ = lv_obj_create(camera_box_);
        lv_obj_remove_style_all(guide_);
        lv_obj_set_style_border_width(guide_, 1, 0);
        lv_obj_set_style_border_color(guide_, lv_color_hex(0x45d6b3), 0);
        lv_obj_remove_flag(guide_, LV_OBJ_FLAG_SCROLLABLE);
        hint_ = label(camera_box_, 4, 42, 152, u8"未扫描\n8/OK 扫码");
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
        label(root_, 166, 22, 150, u8"最近音频");
        for (size_t i = 0; i < tracks_.size(); ++i) {
            tracks_[i] = label(root_, 166, 41 + static_cast<int>(i) * 20, 150, "");
            lv_label_set_long_mode(tracks_[i], LV_LABEL_LONG_DOT);
            lv_obj_set_height(tracks_[i], 18);
        }
        status_ = label(root_, 166, 102, 150, u8"等待扫码");
        lv_label_set_long_mode(status_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        elapsed_ = label(root_, 166, 121, 150, "");
        progress_ = lv_bar_create(root_);
        lv_obj_set_pos(progress_, 166, 138);
        lv_obj_set_size(progress_, 150, 3);
        lv_bar_set_range(progress_, 0, 1000);
        lv_obj_set_style_bg_color(progress_, lv_color_hex(0x45d6b3), LV_PART_INDICATOR);
        constexpr std::array<int, 5> centers{54, 109, 162, 213, 266};
        constexpr std::array<const char*, 5> actions{
            u8"4\n设置", u8"5\n上一曲", u8"6\n暂停/继续", u8"7\n下一曲", u8"8/OK\n扫码"
        };
        for (size_t i = 0; i < centers.size(); ++i) {
            auto* button_hint = label(root_, centers[i] - 26, 143, 52, actions[i]);
            lv_obj_set_style_text_font(button_hint, small ? small : &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_align(button_hint, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_height(button_hint, 27);
            lv_obj_set_style_text_line_space(button_hint, -1, 0);
        }

        settings_screen_ = lv_obj_create(root_);
        lv_obj_remove_style_all(settings_screen_);
        lv_obj_set_size(settings_screen_, 320, 170);
        lv_obj_set_style_bg_color(settings_screen_, lv_color_hex(0x101820), 0);
        lv_obj_set_style_bg_opa(settings_screen_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(settings_screen_, LV_OBJ_FLAG_SCROLLABLE);
        settings_title_ = label(settings_screen_, 6, 2, 308, u8"扫码设置");
        lv_label_set_long_mode(settings_title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        for (size_t i = 0; i < setting_rows_.size(); ++i) {
            setting_rows_[i] = label(settings_screen_, 8, 30 + static_cast<int>(i) * 26, 304, "");
        }
        auto* settings_footer = label(settings_screen_, 5, 145, 310,
            u8"F/X 选择  Z/C 调值  OK 保存  ESC 取消");
        lv_obj_set_style_text_font(settings_footer, small ? small : &lv_font_montserrat_12, 0);
        lv_obj_add_flag(settings_screen_, LV_OBJ_FLAG_HIDDEN);

        image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        image_.header.cf = LV_COLOR_FORMAT_RGB565;
        image_.header.w = kPreviewWidth;
        image_.header.h = kPreviewHeight;
        image_.header.stride = kPreviewWidth * sizeof(uint16_t);
        image_.data_size = kPreviewWidth * kPreviewHeight * sizeof(uint16_t);
        try {
            config_path_ = settings_path();
            settings_ = load_settings(config_path_);
        } catch (const std::exception& error) {
            config_error_ = error.what();
            fail(config_error_, true);
        }
        platform::set_global_key_listener(key_listener, this);
        lv_screen_load(root_);
        show_state();
    }

    ~ScannerScreen() {
        camera_.stop();
        player_.stop();
        platform::clear_global_key_listener(key_listener, this);
        lv_image_cache_drop(&image_);
        lv_obj_delete(root_);
    }

    bool running() const { return running_; }

    void update() {
        if (warm_until_ && std::chrono::steady_clock::now() >= *warm_until_) {
            camera_.stop();
            warm_until_.reset();
            std::fprintf(stderr, "Cartoon camera: 30-second standby expired; camera released.\n");
            show_state();
        }
        auto next = camera_.take_update();
        const auto state = session_.state();
        const bool scanning = state == ScanState::Starting || state == ScanState::Scanning;
        if (scanning && !next.error.empty()) {
            camera_.stop();
            warm_until_.reset();
            fail(std::move(next.error), true);
        } else if (!scanning && !next.error.empty()) {
            camera_.stop();
            warm_until_.reset();
            show_state();
        } else if (scanning && !next.preview.empty()) {
            lv_image_cache_drop(&image_);
            pixels_ = std::move(next.preview);
            image_.data = reinterpret_cast<const uint8_t*>(pixels_.data());
            lv_image_set_src(preview_, &image_);
            lv_obj_add_flag(hint_, LV_OBJ_FLAG_HIDDEN);
            session_.frame_received();
            if (state == ScanState::Starting) {
                std::fprintf(stderr, "Cartoon: camera preview active.\n");
            }
            if (next.result.found) {
                execute(session_.decoded(next.result));
                if (session_.state() == ScanState::Error) {
                    std::fprintf(stderr, "Cartoon: %s\n", session_.error().c_str());
                }
            }
            show_state();
        }
        if (auto audio = player_.take_update()) {
            session_.playback(*audio);
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

    void fail(std::string error, bool camera_error) {
        std::fprintf(stderr, "Cartoon: %s\n", error.c_str());
        session_.failed(std::move(error), camera_error);
    }

    void standby() {
        if (!camera_.running()) return;
        camera_.pause();
        if (!warm_until_) {
            warm_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            std::fprintf(stderr, "Cartoon camera: warm standby started (30 seconds).\n");
        }
    }

    void execute(ScanAction action) {
        try {
            switch (action) {
                case ScanAction::StartScan:
                    player_.stop();
                    if (!config_error_.empty()) {
                        fail(config_error_, true);
                        break;
                    }
                    camera_.start(settings_);
                    warm_until_.reset();
                    break;
                case ScanAction::PauseScan:
                    standby();
                    break;
                case ScanAction::StopScan:
                    camera_.stop();
                    warm_until_.reset();
                    break;
                case ScanAction::Play:
                    standby();
                    player_.play(session_.current()->path, session_.volume());
                    break;
                case ScanAction::PauseAudio: player_.set_paused(true); break;
                case ScanAction::ResumeAudio: player_.set_paused(false); break;
                case ScanAction::Volume: player_.set_volume(session_.volume()); break;
                case ScanAction::Quit: running_ = false; break;
                case ScanAction::None: break;
            }
        } catch (const std::exception& error) {
            if (action == ScanAction::StartScan) warm_until_.reset();
            fail(error.what(), action == ScanAction::StartScan);
        }
    }

    void show_state() {
        lv_label_set_text_fmt(volume_, u8"音量 %d%%", session_.volume());
        const auto state = session_.state();
        const bool scanning = state == ScanState::Starting || state == ScanState::Scanning;
        if (scanning) {
            lv_obj_remove_flag(preview_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(guide_, LV_OBJ_FLAG_HIDDEN);
            const int margin_x = settings_.center_only ? 40 : 2;
            const int margin_y = settings_.center_only ? 30 : 2;
            lv_obj_set_pos(guide_, margin_x, margin_y);
            lv_obj_set_size(guide_, 160 - 2 * margin_x, 120 - 2 * margin_y);
            if (state == ScanState::Starting) {
                lv_obj_add_flag(preview_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(hint_, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(hint_, u8"正在启动摄像头");
            }
        } else {
            lv_obj_add_flag(preview_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(guide_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(hint_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(hint_, state == ScanState::ScanPaused ?
                u8"扫码已暂停\n6 继续" :
                (warm_until_ ? u8"摄像头待机\n8/OK 扫码" : u8"未扫描\n8/OK 扫码"));
        }
        for (size_t i = 0; i < tracks_.size(); ++i) {
            const bool selected = i < session_.history().size() && i == session_.selected();
            lv_obj_set_style_text_color(tracks_[i], lv_color_hex(selected ? 0x45d6b3 : 0xf0f4f8), 0);
            lv_label_set_long_mode(tracks_[i], selected ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
            const auto text = i < session_.history().size() ?
                std::string(selected ? "> " : "  ") + session_.history()[i].title : "";
            if (text != track_text_[i]) {
                track_text_[i] = text;
                lv_label_set_text(tracks_[i], text.c_str());
            }
        }
        const char* status = u8"等待扫码";
        switch (state) {
            case ScanState::Starting: status = u8"正在启动扫码"; break;
            case ScanState::Scanning: status = u8"正在扫描"; break;
            case ScanState::ScanPaused: status = u8"扫码已暂停"; break;
            case ScanState::Loading: status = u8"加载 / 缓冲中"; break;
            case ScanState::Playing: status = u8"正在播放"; break;
            case ScanState::AudioPaused: status = u8"播放已暂停"; break;
            case ScanState::Finished: status = u8"播放结束"; break;
            case ScanState::Error: status = session_.error().c_str(); break;
            case ScanState::Idle: break;
        }
        if (status_text_ != status) {
            status_text_ = status;
            lv_label_set_text(status_, status);
        }
        const auto time = session_.current() ?
            time_text(session_.position()) + " / " +
                (session_.duration() > 0 ? time_text(session_.duration()) : "--:--") : "";
        lv_label_set_text(elapsed_, time.c_str());
        lv_bar_set_value(progress_, session_.duration() > 0 ?
            static_cast<int>(std::clamp(session_.position() / session_.duration(), 0.0, 1.0) * 1000) : 0,
            LV_ANIM_OFF);
    }

    void show_settings() {
        const std::array<std::string, 4> values = {
            u8"采集分辨率: " + std::to_string(draft_.width) + " x " + std::to_string(draft_.height),
            u8"扫码频率: " + std::to_string(draft_.decode_fps) + u8" 次/秒",
            u8"曝光补偿: " + std::to_string(draft_.exposure) + " EV",
            std::string(u8"扫码区域: ") + (draft_.center_only ? u8"中心 50%" : u8"全画面")
        };
        for (size_t i = 0; i < values.size(); ++i) {
            lv_label_set_text(setting_rows_[i], values[i].c_str());
            lv_obj_set_style_text_color(setting_rows_[i],
                lv_color_hex(i == setting_index_ ? 0x45d6b3 : 0xf0f4f8), 0);
        }
    }

    bool settings_key(uint32_t key) {
        switch (key) {
            case 'f': case 'F': key = LV_KEY_UP; break;
            case 'x': case 'X': key = LV_KEY_DOWN; break;
            case 'z': case 'Z': key = LV_KEY_LEFT; break;
            case 'c': case 'C': key = LV_KEY_RIGHT; break;
            default: break;
        }
        if (key == LV_KEY_ESC) {
            in_settings_ = false;
            lv_obj_add_flag(settings_screen_, LV_OBJ_FLAG_HIDDEN);
        } else if (key == LV_KEY_ENTER) {
            try {
                if (config_path_.empty()) config_path_ = settings_path();
                save_settings(config_path_, draft_);
                settings_ = draft_;
                config_error_.clear();
                in_settings_ = false;
                lv_obj_add_flag(settings_screen_, LV_OBJ_FLAG_HIDDEN);
                std::fprintf(stderr, "Cartoon: scanner settings saved (%ux%u, %u fps, EV %d, center %d).\n",
                    settings_.width, settings_.height, settings_.decode_fps, settings_.exposure,
                    settings_.center_only);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "Cartoon settings: %s\n", error.what());
                lv_label_set_text(settings_title_, error.what());
            }
        } else if (key == LV_KEY_UP && setting_index_ > 0) {
            --setting_index_;
        } else if (key == LV_KEY_DOWN && setting_index_ + 1 < setting_rows_.size()) {
            ++setting_index_;
        } else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            const int step = key == LV_KEY_RIGHT ? 1 : -1;
            if (setting_index_ == 0) {
                const auto index = std::find(kCaptureWidths.begin(), kCaptureWidths.end(), draft_.width) -
                    kCaptureWidths.begin();
                draft_.width = kCaptureWidths[static_cast<size_t>(std::clamp(
                    static_cast<int>(index) + step, 0, static_cast<int>(kCaptureWidths.size()) - 1))];
                draft_.height = draft_.width * 3 / 4;
            } else if (setting_index_ == 1) {
                constexpr std::array<unsigned, 3> rates{2, 5, 10};
                const auto index = std::find(rates.begin(), rates.end(), draft_.decode_fps) - rates.begin();
                draft_.decode_fps = rates[static_cast<size_t>(std::clamp(static_cast<int>(index) + step, 0, 2))];
            } else if (setting_index_ == 2) {
                draft_.exposure = std::clamp(draft_.exposure + step, -2, 2);
            } else {
                draft_.center_only = !draft_.center_only;
            }
        }
        show_settings();
        show_state();
        return true;
    }

    bool key(uint32_t key, bool held) {
        if (held) return true;
        if (in_settings_) return settings_key(key);
        if (key == '4' || key == 's' || key == 'S') {
            execute(session_.suspend());
            if (session_.state() == ScanState::Loading) player_.set_paused(true);
            draft_ = settings_;
            setting_index_ = 0;
            in_settings_ = true;
            lv_label_set_text(settings_title_, u8"扫码设置（保存后下次扫码生效）");
            show_settings();
            lv_obj_remove_flag(settings_screen_, LV_OBJ_FLAG_HIDDEN);
            show_state();
            return true;
        }
        ScanKey input;
        switch (key) {
            case '8': case LV_KEY_ENTER: input = ScanKey::Ok; break;
            case '6': input = ScanKey::Pause; break;
            case '5': case LV_KEY_LEFT: input = ScanKey::Previous; break;
            case '7': case LV_KEY_RIGHT: input = ScanKey::Next; break;
            case 'r':
            case 'R': input = ScanKey::Replay; break;
            case LV_KEY_UP: input = ScanKey::Up; break;
            case LV_KEY_DOWN: input = ScanKey::Down; break;
            case LV_KEY_ESC: input = ScanKey::Escape; break;
            default: return false;
        }
        execute(session_.press(input));
        show_state();
        return true;
    }

    static bool key_listener(uint32_t key, const char*, bool held, void* context) {
        return static_cast<ScannerScreen*>(context)->key(key, held);
    }

    CameraCapture camera_;
    AudioPlayer player_;
    ScanSession session_;
    ScanSettings settings_;
    ScanSettings draft_;
    std::string config_path_;
    std::string config_error_;
    std::optional<std::chrono::steady_clock::time_point> warm_until_;
    std::array<std::string, 3> track_text_;
    std::string status_text_;
    bool running_{true};
    bool in_settings_{false};
    size_t setting_index_{0};
    lv_obj_t* root_{nullptr};
    lv_obj_t* title_{nullptr};
    lv_obj_t* volume_{nullptr};
    lv_obj_t* camera_box_{nullptr};
    lv_obj_t* preview_{nullptr};
    lv_obj_t* guide_{nullptr};
    lv_obj_t* hint_{nullptr};
    std::array<lv_obj_t*, 3> tracks_{};
    lv_obj_t* status_{nullptr};
    lv_obj_t* elapsed_{nullptr};
    lv_obj_t* progress_{nullptr};
    lv_obj_t* settings_screen_{nullptr};
    lv_obj_t* settings_title_{nullptr};
    std::array<lv_obj_t*, 4> setting_rows_{};
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
    std::fprintf(stderr, "Cartoon: exited; camera and audio released.\n");
    return 0;
}

} // namespace cartoon
