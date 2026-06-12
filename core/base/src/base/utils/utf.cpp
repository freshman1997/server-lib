#include "base/utils/utf.h"

namespace yuan::base::util
{
    bool is_valid_utf8(std::span<const std::uint8_t> data)
    {
        std::size_t i = 0;
        while (i < data.size()) {
            const std::uint8_t byte = data[i];
            if (byte <= 0x7f) {
                ++i;
                continue;
            }

            std::uint32_t codepoint = 0;
            std::size_t needed = 0;
            std::uint32_t min_codepoint = 0;
            if ((byte & 0xe0) == 0xc0) {
                codepoint = byte & 0x1f;
                needed = 1;
                min_codepoint = 0x80;
            } else if ((byte & 0xf0) == 0xe0) {
                codepoint = byte & 0x0f;
                needed = 2;
                min_codepoint = 0x800;
            } else if ((byte & 0xf8) == 0xf0) {
                codepoint = byte & 0x07;
                needed = 3;
                min_codepoint = 0x10000;
            } else {
                return false;
            }

            if (i + needed >= data.size()) {
                return false;
            }

            for (std::size_t j = 1; j <= needed; ++j) {
                const std::uint8_t continuation = data[i + j];
                if ((continuation & 0xc0) != 0x80) {
                    return false;
                }
                codepoint = (codepoint << 6) | (continuation & 0x3f);
            }

            if (codepoint < min_codepoint || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
                return false;
            }

            i += needed + 1;
        }

        return true;
    }
}
