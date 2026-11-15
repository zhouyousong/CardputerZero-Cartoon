#pragma once

#include "audio_player.h"
#include "audio_source.h"
#include "qr_decoder.h"

#include <vector>

namespace cartoon {

enum class ScanState { Idle, Starting, Scanning, ScanPaused, Loading, Playing, AudioPaused, Finished, Error };
enum class ScanKey { Ok, Pause, Previous, Next, Replay, Up, Down, Escape };
enum class ScanAction { None, StartScan, StopScan, Play, PauseAudio, ResumeAudio, Volume, Quit };

class ScanSession {
public:
    ScanAction press(ScanKey key);
    void frame_received();
    ScanAction decoded(const DecodeResult& result);
    void failed(std::string error, bool camera_error);
    void playback(const PlaybackUpdate& update);
    ScanAction suspend();
    ScanState state() const { return state_; }
    const std::vector<AudioSource>& history() const { return history_; }
    size_t selected() const { return selected_; }
    const AudioSource* current() const;
    const std::string& error() const { return error_; }
    int volume() const { return volume_; }
    double position() const { return position_; }
    double duration() const { return duration_; }

private:
    ScanAction play_selected();
    ScanState state_{ScanState::Idle};
    std::vector<AudioSource> history_;
    size_t selected_{0};
    std::string error_;
    bool camera_error_{false};
    int volume_{60};
    double position_{0};
    double duration_{0};
};

} // namespace cartoon
