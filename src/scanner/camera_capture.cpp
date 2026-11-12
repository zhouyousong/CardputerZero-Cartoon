#include "camera_capture.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace cartoon {
namespace {

class CameraProcess {
public:
    ~CameraProcess() {
        if (fd >= 0) {
            close(fd);
        }
        if (pid <= 0) {
            return;
        }
        kill(pid, SIGTERM);
        for (int attempt = 0; attempt < 50; ++attempt) {
            const auto status = waitpid(pid, nullptr, WNOHANG);
            if (status == pid || (status < 0 && errno == ECHILD)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid, SIGKILL);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }

    void launch() {
        int pipe_fds[2];
        if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
            throw std::runtime_error(std::string("Camera pipe: ") + strerror(errno));
        }
        posix_spawn_file_actions_t actions;
        int status = posix_spawn_file_actions_init(&actions);
        if (status != 0) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            throw std::runtime_error(std::string("Camera process setup: ") + strerror(status));
        }
        status = posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        if (status == 0) {
            status = posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        }
        if (status == 0) {
            status = posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
        }
        const char* arguments[] = {
            "/usr/bin/rpicam-vid", "--nopreview", "--width", "320", "--height", "240",
            "--framerate", "10", "--codec", "yuv420", "--timeout", "0", "--output", "-",
            nullptr
        };
        if (status == 0) {
            status = posix_spawn(&pid, arguments[0], &actions, nullptr,
                                 const_cast<char* const*>(arguments), environ);
        }
        posix_spawn_file_actions_destroy(&actions);
        close(pipe_fds[1]);
        if (status != 0) {
            close(pipe_fds[0]);
            throw std::runtime_error(std::string("Cannot start rpicam-vid: ") + strerror(status));
        }
        fd = pipe_fds[0];
    }

    int fd{-1};
    pid_t pid{-1};
};

} // namespace

std::vector<uint16_t> make_preview(const std::vector<uint8_t>& frame) {
    if (frame.size() < kCaptureWidth * kCaptureHeight) {
        throw std::invalid_argument("Preview requires a complete camera luminance frame");
    }
    std::vector<uint16_t> preview(kPreviewWidth * kPreviewHeight);
    for (unsigned y = 0; y < kPreviewHeight; ++y) {
        for (unsigned x = 0; x < kPreviewWidth; ++x) {
            const auto source_y = (kPreviewHeight - 1 - y) * 2;
            const auto source_x = (kPreviewWidth - 1 - x) * 2;
            const uint8_t value = frame[source_y * kCaptureWidth + source_x];
            preview[y * kPreviewWidth + x] =
                static_cast<uint16_t>(((value >> 3) << 11) |
                                      ((value >> 2) << 5) | (value >> 3));
        }
    }
    return preview;
}

CameraCapture::~CameraCapture() {
    stop();
}

void CameraCapture::start() {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        update_ = {};
        paused_ = false;
    }
    stopping_ = false;
    worker_ = std::thread(&CameraCapture::run, this);
}

void CameraCapture::stop() {
    stopping_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void CameraCapture::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = true;
    ++generation_;
    update_ = {};
}

void CameraCapture::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    update_ = {};
    ++generation_;
    paused_ = false;
}

CameraUpdate CameraCapture::take_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    CameraUpdate next = std::move(update_);
    update_ = {};
    return next;
}

void CameraCapture::report_error(const std::string& error) {
    std::fprintf(stderr, "Cartoon camera: %s\n", error.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    update_.error = error;
}

void CameraCapture::run() {
    try {
        capture();
    } catch (const std::exception& error) {
        if (!stopping_) {
            report_error(error.what());
        }
    }
}

void CameraCapture::capture() {
    CameraProcess process;
    process.launch();
    QrDecoder decoder;
    std::vector<uint8_t> frame(kCaptureWidth * kCaptureHeight * 3 / 2);
    size_t received = 0;
    auto last_frame = std::chrono::steady_clock::now();
    bool first_frame = true;
    unsigned frame_number = 0;
    while (!stopping_) {
        pollfd descriptor{process.fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("Camera read failed: ") + strerror(errno));
        }
        if (ready > 0) {
            if (descriptor.revents & POLLIN) {
                const auto bytes = read(process.fd, frame.data() + received, frame.size() - received);
                if (bytes < 0 && errno == EINTR) {
                    continue;
                }
                if (bytes <= 0) {
                    throw std::runtime_error("Camera stopped. Check connection or another camera app.");
                }
                received += static_cast<size_t>(bytes);
            } else if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                throw std::runtime_error("Camera unavailable. Check connection, permissions, or another camera app.");
            }
        }
        if (received == frame.size()) {
            received = 0;
            first_frame = false;
            last_frame = std::chrono::steady_clock::now();
            if (paused_) {
                continue;
            }
            const auto generation = generation_.load();
            CameraUpdate next;
            next.preview = make_preview(frame);
            if (++frame_number % 2 == 0) {
                next.result = decoder.decode(frame.data(), kCaptureWidth, kCaptureHeight);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (!paused_ && generation == generation_) {
                update_ = std::move(next);
                if (update_.result.found) {
                    paused_ = true;
                    return;
                }
            }
        }
        const auto idle = std::chrono::steady_clock::now() - last_frame;
        if (idle > std::chrono::seconds(first_frame ? 15 : 5)) {
            throw std::runtime_error("Camera timed out. Check connection and press OK to retry.");
        }
    }
}

} // namespace cartoon
