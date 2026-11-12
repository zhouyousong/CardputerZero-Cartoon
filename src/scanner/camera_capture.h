#pragma once

#include "qr_decoder.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cartoon {

constexpr unsigned kCaptureWidth = 320;
constexpr unsigned kCaptureHeight = 240;
constexpr unsigned kPreviewWidth = 160;
constexpr unsigned kPreviewHeight = 120;

std::vector<uint16_t> make_preview(const std::vector<uint8_t>& frame);

struct CameraUpdate {
    std::vector<uint16_t> preview;
    DecodeResult result;
    std::string error;
};

class CameraCapture {
public:
    ~CameraCapture();
    void start();
    void stop();
    void pause();
    void resume();
    CameraUpdate take_update();

private:
    void run();
    void capture();
    void report_error(const std::string& error);

    std::atomic<bool> stopping_{false};
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> generation_{0};
    std::thread worker_;
    std::mutex mutex_;
    CameraUpdate update_;
};

} // namespace cartoon
