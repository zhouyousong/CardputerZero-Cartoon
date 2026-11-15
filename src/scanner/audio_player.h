#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace cartoon {

enum class PlaybackState { Loading, Playing, Paused, Finished, Error };

struct PlaybackUpdate {
    PlaybackState state{PlaybackState::Loading};
    double position{0};
    double duration{0};
    std::string error;
};

class AudioPlayer {
public:
    ~AudioPlayer();
    void play(const std::string& path, int volume);
    void stop();
    void set_paused(bool paused);
    void set_volume(int volume);
    std::optional<PlaybackUpdate> take_update();

private:
    void run(std::string path);
    void publish(const PlaybackUpdate& update);

    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::optional<PlaybackUpdate> update_;
    bool paused_{false};
    int volume_{60};
};

} // namespace cartoon
