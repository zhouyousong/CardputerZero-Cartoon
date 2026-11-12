#pragma once

#include <cstdint>
#include <string>
#include <zbar.h>

namespace cartoon {

struct DecodeResult {
    bool found{false};
    bool supported{false};
    std::string text;
};

bool is_displayable_utf8(const std::string& text);

class QrDecoder {
public:
    QrDecoder();
    DecodeResult decode(const uint8_t* pixels, unsigned width, unsigned height);

private:
    zbar::ImageScanner scanner_;
};

} // namespace cartoon
