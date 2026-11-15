#include "camera_capture.h"
#include "qr_decoder.h"
#include "scan_session.h"
#include "scan_settings.h"

#include <qrencode.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

std::atomic<unsigned> fixture_launches{0};
std::atomic<pid_t> fixture_pid{-1};

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

int camera_fixture(unsigned width, unsigned height) {
    std::vector<uint8_t> frame(width * height * 3 / 2, 128);
    std::fill_n(frame.begin(), width * height, 255);
    unsigned side;
    const auto code = qr("/fixture.wav", side);
    require(side <= width && side <= height, "Fixture QR exceeds capture dimensions");
    for (unsigned y = 0; y < side; ++y) {
        std::copy_n(code.data() + y * side, side,
            frame.data() + (y + (height - side) / 2) * width + (width - side) / 2);
    }
    for (;;) {
        size_t sent = 0;
        while (sent < frame.size()) {
            const auto count = write(STDOUT_FILENO, frame.data() + sent, frame.size() - sent);
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "Camera fixture write failed");
            sent += static_cast<size_t>(count);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void warm_capture_contract() {
    using namespace cartoon;
    CameraCapture camera;
    const auto wait_for_qr = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            auto update = camera.take_update();
            require(update.error.empty(), "Fixture capture must not report a camera error");
            if (update.result.found) {
                require(update.result.text == "/fixture.wav" && !update.preview.empty(),
                        "A fresh recognized QR must include its preview");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("Timed out waiting for fixture QR");
    };
    camera.start();
    wait_for_qr();
    const auto first_pid = fixture_pid.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(camera.running() && kill(first_pid, 0) == 0,
            "Recognizing a QR must retain the camera process");
    require(!camera.take_update().result.found, "Standby must not repeatedly publish the same QR");
    camera.pause();
    camera.start();
    wait_for_qr();
    require(fixture_launches == 1 && fixture_pid == first_pid,
            "An unchanged scan must reuse the warm camera");
    camera.pause();
    camera.start({480, 360, 5, 0, false});
    wait_for_qr();
    require(fixture_launches == 2 && fixture_pid != first_pid,
            "Changing capture settings must restart the camera");
    require(kill(first_pid, 0) < 0 && errno == ESRCH, "Old camera child must be reaped");
    const auto last_pid = fixture_pid.load();
    camera.stop();
    require(!camera.running() && kill(last_pid, 0) < 0 && errno == ESRCH,
            "Stopping capture must reap the warm child");
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
    require(session.state() == ScanState::Idle, "Launch must not open the camera");
    require(session.press(ScanKey::Ok) == ScanAction::StartScan, "OK must start scanning");
    session.frame_received();
    require(session.press(ScanKey::Pause) == ScanAction::PauseScan, "6 must suspend decoding when pausing");
    session.decoded({true, true, "stale"});
    require(session.state() == ScanState::ScanPaused && session.history().empty(),
            "Paused scan must ignore queued results");
    require(session.press(ScanKey::Pause) == ScanAction::StartScan, "6 must resume a paused scan");
    session.frame_received();
    require(session.decoded({true, true, "/home/pi/audio/first.wav"}) == ScanAction::Play,
            "Valid audio QR must autoplay");
    require(session.decoded({true, true, "/stale.wav"}) == ScanAction::None, "One scan must play only once");
    session.playback({PlaybackState::Playing, 1, 4, {}});
    require(session.press(ScanKey::Pause) == ScanAction::PauseAudio, "6 must pause audio");
    require(session.press(ScanKey::Pause) == ScanAction::ResumeAudio, "6 must resume audio");
    session.playback({PlaybackState::Finished, 4, 4, {}});
    require(session.state() == ScanState::Finished, "EOF must not loop or rescan");
    require(session.press(ScanKey::Replay) == ScanAction::Play && session.position() == 0,
            "Replay must start the selected audio from zero");
    require(session.press(ScanKey::Ok) == ScanAction::StartScan, "OK must stop audio and rescan");
    session.playback({PlaybackState::Playing, 2, 4, {}});
    require(session.state() == ScanState::Starting, "Old playback events must not interrupt a scan");
    require(session.history().size() == 1, "Rescan must retain the displayed history");
    session.failed("camera unplugged", true);
    require(session.press(ScanKey::Pause) == ScanAction::StartScan, "6 must retry a failed camera");
    require(session.error().empty(), "Retry must clear the previous camera error");
    require(session.press(ScanKey::Escape) == ScanAction::Quit, "ESC must exit");

    for (const auto* path : {"/b.wav", "/c.wav", "/d.wav"}) {
        session.press(ScanKey::Ok);
        require(session.decoded({true, true, path}) == ScanAction::Play, "Audio must enter history");
    }
    require(session.history().size() == 3 && session.history()[0].path == "/d.wav" &&
            session.history()[2].path == "/b.wav", "History must keep exactly three, newest first");
    require(session.press(ScanKey::Previous) == ScanAction::None, "History must not wrap at the newest");
    require(session.press(ScanKey::Next) == ScanAction::Play && session.current()->path == "/c.wav",
            "Next must select and autoplay the older audio");
    require(session.history()[0].path == "/d.wav", "Playback must not reorder history");
    session.press(ScanKey::Next);
    require(session.press(ScanKey::Next) == ScanAction::None, "History must not wrap at the oldest");
    session.press(ScanKey::Ok);
    session.decoded({true, true, "/c.wav"});
    require(session.history().size() == 3 && session.history()[0].path == "/c.wav",
            "Duplicate scan must move to front without duplicating entries");
    session.playback({PlaybackState::Error, 0, 0, "network failed"});
    require(session.press(ScanKey::Pause) == ScanAction::Play, "6 must retry the current failed audio");
    session.press(ScanKey::Ok);
    require(session.decoded({true, true, "not a path"}) == ScanAction::StopScan &&
            session.history().size() == 3, "Invalid QR must show an error without polluting history");
    session.press(ScanKey::Ok);
    require(session.suspend() == ScanAction::PauseScan && session.state() == ScanState::ScanPaused,
            "Settings must suspend decoding without forcing camera release");
    for (int i = 0; i < 30; ++i) session.press(ScanKey::Down);
    require(session.volume() == 0, "Volume must stop at zero");
    for (int i = 0; i < 30; ++i) session.press(ScanKey::Up);
    require(session.volume() == 100, "Volume must stop at 100");
    require(ScanSession{}.history().empty(), "History must not persist across sessions");
}

void source_and_settings_contract() {
    using namespace cartoon;
    AudioSource source;
    std::string error;
    require(parse_audio_source(u8"/home/pi/小熊的故事.wav", source, error) &&
            source.title == u8"小熊的故事.wav" && !source.network, "Local Unicode audio must be supported");
    require(parse_audio_source("https://example.com/music.mp3?token=abc", source, error) &&
            source.path == "https://example.com/music.mp3?token=abc" &&
            source.title == "music.mp3" && source.network, "Network queries must be preserved, not shown in titles");
    for (const auto* invalid : {"", "hello", "file:///etc/passwd", "https://", "https:// /x",
                               "ftp://example.com/audio", "/one\n/two", "https://user:password@host/x"}) {
        require(!parse_audio_source(invalid, source, error) && !error.empty(),
                "Invalid/unsupported audio sources must report an error");
    }
    require(parse_audio_source("/tmp/$(touch unwanted);song.wav", source, error) &&
            source.path == "/tmp/$(touch unwanted);song.wav", "Paths must remain data, not shell commands");
    const auto directory = std::filesystem::temp_directory_path() /
        ("cartoon-settings-test-" + std::to_string(getpid()));
    std::filesystem::create_directory(directory);
    const auto path = (directory / "scanner.conf").string();
    require(load_settings(path).width == 320, "Absent settings must use initial defaults");
    ScanSettings settings{640, 480, 2, -2, true};
    save_settings(path, settings);
    const auto restored = load_settings(path);
    require(restored.width == 640 && restored.height == 480 && restored.decode_fps == 2 &&
            restored.exposure == -2 && restored.center_only, "All scanner parameters must persist");
    for (const unsigned width : {480, 800, 1280}) {
        settings.width = width;
        settings.height = width * 3 / 4;
        save_settings(path, settings);
        require(load_settings(path).width == width, "New capture resolutions must persist");
        std::vector<uint8_t> frame(width * settings.height, 0);
        frame[0] = 255;
        const auto preview = make_preview(frame, settings);
        require(preview.size() == kPreviewWidth * kPreviewHeight && preview.back() == 0xffff,
                "New resolutions must retain fixed preview size and orientation");
    }
    { std::ofstream file(path); file << "640 240 99 8 2"; }
    bool rejected = false;
    try { load_settings(path); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "Corrupt settings must report an error, not silently reset");
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
}

void scan_region_contract() {
    using namespace cartoon;
    ScanSettings settings{640, 480, 10, 2, false};
    unsigned side;
    const auto code = qr("/song.wav", side);
    std::vector<uint8_t> frame(settings.width * settings.height, 255);
    for (unsigned y = 0; y < side; ++y) {
        std::copy_n(code.data() + y * side, side, frame.data() + y * settings.width);
    }
    QrDecoder decoder;
    require(decode_frame(decoder, frame, settings).found, "Full-frame scan must find an off-center code");
    settings.center_only = true;
    require(!decode_frame(decoder, frame, settings).found, "Center crop must exclude off-center codes");
    std::fill(frame.begin(), frame.end(), 255);
    for (unsigned y = 0; y < side; ++y) {
        std::copy_n(code.data() + y * side, side,
            frame.data() + (y + (settings.height - side) / 2) * settings.width + (settings.width - side) / 2);
    }
    require(decode_frame(decoder, frame, settings).found, "Center crop must decode a centered code");
    frame.assign(settings.width * settings.height, 0);
    frame[0] = 255;
    require(make_preview(frame, settings).back() == 0xffff, "640x480 must retain 180-degree orientation");
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

extern "C" int __real_posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*,
                                  const posix_spawnattr_t*, char* const[], char* const[]);

extern "C" int __wrap_posix_spawn(pid_t* pid, const char* path,
                                  const posix_spawn_file_actions_t* actions,
                                  const posix_spawnattr_t* attributes,
                                  char* const arguments[], char* const environment[]) {
    if (std::strcmp(path, "/usr/bin/rpicam-vid") != 0) {
        return __real_posix_spawn(pid, path, actions, attributes, arguments, environment);
    }
    const char* fixture_arguments[]{
        "/proc/self/exe", "--camera-fixture", arguments[3], arguments[5], nullptr
    };
    const int status = __real_posix_spawn(pid, fixture_arguments[0], actions, attributes,
        const_cast<char* const*>(fixture_arguments), environment);
    if (status == 0) {
        fixture_pid = *pid;
        ++fixture_launches;
    }
    return status;
}

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::strcmp(argv[1], "--camera-fixture") == 0) {
            return camera_fixture(static_cast<unsigned>(std::stoul(argv[2])),
                                  static_cast<unsigned>(std::stoul(argv[3])));
        }
        decode_contract();
        nearest_code_contract();
        interaction_contract();
        source_and_settings_contract();
        scan_region_contract();
        preview_orientation_contract();
        warm_capture_contract();
        std::puts("PASS: QR decoding, preview, audio/history controls, settings/crop and warm camera reuse/restart/cleanup.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
