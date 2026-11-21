#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct FT_LibraryRec_;
struct FT_FaceRec_;
struct hb_font_t;
struct hb_buffer_t;

namespace capybaradb::ui::grid {

struct GlyphMetric {
    float    advance_x = 0.0f;
    float    bearing_x = 0.0f;
    float    bearing_y = 0.0f;
    float    w         = 0.0f;
    float    h         = 0.0f;
    float    u0        = 0.0f;
    float    v0        = 0.0f;
    float    u1        = 0.0f;
    float    v1        = 0.0f;
    uint16_t sx        = 0;
    uint16_t sy        = 0;
    bool     valid     = false;
};

struct ShapedGlyph {
    uint32_t glyph_id  = 0;
    float    x_offset  = 0.0f;
    float    y_offset  = 0.0f;
    float    x_advance = 0.0f;
};

/// FreeType 2 + HarfBuzz R8 texture atlas with on-demand dynamic growth (512 -> 1024 -> 2048),
/// O(1) Latin-1 fast-path, and flat 1,024-entry ring HarfBuzz shaping cache for complex/non-ASCII text.
class GlyphAtlas final {
public:
    static constexpr uint32_t kInitialAtlasSize = 512u;
    static constexpr uint32_t kMaxAtlasSize     = 2048u;
    static constexpr uint32_t kAtlasSize        = kMaxAtlasSize;
    static constexpr size_t   kMaxShapedCache   = 1024u;
    static constexpr float    kBaseFontPx       = 12.0f;

    GlyphAtlas();
    ~GlyphAtlas() noexcept;

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    /// Initializes or rebuilds the atlas for the given device pixel ratio (1.0, 1.25, 1.5, 2.0...).
    /// No-op if already initialized at this DPR.
    bool ensure_initialized(float dpr = 1.0f);

    [[nodiscard]] bool     is_ready() const noexcept { return initialized_; }
    [[nodiscard]] float    dpr() const noexcept { return dpr_; }
    [[nodiscard]] float    ascender_px() const noexcept { return ascender_px_; }
    [[nodiscard]] float    descender_px() const noexcept { return descender_px_; }
    [[nodiscard]] float    baseline_in_row(float row_height_phys) const noexcept;
    [[nodiscard]] uint64_t atlas_version() const noexcept { return atlas_version_; }

    [[nodiscard]] const uint8_t* pixels() const noexcept { return atlas_pixels_.data(); }
    [[nodiscard]] uint32_t       width() const noexcept { return atlas_size_; }
    [[nodiscard]] uint32_t       height() const noexcept { return atlas_size_; }
    [[nodiscard]] size_t         byte_size() const noexcept { return atlas_pixels_.size(); }

    /// Fast-path O(1) lookup for Latin-1 bytes (0x00..0xFF) in physical pixels.
    [[nodiscard]] const GlyphMetric& latin1(uint8_t byte) const noexcept { return latin1_[byte]; }

    /// Pre-rasterized horizontal ellipsis ('…', U+2026) metric in physical pixels.
    [[nodiscard]] const GlyphMetric& ellipsis() const noexcept { return ellipsis_; }

    /// Rasterizes/fetches metric for an arbitrary FreeType glyph index (for HarfBuzz runs).
    [[nodiscard]] const GlyphMetric& glyph_by_id(uint32_t glyph_id);

    /// Shapes a UTF-8 non-ASCII run via HarfBuzz with flat 1,024-entry ring caching (zero std::list nodes).
    [[nodiscard]] const std::vector<ShapedGlyph>& shape_utf8_cached(std::string_view utf8);

    /// Fast text width measurement in logical DIPs (zero heap allocation).
    [[nodiscard]] float measure_fast(std::string_view text, uint32_t max_chars = 128u) const noexcept;

    /// Synthetic test helper to pack a custom bitmap block (gw x gh) and exercise shelf growth (512 -> 1024 -> 2048).
    bool pack_custom_rect_for_test(uint32_t gw, uint32_t gh, uint8_t fill_value, GlyphMetric& out);

private:
    bool load_platform_font(float pixel_size);
    bool rasterize_glyph_id(uint32_t glyph_id, GlyphMetric& out);
    bool pack_bitmap(const uint8_t* src_buf, int src_pitch, uint32_t gw, uint32_t gh, GlyphMetric& out);
    bool grow_atlas(uint32_t new_size);
    void populate_latin1_fast_table();
    void clear_atlas_pixels();
    void destroy_freetype() noexcept;

    FT_LibraryRec_* ft_lib_  = nullptr;
    FT_FaceRec_*    ft_face_ = nullptr;
    hb_font_t*      hb_font_ = nullptr;
    hb_buffer_t*    hb_buf_  = nullptr;

    bool     initialized_   = false;
    float    dpr_           = 0.0f;
    float    ascender_px_   = 10.0f;
    float    descender_px_  = -3.0f;
    float    fallback_adv_  = 7.0f;
    uint64_t atlas_version_ = 0;

    uint32_t             atlas_size_ = 0;
    std::vector<uint8_t> atlas_pixels_;
    uint32_t             shelf_x_ = 1;
    uint32_t             shelf_y_ = 1;
    uint32_t             shelf_h_ = 0;

    std::array<GlyphMetric, 256> latin1_{};
    GlyphMetric                  ellipsis_{};
    std::unordered_map<uint32_t, GlyphMetric> glyph_cache_;

    struct ShapedCacheEntry {
        std::string              key;
        std::vector<ShapedGlyph> glyphs;
    };
    std::vector<ShapedCacheEntry>               shaped_entries_;
    uint32_t                                    shaped_clock_hand_ = 0;
    std::unordered_map<std::string_view, uint32_t> shaped_map_;
    std::vector<ShapedGlyph>                    empty_shaped_;
};

/// Shared per-DPR GlyphAtlas cache for the UI thread.
/// Eliminates duplicate 4 MB atlas buffers and FreeType/HarfBuzz instances across open query tabs.
[[nodiscard]] GlyphAtlas* glyph_atlas_for_dpr(float dpr = 1.0f);
[[nodiscard]] size_t cached_glyph_atlas_dpr_count() noexcept;
void clear_glyph_atlas_cache() noexcept;

} // namespace capybaradb::ui::grid
