#include "camera_capture.h"
#include "qr_decoder.h"
#include "scan_session.h"

#include <qrencode.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<uint8_t> qr(const std::string& text, unsigned& side) {
    std::unique_ptr<QRcode, decltype(&QRcode_free)> code(
        QRcode_encodeString8bit(text.c_str(), 0, QR_ECLEVEL_M), QRcode_free);
    require(code != nullptr, "Could not generate test QR code");
    constexpr unsigned scale = 4;
    side = (code->width + 8) * scale;
    std::vector<uint8_t> pixels(side * side, 255);
    for (int y = 0; y < code->width; ++y) {
        for (int x = 0; x < code->width; ++x) {
            if (!(code->data[y * code->width + x] & 1)) {
                continue;
            }
            for (unsigned dy = 0; dy < scale; ++dy) {
                for (unsigned dx = 0; dx < scale; ++dx) {
                    pixels[((y + 4) * scale + dy) * side + (x + 4) * scale + dx] = 0;
                }
            }
        }
    }
    return pixels;
}

void decode_contract() {
    cartoon::QrDecoder decoder;
    for (const auto& text : std::vector<std::string>{
             "https://example.com/cartoon?value=42",
             u8"Cardputer QR: \u4e2d\u6587\nSecond line",
             std::string(1200, 'Q') + "\nlast line"}) {
        unsigned side;
        const auto pixels = qr(text, side);
        const auto decoded = decoder.decode(pixels.data(), side, side);
        require(decoded.found && decoded.supported, "Expected displayable QR text");
        require(decoded.text == text, "QR contents were truncated or changed");
    }
    std::vector<uint8_t> blank(320 * 240, 255);
    require(!decoder.decode(blank.data(), 320, 240).found, "Blank frame is not a QR code");
    require(!cartoon::is_displayable_utf8(std::string("a\0b", 3)), "Embedded NUL must be rejected");
    require(!cartoon::is_displayable_utf8("\xc0\xaf"), "Overlong UTF-8 must be rejected");
    require(!cartoon::is_displayable_utf8("\xed\xa0\x80"), "Surrogate UTF-8 must be rejected");
    require(!cartoon::is_displayable_utf8("\xf0\x9f"), "Truncated UTF-8 must be rejected");
    require(cartoon::is_displayable_utf8(u8"\U0001f600\n\ttext"), "Valid Unicode must be accepted");
}

void nearest_code_contract() {
    unsigned side;
    const auto left = qr("left", side);
    unsigned center_side;
    const auto center = qr("center", center_side);
    require(side == center_side, "Expected matching fixture sizes");
    constexpr unsigned width = 640;
    constexpr unsigned height = 240;
    std::vector<uint8_t> canvas(width * height, 255);
    for (unsigned y = 0; y < side; ++y) {
        std::copy_n(left.data() + y * side, side, canvas.data() + y * width);
        std::copy_n(center.data() + y * side, side,
                    canvas.data() + (y + (height - side) / 2) * width + (width - side) / 2);
    }
    cartoon::QrDecoder decoder;
    const auto decoded = decoder.decode(canvas.data(), width, height);
    require(decoded.found && decoded.text == "center", "The code nearest the center must win");
}

void interaction_contract() {
    using namespace cartoon;
    ScanSession session;
    session.frame_received();
    require(session.press(ScanKey::Pause) == ScanAction::Pause, "5 must pause scanning");
    session.decoded({true, true, "stale"});
    require(session.state() == ScanState::Paused, "Paused scan must ignore queued results");
    require(session.press(ScanKey::Pause) == ScanAction::Resume, "5 must resume a paused scan");
    session.frame_received();
    session.decoded({true, true, "first result"});
    require(session.press(ScanKey::Pause) == ScanAction::None, "5 must not restart a result");
    require(session.press(ScanKey::Down) == ScanAction::ScrollDown, "Result must support scrolling");
    require(session.press(ScanKey::Ok) == ScanAction::Resume, "OK must start a new scan");
    require(session.result().text.empty(), "Rescan must clear the previous result");
    session.failed("camera unplugged");
    require(session.press(ScanKey::Ok) == ScanAction::Retry, "OK must retry a failed camera");
    require(session.error().empty(), "Retry must clear the previous camera error");
    require(session.press(ScanKey::Escape) == ScanAction::Quit, "ESC must exit");
}

void preview_orientation_contract() {
    using namespace cartoon;
    std::vector<uint8_t> frame(kCaptureWidth * kCaptureHeight, 0);
    frame[0] = 0xff;
    frame[kCaptureWidth - 2] = 0x80;
    frame[(kCaptureHeight - 2) * kCaptureWidth] = 0x40;
    frame[(kCaptureHeight - 2) * kCaptureWidth + kCaptureWidth - 2] = 0x20;
    const auto preview = make_preview(frame);
    require(preview[0] == 0x2104, "Bottom-right must become preview top-left");
    require(preview[kPreviewWidth - 1] == 0x4208, "Bottom-left must become preview top-right");
    require(preview[(kPreviewHeight - 1) * kPreviewWidth] == 0x8410,
            "Top-right must become preview bottom-left");
    require(preview.back() == 0xffff, "Top-left must become preview bottom-right");
}

} // namespace

int main() {
    try {
        decode_contract();
        nearest_code_contract();
        interaction_contract();
        preview_orientation_contract();
        std::puts("PASS: QR decoding, exact Unicode/long text, center selection, OK rescan, recovery, and 180-degree preview.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
