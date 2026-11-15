#include "scan_settings.h"
#include "audio_source.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace cartoon {

bool valid_settings(const ScanSettings& settings) {
    return std::find(kCaptureWidths.begin(), kCaptureWidths.end(), settings.width) != kCaptureWidths.end() &&
        settings.height == settings.width * 3 / 4 &&
        (settings.decode_fps == 2 || settings.decode_fps == 5 || settings.decode_fps == 10) &&
        settings.exposure >= -2 && settings.exposure <= 2 &&
        (settings.algorithm == QrAlgorithm::ZBar || settings.algorithm == QrAlgorithm::ZXing) &&
        valid_audio_base(settings.base_address);
}

std::string settings_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') {
        return std::string(xdg) + "/cardputerzero-cartoon/scanner.conf";
    }
    const char* home = std::getenv("HOME");
    if (!home || *home != '/') {
        throw std::runtime_error(u8"无法确定设置保存目录");
    }
    return std::string(home) + "/.config/cardputerzero-cartoon/scanner.conf";
}

ScanSettings load_settings(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(u8"无法读取扫码设置；按 4 检查");
    }
    ScanSettings settings;
    std::string line;
    std::getline(input, line);
    std::istringstream parameters(line);
    unsigned center;
    unsigned algorithm = 0;
    std::string trailing;
    if (!(parameters >> settings.width >> settings.height >> settings.decode_fps >> settings.exposure >> center) ||
        center > 1 || input.bad()) {
        throw std::runtime_error(u8"扫码设置损坏；按 4 重新保存");
    }
    parameters >> std::ws;
    // Older five-field settings deliberately retain the existing ZBar default.
    if (parameters.peek() != std::char_traits<char>::eof() &&
        (!(parameters >> algorithm) || algorithm > 1 || (parameters >> trailing))) {
        throw std::runtime_error(u8"扫码设置损坏；按 4 重新保存");
    }
    settings.center_only = center == 1;
    settings.algorithm = static_cast<QrAlgorithm>(algorithm);
    bool base_seen = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (base_seen || line.rfind("base_address=", 0) != 0) {
            throw std::runtime_error(u8"扫码设置损坏；请检查 base_address 配置");
        }
        settings.base_address = line.substr(13);
        base_seen = true;
    }
    if (input.bad() || !valid_settings(settings)) {
        throw std::runtime_error(u8"扫码设置损坏；按 4 重新保存");
    }
    return settings;
}

void save_settings(const std::string& path, const ScanSettings& settings) {
    if (!valid_settings(settings)) {
        throw std::runtime_error(u8"扫码参数无效");
    }
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        throw std::runtime_error(u8"无法创建设置文件");
    }
    std::ostringstream contents;
    contents << settings.width << ' ' << settings.height << ' ' << settings.decode_fps << ' '
             << settings.exposure << ' ' << settings.center_only << ' '
             << static_cast<unsigned>(settings.algorithm) << '\n'
             << "base_address=" << settings.base_address << '\n';
    const auto text = contents.str();
    size_t offset = 0;
    bool success = true;
    while (offset < text.size()) {
        const auto count = write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { success = false; break; }
        offset += static_cast<size_t>(count);
    }
    if (success && fsync(fd) < 0) success = false;
    if (close(fd) < 0) success = false;
    if (success && rename(temporary.c_str(), path.c_str()) < 0) success = false;
    if (!success) {
        if (unlink(temporary.c_str()) < 0 && errno != ENOENT) {
            std::fprintf(stderr, "Cartoon: cannot remove temporary settings file\n");
        }
        throw std::runtime_error(u8"扫码设置保存失败");
    }
}

} // namespace cartoon
