#include "qr_decoder.h"

#include <limits>
#include <stdexcept>

namespace cartoon {

bool is_displayable_utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<uint8_t>(text[i++]);
        if (first < 0x80) {
            if ((first < 0x20 && first != '\n' && first != '\r' && first != '\t') ||
                first == 0x7f) {
                return false;
            }
            continue;
        }
        unsigned count;
        uint32_t value;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1; value = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2; value = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3; value = first & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (text.size() - i < count) {
            return false;
        }
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<uint8_t>(text[i++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

QrDecoder::QrDecoder() {
    scanner_.set_config(zbar::ZBAR_NONE, zbar::ZBAR_CFG_ENABLE, 0);
    scanner_.set_config(zbar::ZBAR_QRCODE, zbar::ZBAR_CFG_ENABLE, 1);
}

DecodeResult QrDecoder::decode(const uint8_t* pixels, unsigned width, unsigned height) {
    if (!pixels || !width || !height) {
        throw std::invalid_argument("QR image must contain grayscale pixels");
    }
    zbar::Image image(width, height, "Y800", pixels, static_cast<unsigned long>(width) * height);
    const int count = scanner_.scan(image);
    if (count < 0) {
        image.set_data(nullptr, 0);
        throw std::runtime_error("QR decoder could not scan the frame");
    }
    DecodeResult result;
    double best_distance = std::numeric_limits<double>::max();
    for (auto symbol = image.symbol_begin(); symbol != image.symbol_end(); ++symbol) {
        if (symbol->get_type() != zbar::ZBAR_QRCODE || symbol->get_location_size() == 0) {
            continue;
        }
        double x = 0;
        double y = 0;
        for (unsigned point = 0; point < symbol->get_location_size(); ++point) {
            x += symbol->get_location_x(point);
            y += symbol->get_location_y(point);
        }
        x = x / symbol->get_location_size() - width / 2.0;
        y = y / symbol->get_location_size() - height / 2.0;
        const double distance = x * x + y * y;
        if (distance < best_distance) {
            best_distance = distance;
            result.found = true;
            result.text = symbol->get_data();
            result.supported = is_displayable_utf8(result.text);
        }
    }
    image.set_data(nullptr, 0);
    return result;
}

} // namespace cartoon
