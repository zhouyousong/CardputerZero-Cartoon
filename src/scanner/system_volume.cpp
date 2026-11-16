#include "system_volume.h"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace cartoon {
namespace {

using Mainloop = std::unique_ptr<pa_mainloop, decltype(&pa_mainloop_free)>;
using Context = std::unique_ptr<pa_context, decltype(&pa_context_unref)>;
using Operation = std::unique_ptr<pa_operation, decltype(&pa_operation_unref)>;

class Mixer {
public:
    Mixer()
        : loop_(pa_mainloop_new(), pa_mainloop_free),
          context_(loop_ ? pa_context_new(pa_mainloop_get_api(loop_.get()), "Cartoon volume") : nullptr,
                   pa_context_unref) {
        if (!context_ || pa_context_connect(context_.get(), nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            throw std::runtime_error(u8"无法连接系统音量服务");
        }
        while (pa_context_get_state(context_.get()) != PA_CONTEXT_READY) pump();
    }

    ~Mixer() {
        if (context_) pa_context_disconnect(context_.get());
    }

    VolumeUpdate apply(VolumeAction action) {
        read();
        if (action == VolumeAction::ToggleMute) {
            write_mute(!muted_);
        } else {
            const int percent = std::clamp(current_percent() + (action == VolumeAction::Up ? 5 : -5), 0, 100);
            pa_cvolume target;
            pa_cvolume_set(&target, volume_.channels,
                static_cast<pa_volume_t>(std::lround(percent * PA_VOLUME_NORM / 100.0)));
            succeeded_ = false;
            finish(pa_context_set_sink_volume_by_name(context_.get(), name_.c_str(), &target, written, this));
            check_write();
            if (muted_) write_mute(false);
        }
        read();
        return {current_percent(), muted_, {}};
    }

private:
    void pump() {
        if (std::chrono::steady_clock::now() >= deadline_) {
            throw std::runtime_error(u8"系统音量服务超时");
        }
        const auto state = pa_context_get_state(context_.get());
        if (!PA_CONTEXT_IS_GOOD(state) || pa_mainloop_iterate(loop_.get(), 0, nullptr) < 0) {
            throw std::runtime_error(std::string(u8"系统音量服务失败: ") +
                                     pa_strerror(pa_context_errno(context_.get())));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    void finish(pa_operation* raw) {
        Operation operation(raw, pa_operation_unref);
        if (!operation) throw std::runtime_error(u8"无法发送系统音量命令");
        try {
            while (pa_operation_get_state(operation.get()) == PA_OPERATION_RUNNING) pump();
        } catch (...) {
            pa_operation_cancel(operation.get());
            throw;
        }
        if (pa_operation_get_state(operation.get()) != PA_OPERATION_DONE) {
            throw std::runtime_error(u8"系统音量命令被取消");
        }
    }

    static void sink_info(pa_context*, const pa_sink_info* info, int, void* data) {
        auto& self = *static_cast<Mixer*>(data);
        if (info) {
            self.name_ = info->name;
            self.volume_ = info->volume;
            self.muted_ = info->mute != 0;
            self.received_ = true;
        }
    }

    static void written(pa_context*, int success, void* data) {
        static_cast<Mixer*>(data)->succeeded_ = success != 0;
    }

    void read() {
        received_ = false;
        finish(pa_context_get_sink_info_by_name(context_.get(),
            name_.empty() ? nullptr : name_.c_str(), sink_info, this));
        if (!received_ || !pa_cvolume_valid(&volume_)) {
            throw std::runtime_error(u8"无法读取系统输出音量");
        }
    }

    int current_percent() const {
        return static_cast<int>(std::lround(pa_cvolume_avg(&volume_) * 100.0 / PA_VOLUME_NORM));
    }

    void check_write() {
        if (!succeeded_) throw std::runtime_error(u8"无法修改系统输出音量");
    }

    void write_mute(bool muted) {
        succeeded_ = false;
        finish(pa_context_set_sink_mute_by_name(context_.get(), name_.c_str(), muted, written, this));
        check_write();
    }

    Mainloop loop_;
    Context context_;
    const std::chrono::steady_clock::time_point deadline_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string name_;
    pa_cvolume volume_{};
    bool muted_{false};
    bool received_{false};
    bool succeeded_{false};
};

} // namespace

SystemVolume::~SystemVolume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_.clear();
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void SystemVolume::request(VolumeAction action) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!worker_.joinable()) worker_ = std::thread(&SystemVolume::run, this);
    pending_.push_back(action);
    wake_.notify_one();
}

std::optional<VolumeUpdate> SystemVolume::take_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::move(update_);
    update_.reset();
    return result;
}

void SystemVolume::run() {
    for (;;) {
        VolumeAction action;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            action = pending_.front();
            pending_.pop_front();
        }
        VolumeUpdate result;
        try {
            result = Mixer().apply(action);
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        update_ = std::move(result);
    }
}

} // namespace cartoon
