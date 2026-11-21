#include "core/config/connection_profile.hpp"

#include <libsecret/secret.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>

namespace capybaradb::core {
namespace {

const SecretSchema* capybaradb_secret_schema() noexcept {
    static const SecretSchema schema = {
        "io.github.capybaradb.ConnectionPassword",
        SECRET_SCHEMA_NONE,
        {
            {"profile_id", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, static_cast<SecretSchemaAttributeType>(0)},
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

std::string derive_obfuscation_pad(std::string_view guid, size_t len) {
    // Fallback stream key when no D-Bus Secret Service session is running (e.g. headless CI).
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (const char ch : guid) {
        s ^= static_cast<uint8_t>(ch);
        s *= 0xBF58476D1CE4E5B9ull;
        s ^= s >> 31;
    }
    std::string out(len, '\0');
    for (size_t i = 0; i < len; ++i) {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        out[i] = static_cast<char>((z ^ (z >> 31)) & 0xFFu);
    }
    return out;
}

} // namespace

std::string generate_uuid() {
    std::array<uint8_t, 16> bytes{};
    bool filled = false;
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.is_open()) {
        urandom.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        filled = (urandom.gcount() == static_cast<std::streamsize>(bytes.size()));
    }
    if (!filled) {
        std::random_device rd;
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(rd() & 0xFFu);
        }
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3Fu) | 0x80u);

    char buf[40]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string{buf};
}

std::string protect_secret(
    std::string_view plaintext,
    std::string_view profile_guid_str) {
    if (std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr && !profile_guid_str.empty()) {
        GError* err = nullptr;
        std::string label = "CapybaraDB profile " + std::string(profile_guid_str);
        std::string plain_copy(plaintext);
        std::string id_copy(profile_guid_str);
        const gboolean ok = secret_password_store_sync(
            capybaradb_secret_schema(),
            SECRET_COLLECTION_DEFAULT,
            label.c_str(),
            plain_copy.c_str(),
            nullptr,
            &err,
            "profile_id", id_copy.c_str(),
            nullptr);
        secure_wipe(plain_copy);
        if (err != nullptr) {
            g_error_free(err);
        } else if (ok) {
            return "libsecret:v1";
        }
    }

    std::vector<uint8_t> cipher(plaintext.size());
    const std::string pad = derive_obfuscation_pad(profile_guid_str, plaintext.size());
    for (size_t i = 0; i < plaintext.size(); ++i) {
        cipher[i] = static_cast<uint8_t>(static_cast<uint8_t>(plaintext[i]) ^ static_cast<uint8_t>(pad[i]));
    }
    return "local:" + base64_encode(cipher);
}

std::optional<std::string> unprotect_secret(
    std::string_view encoded_ciphertext,
    std::string_view profile_guid_str) {
    if (encoded_ciphertext.empty()) {
        return std::string{};
    }
    if (encoded_ciphertext == "libsecret:v1") {
        if (std::getenv("DBUS_SESSION_BUS_ADDRESS") == nullptr || profile_guid_str.empty()) {
            return std::nullopt;
        }
        GError* err = nullptr;
        std::string id_copy(profile_guid_str);
        gchar* raw = secret_password_lookup_sync(
            capybaradb_secret_schema(),
            nullptr,
            &err,
            "profile_id", id_copy.c_str(),
            nullptr);
        if (err != nullptr) {
            g_error_free(err);
            return std::nullopt;
        }
        if (raw == nullptr) {
            return std::nullopt;
        }
        std::string out{raw};
        secret_password_free(raw);
        return out;
    }

    constexpr std::string_view kPrefix = "local:";
    if (encoded_ciphertext.starts_with(kPrefix)) {
        const auto cipher = base64_decode(encoded_ciphertext.substr(kPrefix.size()));
        if (!cipher.has_value()) {
            return std::nullopt;
        }
        const std::string pad = derive_obfuscation_pad(profile_guid_str, cipher->size());
        std::string plain(cipher->size(), '\0');
        for (size_t i = 0; i < cipher->size(); ++i) {
            plain[i] = static_cast<char>((*cipher)[i] ^ static_cast<uint8_t>(pad[i]));
        }
        return plain;
    }
    return std::nullopt;
}

} // namespace capybaradb::core
