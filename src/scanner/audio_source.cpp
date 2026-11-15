#include "audio_source.h"
#include "qr_decoder.h"

#include <filesystem>
#include <utility>

namespace cartoon {
namespace {

bool network_path(const std::string& path) {
    return path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0;
}

std::string remove_dot_segments(std::string input) {
    std::string output;
    while (!input.empty()) {
        if (input.rfind("../", 0) == 0) input.erase(0, 3);
        else if (input.rfind("./", 0) == 0) input.erase(0, 2);
        else if (input.rfind("/./", 0) == 0 || input == "/.") input.replace(0, 3, "/");
        else if (input.rfind("/../", 0) == 0 || input == "/..") {
            input.replace(0, 4, "/");
            const auto slash = output.find_last_of('/');
            output.erase(slash == std::string::npos ? 0 : slash);
        } else if (input == "." || input == "..") input.clear();
        else {
            const auto end = input.find('/', input.front() == '/' ? 1 : 0);
            output += input.substr(0, end);
            input.erase(0, end == std::string::npos ? input.size() : end);
        }
    }
    return output;
}

} // namespace

bool valid_audio_base(const std::string& base) {
    if (base.empty()) return true;
    AudioSource source;
    std::string error;
    if (!parse_audio_source(base, source, error) || source.path != base) return false;
    return !source.network || base.find_first_of("?#\\") == std::string::npos;
}

bool parse_audio_source(const std::string& text, AudioSource& source, std::string& error,
                        const std::string& base) {
    source = {};
    error.clear();
    if (text.empty() || text.size() > 8192 || !is_displayable_utf8(text)) {
        error = u8"二维码不是有效的音频路径";
        return false;
    }
    const auto first = text.find_first_not_of(' ');
    const auto last = text.find_last_not_of(' ');
    if (first == std::string::npos) {
        error = u8"音频路径为空";
        return false;
    }
    const auto path = text.substr(first, last - first + 1);
    for (const unsigned char ch : path) {
        if (ch < 32 || ch == 127) {
            error = u8"路径不能包含换行或控制字符";
            return false;
        }
    }
    const bool network = network_path(path);
    if (!network && path.front() != '/') {
        const auto colon = path.find(':');
        const auto separator = path.find_first_of("/?#");
        if (colon != std::string::npos && (separator == std::string::npos || colon < separator)) {
            error = u8"不支持此地址协议；仅支持本地路径和 HTTP/HTTPS";
            return false;
        }
        if (base.empty()) {
            error = u8"相对路径需要先设置音频基地址";
            return false;
        }
        if (!valid_audio_base(base)) {
            error = u8"音频基地址无效；请检查设置文件";
            return false;
        }
        std::string resolved;
        if (network_path(base)) {
            if (path.front() == '?' || path.front() == '#' || path.find('\\') != std::string::npos) {
                error = u8"相对网址必须包含音频文件路径，不能包含反斜杠";
                return false;
            }
            const auto host_end = base.find('/', base.find("://") + 3);
            const auto origin = base.substr(0, host_end);
            auto directory = host_end == std::string::npos ? "/" : base.substr(host_end);
            if (directory.back() != '/') directory += '/';
            const auto suffix = path.find_first_of("?#");
            resolved = origin + remove_dot_segments(directory + path.substr(0, suffix));
            if (suffix != std::string::npos) resolved += path.substr(suffix);
        } else {
            resolved = (std::filesystem::path(base) / path).lexically_normal().string();
        }
        return parse_audio_source(resolved, source, error);
    }
    std::string display_path = path;
    if (network) {
        const auto host_start = path.find("://") + 3;
        const auto host_end = path.find_first_of("/?#", host_start);
        const auto host = path.substr(host_start, host_end - host_start);
        if (host.empty() || host.find_first_of(" @\\") != std::string::npos) {
            error = u8"音频网址无效（不支持内嵌账户密码）";
            return false;
        }
        display_path = path.substr(0, path.find_first_of("?#"));
    }
    const auto slash = display_path.find_last_of('/');
    auto title = display_path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (title.empty()) {
        title = network ? u8"网络音频" : u8"本地音频";
    }
    source = {path, std::move(title), network};
    return true;
}

} // namespace cartoon
