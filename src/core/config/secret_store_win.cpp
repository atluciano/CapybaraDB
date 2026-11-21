#include "core/config/connection_profile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace capybaradb::core {

std::string generate_uuid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return {};
    }
    char buf[40]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(guid.Data1),
        static_cast<unsigned>(guid.Data2),
        static_cast<unsigned>(guid.Data3),
        static_cast<unsigned>(guid.Data4[0]),
        static_cast<unsigned>(guid.Data4[1]),
        static_cast<unsigned>(guid.Data4[2]),
        static_cast<unsigned>(guid.Data4[3]),
        static_cast<unsigned>(guid.Data4[4]),
        static_cast<unsigned>(guid.Data4[5]),
        static_cast<unsigned>(guid.Data4[6]),
        static_cast<unsigned>(guid.Data4[7]));
    return std::string{buf};
}

std::string protect_secret(
    std::string_view plaintext,
    std::string_view profile_guid_str) {
    std::vector<uint8_t> plain_bytes(plaintext.begin(), plaintext.end());
    uint8_t dummy = 0;

    DATA_BLOB in_blob{};
    in_blob.cbData = static_cast<DWORD>(plain_bytes.size());
    in_blob.pbData = plain_bytes.empty() ? &dummy : plain_bytes.data();

    const auto guid_bytes = parse_guid_bytes(profile_guid_str);
    DATA_BLOB entropy_blob{};
    if (guid_bytes.has_value()) {
        entropy_blob.cbData = static_cast<DWORD>(guid_bytes->size());
        entropy_blob.pbData = const_cast<BYTE*>(guid_bytes->data());
    }

    DATA_BLOB out_blob{};
    const BOOL ok = CryptProtectData(
        &in_blob,
        nullptr,
        guid_bytes.has_value() ? &entropy_blob : nullptr,
        nullptr,
        nullptr,
        CRYPTPROTECT_UI_FORBIDDEN,
        &out_blob);

    secure_wipe(plain_bytes);
    if (!ok || out_blob.pbData == nullptr) {
        return {};
    }

    std::string encoded = base64_encode(std::span<const uint8_t>(out_blob.pbData, out_blob.cbData));
    SecureZeroMemory(out_blob.pbData, out_blob.cbData);
    LocalFree(out_blob.pbData);
    return encoded;
}

std::optional<std::string> unprotect_secret(
    std::string_view encoded_ciphertext,
    std::string_view profile_guid_str) {
    if (encoded_ciphertext.empty()) {
        return std::string{};
    }
    const auto cipher_bytes = base64_decode(encoded_ciphertext);
    if (!cipher_bytes.has_value() || cipher_bytes->empty()) {
        return std::nullopt;
    }

    DATA_BLOB in_blob{};
    in_blob.cbData = static_cast<DWORD>(cipher_bytes->size());
    in_blob.pbData = const_cast<BYTE*>(cipher_bytes->data());

    const auto guid_bytes = parse_guid_bytes(profile_guid_str);
    DATA_BLOB entropy_blob{};
    if (guid_bytes.has_value()) {
        entropy_blob.cbData = static_cast<DWORD>(guid_bytes->size());
        entropy_blob.pbData = const_cast<BYTE*>(guid_bytes->data());
    }

    DATA_BLOB out_blob{};
    const BOOL ok = CryptUnprotectData(
        &in_blob,
        nullptr,
        guid_bytes.has_value() ? &entropy_blob : nullptr,
        nullptr,
        nullptr,
        CRYPTPROTECT_UI_FORBIDDEN,
        &out_blob);

    if (!ok || out_blob.pbData == nullptr) {
        return std::nullopt;
    }

    std::string plain(reinterpret_cast<const char*>(out_blob.pbData), out_blob.cbData);
    SecureZeroMemory(out_blob.pbData, out_blob.cbData);
    LocalFree(out_blob.pbData);
    return plain;
}

} // namespace capybaradb::core
