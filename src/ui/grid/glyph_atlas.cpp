#include "ui/grid/glyph_atlas.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace capybaradb::ui::grid {
namespace {

std::vector<std::filesystem::path> candidate_ui_font_paths() {
    std::vector<std::filesystem::path> paths;
#ifdef _WIN32
    std::filesystem::path win_dir = "C:/Windows";
    char* env_windir = nullptr;
    size_t env_len = 0;
    if (_dupenv_s(&env_windir, &env_len, "WINDIR") == 0 && env_windir != nullptr) {
        if (*env_windir != '\0') {
            win_dir = env_windir;
        }
        std::free(env_windir);
    }
    const std::filesystem::path fonts = win_dir / "Fonts";
    paths.push_back(fonts / "segoeui.ttf");
    paths.push_back(fonts / "arial.ttf");
    paths.push_back(fonts / "tahoma.ttf");
    paths.push_back(fonts / "consola.ttf");
#else
    paths.push_back("/usr/share/fonts/truetype/inter/Inter-Regular.ttf");
    paths.push_back("/usr/share/fonts/opentype/inter/Inter-Regular.otf");
    paths.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    paths.push_back("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf");
    paths.push_back("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
    paths.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    paths.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
#endif
    return paths;
}

struct DprAtlasSlot {
    int                         dpr_key = 0;
    std::unique_ptr<GlyphAtlas> atlas;
};

constexpr size_t kMaxDprAtlasSlots = 8u;

struct DprAtlasTable {
    std::array<DprAtlasSlot, kMaxDprAtlasSlots> slots{};
    size_t                                      count = 0;
};

DprAtlasTable& dpr_atlas_table() noexcept {
    static DprAtlasTable instance;
    return instance;
}

int quantize_dpr_key(float dpr) noexcept {
    const float clamped = (std::max)(1.0f, (std::min)(4.0f, dpr));
    return static_cast<int>(std::lround(clamped * 100.0f));
}

} // namespace

GlyphAtlas::GlyphAtlas() = default;

GlyphAtlas::~GlyphAtlas() noexcept {
    destroy_freetype();
}

void GlyphAtlas::destroy_freetype() noexcept {
    if (hb_buf_ != nullptr) {
        hb_buffer_destroy(hb_buf_);
        hb_buf_ = nullptr;
    }
    if (hb_font_ != nullptr) {
        hb_font_destroy(hb_font_);
        hb_font_ = nullptr;
    }
    if (ft_face_ != nullptr) {
        FT_Done_Face(ft_face_);
        ft_face_ = nullptr;
    }
    if (ft_lib_ != nullptr) {
        FT_Done_FreeType(ft_lib_);
        ft_lib_ = nullptr;
    }
}

void GlyphAtlas::clear_atlas_pixels() {
    atlas_size_ = kInitialAtlasSize;
    atlas_pixels_.assign(static_cast<size_t>(atlas_size_) * atlas_size_, 0u);
    shelf_x_ = 1;
    shelf_y_ = 1;
    shelf_h_ = 0;
    glyph_cache_.clear();
    shaped_map_.clear();
    shaped_entries_.clear();
    shaped_entries_.reserve(kMaxShapedCache);
    shaped_clock_hand_ = 0;
    ++atlas_version_;
}

bool GlyphAtlas::grow_atlas(uint32_t new_size) {
    if (new_size <= atlas_size_ || new_size > kMaxAtlasSize) {
        return false;
    }
    std::vector<uint8_t> next_pixels(static_cast<size_t>(new_size) * new_size, 0u);
    const uint32_t used_rows = (std::min)(atlas_size_, shelf_y_ + shelf_h_ + 1u);
    for (uint32_t y = 0; y < used_rows; ++y) {
        std::memcpy(
            next_pixels.data() + static_cast<size_t>(y) * new_size,
            atlas_pixels_.data() + static_cast<size_t>(y) * atlas_size_,
            atlas_size_);
    }
    atlas_pixels_ = std::move(next_pixels);
    atlas_size_   = new_size;

    const float inv = 1.0f / static_cast<float>(atlas_size_);
    auto rescale_uv = [inv](GlyphMetric& m) noexcept {
        if (m.valid && m.w > 0.0f && m.h > 0.0f) {
            m.u0 = static_cast<float>(m.sx) * inv;
            m.v0 = static_cast<float>(m.sy) * inv;
            m.u1 = (static_cast<float>(m.sx) + m.w) * inv;
            m.v1 = (static_cast<float>(m.sy) + m.h) * inv;
        }
    };
    for (GlyphMetric& m : latin1_) {
        rescale_uv(m);
    }
    rescale_uv(ellipsis_);
    for (auto& [_, m] : glyph_cache_) {
        rescale_uv(m);
    }
    ++atlas_version_;
    return true;
}

bool GlyphAtlas::load_platform_font(float pixel_size) {
    if (ft_lib_ == nullptr) {
        if (FT_Init_FreeType(&ft_lib_) != 0) {
            return false;
        }
    }
    if (hb_font_ != nullptr) {
        hb_font_destroy(hb_font_);
        hb_font_ = nullptr;
    }
    if (ft_face_ != nullptr) {
        FT_Done_Face(ft_face_);
        ft_face_ = nullptr;
    }

    for (const auto& p : candidate_ui_font_paths()) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            continue;
        }
        const std::string utf8_path = p.string();
        if (FT_New_Face(ft_lib_, utf8_path.c_str(), 0, &ft_face_) == 0) {
            break;
        }
    }

    if (ft_face_ == nullptr) {
        return false;
    }

    const FT_UInt px = static_cast<FT_UInt>((std::max)(8.0f, std::round(pixel_size)));
    (void)FT_Set_Pixel_Sizes(ft_face_, 0, px);

    ascender_px_  = static_cast<float>(ft_face_->size->metrics.ascender) / 64.0f;
    descender_px_ = static_cast<float>(ft_face_->size->metrics.descender) / 64.0f;
    fallback_adv_ = std::round(pixel_size * 0.55f);

    hb_font_ = hb_ft_font_create_referenced(ft_face_);
    if (hb_buf_ == nullptr) {
        hb_buf_ = hb_buffer_create();
    }
    return true;
}

