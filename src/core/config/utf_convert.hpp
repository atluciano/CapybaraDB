#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace capybaradb::core {

/// Converts valid UTF-8 into UTF-16 (replacing invalid sequences with U+FFFD).
[[nodiscard]] inline std::u16string utf8_to_utf16(std::string_view utf8) {
    std::u16string out;
    out.reserve(utf8.size());

    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        const auto b0 = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0xFFFDu;

        if (b0 < 0x80u) {
            cp = b0;
            ++i;
        } else if ((b0 & 0xE0u) == 0xC0u) {
            if (i + 1 < n) {
                const auto b1 = static_cast<uint8_t>(utf8[i + 1]);
                if ((b1 & 0xC0u) == 0x80u) {
                    const uint32_t cand = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
                    if (cand >= 0x80u) {
                        cp = cand;
                    }
                    i += 2;
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        } else if ((b0 & 0xF0u) == 0xE0u) {
            if (i + 2 < n) {
                const auto b1 = static_cast<uint8_t>(utf8[i + 1]);
                const auto b2 = static_cast<uint8_t>(utf8[i + 2]);
                if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                    const uint32_t cand = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
                    if (cand >= 0x800u && (cand < 0xD800u || cand > 0xDFFFu)) {
                        cp = cand;
                    }
                    i += 3;
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        } else if ((b0 & 0xF8u) == 0xF0u) {
            if (i + 3 < n) {
                const auto b1 = static_cast<uint8_t>(utf8[i + 1]);
                const auto b2 = static_cast<uint8_t>(utf8[i + 2]);
                const auto b3 = static_cast<uint8_t>(utf8[i + 3]);
                if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                    const uint32_t cand = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12)
                                        | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
                    if (cand >= 0x10000u && cand <= 0x10FFFFu) {
                        cp = cand;
                    }
                    i += 4;
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        } else {
            ++i;
        }

        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const uint32_t v = cp - 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00u + (v & 0x3FFu)));
        }
    }
    return out;
}

/// Converts UTF-16 code units into UTF-8 (replacing unpaired surrogates with U+FFFD).
[[nodiscard]] inline std::string utf16_to_utf8(std::u16string_view utf16) {
    std::string out;
    out.reserve(utf16.size());

    size_t i = 0;
    const size_t n = utf16.size();
    while (i < n) {
        uint32_t cp = utf16[i++];
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            if (i < n) {
                const uint32_t lo = utf16[i];
                if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                    ++i;
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                } else {
                    cp = 0xFFFDu;
                }
            } else {
                cp = 0xFFFDu;
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
        }

        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

#ifdef _WIN32
[[nodiscard]] inline std::wstring utf8_to_wstring(std::string_view utf8) {
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    const std::u16string u16 = utf8_to_utf16(utf8);
    return std::wstring(reinterpret_cast<const wchar_t*>(u16.data()), u16.size());
}

[[nodiscard]] inline std::string wstring_to_utf8(std::wstring_view wide) {
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    return utf16_to_utf8(std::u16string_view(reinterpret_cast<const char16_t*>(wide.data()), wide.size()));
}
#endif

} // namespace capybaradb::core
