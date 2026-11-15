#pragma once

#include <array>
#include <string>

namespace cartoon {

inline constexpr std::array<unsigned, 5> kCaptureWidths{320, 480, 640, 800, 1280};

struct ScanSettings {
    unsigned width{320};
    unsigned height{240};
    unsigned decode_fps{5};
    int exposure{0};
    bool center_only{false};
};

bool valid_settings(const ScanSettings& settings);
std::string settings_path();
ScanSettings load_settings(const std::string& path);
void save_settings(const std::string& path, const ScanSettings& settings);

} // namespace cartoon