bool GlyphAtlas::pack_bitmap(const uint8_t* src_buf, int src_pitch, uint32_t gw, uint32_t gh, GlyphMetric& out) {
    if (gw == 0 || gh == 0 || src_buf == nullptr) {
        return true;
    }
    if (atlas_size_ == 0) {
        clear_atlas_pixels();
    }

    while (gw + 2u >= atlas_size_) {
        if (atlas_size_ >= kMaxAtlasSize || !grow_atlas(atlas_size_ * 2u)) {
            out.w = 0.0f;
            out.h = 0.0f;
            return true;
        }
    }

    if (shelf_x_ + gw + 2u >= atlas_size_) {
        shelf_x_ = 1u;
        shelf_y_ += shelf_h_ + 2u;
        shelf_h_ = 0u;
    }
    while (shelf_y_ + gh + 2u >= atlas_size_) {
        if (atlas_size_ >= kMaxAtlasSize || !grow_atlas(atlas_size_ * 2u)) {
            out.w = 0.0f;
            out.h = 0.0f;
            return true;
        }
    }

    const size_t pitch_abs = static_cast<size_t>(std::abs(src_pitch));
    for (uint32_t row = 0; row < gh; ++row) {
        const uint8_t* src = src_buf + static_cast<size_t>(row) * pitch_abs;
        uint8_t* dst = atlas_pixels_.data() + static_cast<size_t>(shelf_y_ + row) * atlas_size_ + shelf_x_;
        std::memcpy(dst, src, gw);
    }

    out.sx = static_cast<uint16_t>(shelf_x_);
    out.sy = static_cast<uint16_t>(shelf_y_);

    const float inv = 1.0f / static_cast<float>(atlas_size_);
    out.u0 = static_cast<float>(shelf_x_) * inv;
    out.v0 = static_cast<float>(shelf_y_) * inv;
    out.u1 = static_cast<float>(shelf_x_ + gw) * inv;
    out.v1 = static_cast<float>(shelf_y_ + gh) * inv;

    shelf_x_ += gw + 2u;
    shelf_h_ = (std::max)(shelf_h_, gh);
    ++atlas_version_;
    return true;
}

