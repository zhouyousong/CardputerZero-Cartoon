#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace cartoon {

enum class VolumeAction { Down, Up, ToggleMute };

struct VolumeUpdate {
    int percent{0};
    bool muted{false};
    std::string error;
};

class SystemVolume {
public:
    ~SystemVolume();
    void request(VolumeAction action);
    std::optional<VolumeUpdate> take_update();

private:
    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<VolumeAction> pending_;
    std::optional<VolumeUpdate> update_;
    bool stopping_{false};
    std::thread worker_;
};

} // namespace cartoon
