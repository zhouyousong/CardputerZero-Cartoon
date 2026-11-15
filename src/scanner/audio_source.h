#pragma once

#include <string>

namespace cartoon {

struct AudioSource {
    std::string path;
    std::string title;
    bool network{false};
};

bool parse_audio_source(const std::string& text, AudioSource& source, std::string& error);

} // namespace cartoon