bool GlyphAtlas::rasterize_glyph_id(uint32_t glyph_id, GlyphMetric& out) {
    out = GlyphMetric{};
    if (ft_face_ == nullptr) {
        return false;
    }
    if (FT_Load_Glyph(ft_face_, glyph_id, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
        return false;
    }

    const FT_GlyphSlot slot = ft_face_->glyph;
    const FT_Bitmap& bmp    = slot->bitmap;
    const uint32_t gw       = bmp.width;
    const uint32_t gh       = bmp.rows;

    out.advance_x = static_cast<float>(slot->advance.x) / 64.0f;
    out.bearing_x = static_cast<float>(slot->bitmap_left);
    out.bearing_y = static_cast<float>(slot->bitmap_top);
    out.w         = static_cast<float>(gw);
    out.h         = static_cast<float>(gh);
    out.valid     = true;

    return pack_bitmap(bmp.buffer, bmp.pitch, gw, gh, out);
}

bool GlyphAtlas::pack_custom_rect_for_test(uint32_t gw, uint32_t gh, uint8_t fill_value, GlyphMetric& out) {
    out = GlyphMetric{};
    out.advance_x = static_cast<float>(gw);
    out.w         = static_cast<float>(gw);
    out.h         = static_cast<float>(gh);
    out.valid     = true;
    std::vector<uint8_t> buf(static_cast<size_t>(gw) * gh, fill_value);
    return pack_bitmap(buf.data(), static_cast<int>(gw), gw, gh, out);
}

void GlyphAtlas::populate_latin1_fast_table() {
    latin1_.fill(GlyphMetric{});
    ellipsis_ = GlyphMetric{};

    if (ft_face_ == nullptr) {
        for (uint32_t cp = 0x20u; cp <= 0xFFu; ++cp) {
            latin1_[cp].advance_x = fallback_adv_;
            latin1_[cp].valid     = true;
        }
        ellipsis_.advance_x = fallback_adv_ * 2.0f;
        ellipsis_.valid     = true;
        return;
    }

    for (uint32_t cp = 0x20u; cp <= 0xFFu; ++cp) {
        const FT_UInt gid = FT_Get_Char_Index(ft_face_, cp);
        if (gid != 0) {
            GlyphMetric m{};
            if (rasterize_glyph_id(gid, m)) {
                latin1_[cp] = m;
                glyph_cache_[gid] = m;
            }
        }
    }

    const FT_UInt ell_gid = FT_Get_Char_Index(ft_face_, 0x2026u);
    if (ell_gid != 0) {
        if (rasterize_glyph_id(ell_gid, ellipsis_)) {
            glyph_cache_[ell_gid] = ellipsis_;
        }
    }
    if (!ellipsis_.valid) {
        ellipsis_ = latin1_['.'];
        ellipsis_.advance_x = latin1_['.'].advance_x * 3.0f;
    }
    fallback_adv_ = (latin1_['0'].advance_x > 0.0f) ? latin1_['0'].advance_x : fallback_adv_;
}

bool GlyphAtlas::ensure_initialized(float dpr) {
    const float clamped_dpr = (std::max)(1.0f, (std::min)(4.0f, dpr));
    if (initialized_ && std::abs(dpr_ - clamped_dpr) < 0.01f) {
        return true;
    }

    dpr_ = clamped_dpr;
    clear_atlas_pixels();
    (void)load_platform_font(kBaseFontPx * dpr_);
    populate_latin1_fast_table();
    initialized_ = true;
    return true;
}

float GlyphAtlas::baseline_in_row(float row_height_phys) const noexcept {
    const float text_h = ascender_px_ - descender_px_;
    const float top_pad = std::floor((row_height_phys - text_h) * 0.5f);
    return top_pad + ascender_px_;
}

const GlyphMetric& GlyphAtlas::glyph_by_id(uint32_t glyph_id) {
    if (const auto it = glyph_cache_.find(glyph_id); it != glyph_cache_.end()) {
        return it->second;
    }
    GlyphMetric m{};
    (void)rasterize_glyph_id(glyph_id, m);
    auto [inserted, _] = glyph_cache_.emplace(glyph_id, m);
    return inserted->second;
}

const std::vector<ShapedGlyph>& GlyphAtlas::shape_utf8_cached(std::string_view utf8) {
    if (utf8.empty() || hb_font_ == nullptr || hb_buf_ == nullptr) {
        return empty_shaped_;
    }
    if (const auto it = shaped_map_.find(utf8); it != shaped_map_.end()) {
        return shaped_entries_[it->second].glyphs;
    }

    hb_buffer_clear_contents(hb_buf_);
    hb_buffer_add_utf8(hb_buf_, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(hb_buf_);
    hb_shape(hb_font_, hb_buf_, nullptr, 0);

    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(hb_buf_, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(hb_buf_, &count);

    std::vector<ShapedGlyph> run;
    run.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        run.push_back(ShapedGlyph{
            .glyph_id  = infos[i].codepoint,
            .x_offset  = static_cast<float>(pos[i].x_offset) / 64.0f,
            .y_offset  = static_cast<float>(pos[i].y_offset) / 64.0f,
            .x_advance = static_cast<float>(pos[i].x_advance) / 64.0f,
        });
        (void)glyph_by_id(infos[i].codepoint);
    }

    if (shaped_entries_.capacity() < kMaxShapedCache) {
        shaped_entries_.reserve(kMaxShapedCache);
    }

    uint32_t slot = 0;
    if (shaped_entries_.size() < kMaxShapedCache) {
        slot = static_cast<uint32_t>(shaped_entries_.size());
        shaped_entries_.push_back(ShapedCacheEntry{std::string(utf8), std::move(run)});
    } else {
        slot = shaped_clock_hand_;
        shaped_clock_hand_ = (shaped_clock_hand_ + 1u) % static_cast<uint32_t>(kMaxShapedCache);
        shaped_map_.erase(std::string_view{shaped_entries_[slot].key});
        shaped_entries_[slot].key.assign(utf8);
        shaped_entries_[slot].glyphs = std::move(run);
    }
    shaped_map_.emplace(std::string_view{shaped_entries_[slot].key}, slot);
    return shaped_entries_[slot].glyphs;
}

float GlyphAtlas::measure_fast(std::string_view text, uint32_t max_chars) const noexcept {
    if (text.empty()) {
        return 0.0f;
    }
    const float inv_dpr = (dpr_ > 0.0f) ? (1.0f / dpr_) : 1.0f;
    float w_phys = 0.0f;
    uint32_t chars = 0;

    for (size_t i = 0; i < text.size() && chars < max_chars; ++i) {
        const uint8_t b = static_cast<uint8_t>(text[i]);
        if (b < 0x80u) {
            const uint8_t mapped = (b == '\r' || b == '\n' || b == '\t') ? static_cast<uint8_t>(' ') : b;
            w_phys += latin1_[mapped].valid ? latin1_[mapped].advance_x : fallback_adv_;
            ++chars;
        } else if ((b & 0xC0u) != 0x80u) {
            uint32_t cp = 0;
            if ((b & 0xE0u) == 0xC0u && i + 1 < text.size()) {
                cp = ((b & 0x1Fu) << 6u) | (static_cast<uint8_t>(text[i + 1]) & 0x3Fu);
                ++i;
            }
            if (cp >= 0x80u && cp <= 0xFFu && latin1_[cp].valid) {
                w_phys += latin1_[cp].advance_x;
            } else {
                w_phys += fallback_adv_;
            }
            ++chars;
        }
    }
    return w_phys * inv_dpr;
}

GlyphAtlas* glyph_atlas_for_dpr(float dpr) {
    const int key = quantize_dpr_key(dpr);
    DprAtlasTable& tbl = dpr_atlas_table();
    for (size_t i = 0; i < tbl.count; ++i) {
        if (tbl.slots[i].dpr_key == key && tbl.slots[i].atlas != nullptr) {
            return tbl.slots[i].atlas.get();
        }
    }
    size_t idx = tbl.count;
    if (idx < kMaxDprAtlasSlots) {
        ++tbl.count;
    } else {
        idx = kMaxDprAtlasSlots - 1u;
    }
    tbl.slots[idx].dpr_key = key;
    tbl.slots[idx].atlas   = std::make_unique<GlyphAtlas>();
    tbl.slots[idx].atlas->ensure_initialized(static_cast<float>(key) / 100.0f);
    return tbl.slots[idx].atlas.get();
}

size_t cached_glyph_atlas_dpr_count() noexcept {
    return dpr_atlas_table().count;
}

void clear_glyph_atlas_cache() noexcept {
    DprAtlasTable& tbl = dpr_atlas_table();
    for (size_t i = 0; i < tbl.count; ++i) {
        tbl.slots[i].dpr_key = 0;
        tbl.slots[i].atlas.reset();
    }
    tbl.count = 0;
}

} // namespace capybaradb::ui::grid
