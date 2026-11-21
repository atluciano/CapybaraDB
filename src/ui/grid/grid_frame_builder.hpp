#pragma once

#include "core/result/result_set_storage.hpp"
#include "ui/grid/glyph_atlas.hpp"
#include "ui/grid/grid_view_state.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace capybaradb::ui::grid {

enum RectStyleFlags : uint32_t {
    kRectSolid      = 0u,
    kRectFocusDash  = 1u,
    kRectSortAsc    = 2u,
    kRectSortDesc   = 3u,
};

[[nodiscard]] constexpr uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFFu) noexcept {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(b) << 16u) |
           (static_cast<uint32_t>(a) << 24u);
}

// Exact PARITY_SPEC.md §1.2 color tokens packed as RGBA8
inline constexpr uint32_t kColGridCellBg      = pack_rgba(0xF5, 0xF5, 0xF5);
inline constexpr uint32_t kColGridNullBg      = pack_rgba(0xFD, 0xFB, 0xAC);
inline constexpr uint32_t kColGridSelectionBg = pack_rgba(0xA5, 0xCF, 0xF1);
inline constexpr uint32_t kColGridLine        = pack_rgba(0xCC, 0xCE, 0xDB);
inline constexpr uint32_t kColGridHeaderBg    = pack_rgba(0xF0, 0xF0, 0xF0);
inline constexpr uint32_t kColGridTopLeftBg   = pack_rgba(0xE1, 0xE1, 0xE1);
inline constexpr uint32_t kColGridHeaderDiv   = pack_rgba(0xD0, 0xD0, 0xD0);
inline constexpr uint32_t kColGridOuterBorder = pack_rgba(0xAB, 0xAD, 0xB3);
inline constexpr uint32_t kColGridTextFg      = pack_rgba(0x1E, 0x1E, 0x1E);
inline constexpr uint32_t kColGridLinkFg      = pack_rgba(0x00, 0x66, 0xCC);
inline constexpr uint32_t kColGridSortArrow   = pack_rgba(0x71, 0x71, 0x71);
inline constexpr uint32_t kColGridFocusRect   = pack_rgba(0x00, 0x00, 0x00);

inline constexpr float kRowHeightDip          = 19.0f;
inline constexpr float kColHeaderHeightDip    = 19.0f;
inline constexpr float kMinRowHeaderWidthDip  = 32.0f;

struct alignas(16) RectInstance {
    float    x, y, w, h;                         // location 0: physical pixels
    float    clip_x0, clip_y0, clip_x1, clip_y1; // location 1: physical scissor rect
    uint32_t rgba;                               // location 2: packed RGBA8
    uint32_t style_flags;                        // location 3: RectStyleFlags
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};
static_assert(sizeof(RectInstance) == 48 && alignof(RectInstance) == 16);

struct alignas(16) GlyphInstance {
    float    x, y, w, h;                         // location 0: physical pixels
    float    u0, v0, u1, v1;                     // location 1: atlas UV
    float    clip_x0, clip_y0, clip_x1, clip_y1; // location 2: physical scissor rect
    uint32_t rgba;                               // location 3: packed RGBA8
    uint16_t sx   = 0;                           // integer atlas pixel X (immune to atlas resize)
    uint16_t sy   = 0;                           // integer atlas pixel Y (immune to atlas resize)
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};
static_assert(sizeof(GlyphInstance) == 64 && alignof(GlyphInstance) == 16);

struct GridFrameInput {
    const core::ResultSetStorage* storage             = nullptr;
    GridViewState*                view_state          = nullptr;
    int                           viewport_w_dip      = 0;
    int                           viewport_h_dip      = 0;
    float                         dpr                 = 1.0f;
    float                         row_header_w_dip    = kMinRowHeaderWidthDip;
    bool                          display_json_as_link = true;
    bool                          draw_outer_border   = false;
};

/// Computes dynamic row header width: max(32, 10 + 7 * digits(max_row_number)).
[[nodiscard]] float compute_row_header_width_dip(uint64_t total_rows) noexcept;

/// Immediate-mode frame builder that translates visible ResultSetStorage cells into
/// flat RectInstance[] and GlyphInstance[] buffers with ZERO heap allocations per frame.
class GridFrameBuilder final {
public:
    GridFrameBuilder();

    void build(const GridFrameInput& input, GlyphAtlas& atlas);

    [[nodiscard]] std::span<const RectInstance>  rects() const noexcept { return rects_; }
    [[nodiscard]] std::span<const GlyphInstance> glyphs() const noexcept { return glyphs_; }

private:
    void push_rect(
        float x, float y, float w, float h,
        uint32_t rgba,
        uint32_t style_flags,
        float cx0, float cy0, float cx1, float cy1);

    float emit_text_fast(
        GlyphAtlas& atlas,
        std::string_view text,
        uint8_t cell_flags,
        float x_phys,
        float baseline_phys,
        float avail_w_phys,
        uint32_t rgba,
        float cx0, float cy0, float cx1, float cy1);

    std::vector<RectInstance>  rects_;
    std::vector<GlyphInstance> glyphs_;
    std::string                non_ascii_scratch_;
};

} // namespace capybaradb::ui::grid
