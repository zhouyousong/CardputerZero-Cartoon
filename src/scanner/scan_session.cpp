#include "scan_session.h"

#include <utility>

namespace cartoon {

ScanAction ScanSession::press(ScanKey key) {
    if (key == ScanKey::Escape) {
        return ScanAction::Quit;
    }
    if (key == ScanKey::Ok && (state_ == ScanState::Result || state_ == ScanState::Error)) {
        const bool retry = state_ == ScanState::Error;
        result_ = {};
        error_.clear();
        state_ = ScanState::Starting;
        return retry ? ScanAction::Retry : ScanAction::Resume;
    }
    if (state_ == ScanState::Result || state_ == ScanState::Error) {
        if (key == ScanKey::Up) {
            return ScanAction::ScrollUp;
        }
        if (key == ScanKey::Down) {
            return ScanAction::ScrollDown;
        }
    }
    if (key == ScanKey::Pause && state_ == ScanState::Scanning) {
        state_ = ScanState::Paused;
        return ScanAction::Pause;
    }
    if (key == ScanKey::Pause && state_ == ScanState::Paused) {
        state_ = ScanState::Starting;
        return ScanAction::Resume;
    }
    return ScanAction::None;
}

void ScanSession::frame_received() {
    if (state_ == ScanState::Starting) {
        state_ = ScanState::Scanning;
    }
}

void ScanSession::decoded(DecodeResult result) {
    if (result.found && (state_ == ScanState::Starting || state_ == ScanState::Scanning)) {
        result_ = std::move(result);
        state_ = ScanState::Result;
    }
}

void ScanSession::failed(std::string error) {
    error_ = std::move(error);
    state_ = ScanState::Error;
}

} // namespace cartoon
