#include "core/completion/accent_fold.hpp"

#include <cstdint>

namespace capybaradb::core {
namespace {

uint32_t decode_utf8(std::string_view s, size_t& i) noexcept {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if ((b0 & 0x80u) == 0u) {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && i + 1 < s.size()) {
        const uint32_t cp = ((b0 & 0x1Fu) << 6u) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0u) == 0xE0u && i + 2 < s.size()) {
        const uint32_t cp = ((b0 & 0x0Fu) << 12u) |
                            ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6u) |
                            (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((b0 & 0xF8u) == 0xF0u && i + 3 < s.size()) {
        const uint32_t cp = ((b0 & 0x07u) << 18u) |
                            ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12u) |
                            ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6u) |
                            (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        i += 4;
        return cp;
    }
    ++i;
    return b0;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

char fold_latin_codepoint(uint32_t cp) noexcept {
    if (cp >= 'A' && cp <= 'Z') return static_cast<char>(cp + ('a' - 'A'));
    if (cp < 0x80u) return static_cast<char>(cp);

    switch (cp) {
    // Latin-1 Supplement (U+00C0..U+00FF)
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
    case 0x0100: case 0x0101: case 0x0102: case 0x0103: case 0x0104: case 0x0105:
        return 'a';
    case 0x00C7: case 0x00E7:
    case 0x0106: case 0x0107: case 0x0108: case 0x0109: case 0x010A: case 0x010B: case 0x010C: case 0x010D:
        return 'c';
    case 0x00D0: case 0x00F0:
    case 0x010E: case 0x010F: case 0x0110: case 0x0111:
        return 'd';
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
    case 0x0112: case 0x0113: case 0x0114: case 0x0115: case 0x0116: case 0x0117: case 0x0118: case 0x0119: case 0x011A: case 0x011B:
        return 'e';
    case 0x011C: case 0x011D: case 0x011E: case 0x011F: case 0x0120: case 0x0121: case 0x0122: case 0x0123:
        return 'g';
    case 0x0124: case 0x0125: case 0x0126: case 0x0127:
        return 'h';
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
    case 0x0128: case 0x0129: case 0x012A: case 0x012B: case 0x012C: case 0x012D: case 0x012E: case 0x012F: case 0x0130: case 0x0131:
        return 'i';
    case 0x0134: case 0x0135:
        return 'j';
    case 0x0136: case 0x0137:
        return 'k';
    case 0x0139: case 0x013A: case 0x013B: case 0x013C: case 0x013D: case 0x013E: case 0x013F: case 0x0140: case 0x0141: case 0x0142:
        return 'l';
    case 0x00D1: case 0x00F1:
    case 0x0143: case 0x0144: case 0x0145: case 0x0146: case 0x0147: case 0x0148:
        return 'n';
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
    case 0x014C: case 0x014D: case 0x014E: case 0x014F: case 0x0150: case 0x0151:
        return 'o';
    case 0x0154: case 0x0155: case 0x0156: case 0x0157: case 0x0158: case 0x0159:
        return 'r';
    case 0x015A: case 0x015B: case 0x015C: case 0x015D: case 0x015E: case 0x015F: case 0x0160: case 0x0161:
        return 's';
    case 0x0162: case 0x0163: case 0x0164: case 0x0165: case 0x0166: case 0x0167:
        return 't';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
    case 0x0168: case 0x0169: case 0x016A: case 0x016B: case 0x016C: case 0x016D: case 0x016E: case 0x016F: case 0x0170: case 0x0171: case 0x0172: case 0x0173:
        return 'u';
    case 0x0174: case 0x0175:
        return 'w';
    case 0x00DD: case 0x00FD: case 0x00FF: case 0x0176: case 0x0177: case 0x0178:
        return 'y';
    case 0x0179: case 0x017A: case 0x017B: case 0x017C: case 0x017D: case 0x017E:
        return 'z';
    default:
        return '\0';
    }
}

} // namespace

std::string fold_accents_lower(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = decode_utf8(utf8, i);
        // Strip NFD combining diacritical marks (U+0300..U+036F)
        if (cp >= 0x0300u && cp <= 0x036Fu) {
            continue;
        }
        if (cp == 0x00DFu) {
            out += "ss";
            continue;
        }
        if (cp == 0x00C6u || cp == 0x00E6u) {
            out += "ae";
            continue;
        }
        if (cp == 0x0152u || cp == 0x0153u) {
            out += "oe";
            continue;
        }
        if (const char folded = fold_latin_codepoint(cp); folded != '\0') {
            out.push_back(folded);
        } else {
            append_utf8(out, cp);
        }
    }
    return out;
}

bool accent_starts_with(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const std::string fh = fold_accents_lower(haystack);
    const std::string fn = fold_accents_lower(needle);
    return fh.starts_with(fn);
}

bool accent_contains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const std::string fh = fold_accents_lower(haystack);
    const std::string fn = fold_accents_lower(needle);
    return fh.find(fn) != std::string::npos;
}

} // namespace capybaradb::core
