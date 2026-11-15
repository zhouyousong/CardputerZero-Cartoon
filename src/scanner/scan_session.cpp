#include "scan_session.h"

#include <algorithm>
#include <utility>

namespace cartoon {

const AudioSource* ScanSession::current() const {
    return history_.empty() ? nullptr : &history_[selected_];
}

ScanAction ScanSession::play_selected() {
    if (!current()) {
        return ScanAction::None;
    }
    error_.clear();
    position_ = duration_ = 0;
    state_ = ScanState::Loading;
    return ScanAction::Play;
}

ScanAction ScanSession::suspend() {
    if (state_ == ScanState::Starting || state_ == ScanState::Scanning) {
        state_ = ScanState::ScanPaused;
        return ScanAction::PauseScan;
    }
    if (state_ == ScanState::Playing) {
        state_ = ScanState::AudioPaused;
        return ScanAction::PauseAudio;
    }
    return ScanAction::None;
}

ScanAction ScanSession::press(ScanKey key) {
    if (key == ScanKey::Escape) {
        return ScanAction::Quit;
    }
    if (key == ScanKey::Ok) {
        error_.clear();
        if (state_ == ScanState::Starting || state_ == ScanState::Scanning) {
            state_ = ScanState::Idle;
            position_ = duration_ = 0;
            return ScanAction::PauseScan;
        }
        state_ = ScanState::Starting;
        return ScanAction::StartScan;
    }
    if (key == ScanKey::Previous || key == ScanKey::Next) {
        if (history_.empty() ||
            (key == ScanKey::Previous && selected_ == 0) ||
            (key == ScanKey::Next && selected_ + 1 == history_.size())) {
            return ScanAction::None;
        }
        if (key == ScanKey::Previous) {
            --selected_;
        } else {
            ++selected_;
        }
        return play_selected();
    }
    if (key == ScanKey::Replay) {
        return play_selected();
    }
    if (key == ScanKey::Pause) {
        if (state_ == ScanState::Idle || state_ == ScanState::Finished) {
            return play_selected();
        }
        if (state_ == ScanState::Scanning || state_ == ScanState::Starting) {
            state_ = ScanState::ScanPaused;
            return ScanAction::PauseScan;
        }
        if (state_ == ScanState::ScanPaused || (state_ == ScanState::Error && camera_error_)) {
            error_.clear();
            state_ = ScanState::Starting;
            return ScanAction::StartScan;
        }
        if (state_ == ScanState::Playing) {
            state_ = ScanState::AudioPaused;
            return ScanAction::PauseAudio;
        }
        if (state_ == ScanState::AudioPaused) {
            state_ = ScanState::Playing;
            return ScanAction::ResumeAudio;
        }
        if (state_ == ScanState::Error && !camera_error_) {
            return play_selected();
        }
    }
    return ScanAction::None;
}

void ScanSession::frame_received() {
    if (state_ == ScanState::Starting) {
        state_ = ScanState::Scanning;
    }
}

ScanAction ScanSession::decoded(const DecodeResult& result) {
    if (!result.found || (state_ != ScanState::Scanning && state_ != ScanState::Starting)) {
        return ScanAction::None;
    }
    AudioSource source;
    std::string error;
    if (!result.supported || !parse_audio_source(result.text, source, error)) {
        failed(error.empty() ? u8"二维码不是有效的音频路径" : std::move(error), true);
        return ScanAction::PauseScan;
    }
    history_.erase(std::remove_if(history_.begin(), history_.end(),
        [&source](const AudioSource& old) { return old.path == source.path; }), history_.end());
    history_.insert(history_.begin(), std::move(source));
    if (history_.size() > 3) {
        history_.pop_back();
    }
    selected_ = 0;
    return play_selected();
}

void ScanSession::failed(std::string error, bool camera_error) {
    error_ = std::move(error);
    camera_error_ = camera_error;
    state_ = ScanState::Error;
}

void ScanSession::playback(const PlaybackUpdate& update) {
    if (state_ != ScanState::Loading && state_ != ScanState::Playing &&
        state_ != ScanState::AudioPaused) {
        return;
    }
    position_ = update.position;
    duration_ = update.duration;
    switch (update.state) {
        case PlaybackState::Loading: state_ = ScanState::Loading; break;
        case PlaybackState::Playing: state_ = ScanState::Playing; break;
        case PlaybackState::Paused: state_ = ScanState::AudioPaused; break;
        case PlaybackState::Finished: state_ = ScanState::Finished; break;
        case PlaybackState::Error: failed(update.error, false); break;
    }
}

} // namespace cartoon
