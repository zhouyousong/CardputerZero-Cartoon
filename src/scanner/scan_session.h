#pragma once

#include "qr_decoder.h"

#include <string>

namespace cartoon {

enum class ScanState { Starting, Scanning, Paused, Result, Error };
enum class ScanKey { Ok, Pause, Up, Down, Escape };
enum class ScanAction { None, Resume, Pause, Retry, ScrollUp, ScrollDown, Quit };

class ScanSession {
public:
    ScanAction press(ScanKey key);
    void frame_received();
    void decoded(DecodeResult result);
    void failed(std::string error);
    ScanState state() const { return state_; }
    const DecodeResult& result() const { return result_; }
    const std::string& error() const { return error_; }

private:
    ScanState state_{ScanState::Starting};
    DecodeResult result_;
    std::string error_;
};

} // namespace cartoon
