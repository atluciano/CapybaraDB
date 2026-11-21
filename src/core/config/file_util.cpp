#include "core/config/file_util.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace capybaradb::core {
namespace {

std::atomic<uint64_t> g_tmp_counter{0};

} // namespace

bool atomic_write_utf8(const std::filesystem::path& target_path, const std::string& content) {
    std::error_code ec;
    const std::filesystem::path full_path = std::filesystem::weakly_canonical(target_path, ec);
    const std::filesystem::path resolved  = ec ? target_path : full_path;
    const std::filesystem::path parent    = resolved.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    const uint64_t seq = g_tmp_counter.fetch_add(1, std::memory_order_relaxed);
    const auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path tmp_path = resolved;
    tmp_path += "." + std::to_string(now_ticks) + "." + std::to_string(seq) + ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp_path, resolved, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::string data;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size > 0 && size <= 16 * 1024 * 1024) {
        data.resize(static_cast<size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(data.data(), size);
        if (!in) {
            return std::nullopt;
        }
        return data;
    }
    if (size == 0) {
        return data;
    }
    return std::nullopt;
}

} // namespace capybaradb::core
