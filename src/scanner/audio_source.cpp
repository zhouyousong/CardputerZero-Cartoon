#include "audio_source.h"
#include "qr_decoder.h"

#include <utility>

namespace cartoon {

bool parse_audio_source(const std::string& text, AudioSource& source, std::string& error) {
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
    const bool network = path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0;
    if (!network && path.front() != '/') {
        error = u8"请扫描本地绝对路径或 HTTP/HTTPS 音频直链";
        return false;
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
