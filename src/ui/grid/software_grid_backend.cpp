#include "ui/grid/software_grid_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace capybaradb::ui::grid {
namespace {

inline uint32_t div255_exact(uint32_t c) noexcept {
    return (c + 1u + (c >> 8u)) >> 8u;
}

inline uint32_t rgba_to_argb_premul(uint32_t rgba) noexcept {
    const uint32_t r = (rgba      ) & 0xFFu;
    const uint32_t g = (rgba >>  8) & 0xFFu;
    const uint32_t b = (rgba >> 16) & 0xFFu;
    const uint32_t a = (rgba >> 24) & 0xFFu;
    if (a == 0xFFu) {
        return (0xFFu << 24) | (r << 16) | (g << 8) | b;
    }
    const uint32_t pr = div255_exact(r * a + 127u);
    const uint32_t pg = div255_exact(g * a + 127u);
    const uint32_t pb = div255_exact(b * a + 127u);
    return (a << 24) | (pr << 16) | (pg << 8) | pb;
}

inline uint32_t blend_over_opaque(uint32_t dst_argb, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t alpha) noexcept {
    if (alpha == 0u) {
        return dst_argb;
    }
    if (alpha == 255u) {
        return (0xFFu << 24) | (static_cast<uint32_t>(sr) << 16) | (static_cast<uint32_t>(sg) << 8) | sb;
    }
    const uint32_t dr  = (dst_argb >> 16) & 0xFFu;
    const uint32_t dg  = (dst_argb >>  8) & 0xFFu;
    const uint32_t db  = (dst_argb      ) & 0xFFu;
    const uint32_t inv = 255u - alpha;
    const uint32_t rr  = div255_exact(static_cast<uint32_t>(sr) * alpha + dr * inv + 127u);
    const uint32_t rg  = div255_exact(static_cast<uint32_t>(sg) * alpha + dg * inv + 127u);
    const uint32_t rb  = div255_exact(static_cast<uint32_t>(sb) * alpha + db * inv + 127u);
    return (0xFFu << 24) | (rr << 16) | (rg << 8) | rb;
}

} // namespace

void SoftwareGridBackend::render(
    int viewport_w_phys,
    int viewport_h_phys,
    std::span<const RectInstance> rects,
    std::span<const GlyphInstance> glyphs,
    const GlyphAtlas& atlas) {
    if (viewport_w_phys <= 0 || viewport_h_phys <= 0) {
        return;
    }

    if (image_.width() != viewport_w_phys || image_.height() != viewport_h_phys ||
        image_.format() != QImage::Format_ARGB32_Premultiplied) {
        image_ = QImage(viewport_w_phys, viewport_h_phys, QImage::Format_ARGB32_Premultiplied);
    }

    const int W = viewport_w_phys;
    const int H = viewport_h_phys;
    uint8_t* const bits       = image_.bits();
    const ptrdiff_t row_bytes = static_cast<ptrdiff_t>(image_.bytesPerLine());

    auto row_ptr = [bits, row_bytes](int y) noexcept -> uint32_t* {
        return reinterpret_cast<uint32_t*>(bits + static_cast<ptrdiff_t>(y) * row_bytes);
    };

    // 1. Rasterize RectInstances
    for (const RectInstance& r : rects) {
        const int x0 = (std::max)(0, (std::max)(static_cast<int>(std::floor(r.x)), static_cast<int>(std::ceil(r.clip_x0))));
        const int y0 = (std::max)(0, (std::max)(static_cast<int>(std::floor(r.y)), static_cast<int>(std::ceil(r.clip_y0))));
        const int x1 = (std::min)(W, (std::min)(static_cast<int>(std::floor(r.x + r.w)), static_cast<int>(std::floor(r.clip_x1))));
        const int y1 = (std::min)(H, (std::min)(static_cast<int>(std::floor(r.y + r.h)), static_cast<int>(std::floor(r.clip_y1))));
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }

        const uint32_t argb = rgba_to_argb_premul(r.rgba);

        if (r.style_flags == kRectSolid) {
            for (int y = y0; y < y1; ++y) {
                uint32_t* scan = row_ptr(y);
                std::fill(scan + x0, scan + x1, argb);
            }
        } else if (r.style_flags == kRectFocusDash) {
            const int rx0 = static_cast<int>(std::floor(r.x));
            const int ry0 = static_cast<int>(std::floor(r.y));
            const int rx1 = static_cast<int>(std::floor(r.x + r.w)) - 1;
            const int ry1 = static_cast<int>(std::floor(r.y + r.h)) - 1;

            // Top and bottom edges
            if (ry0 >= y0 && ry0 < y1) {
                uint32_t* scan = row_ptr(ry0);
                for (int x = x0; x < x1; ++x) {
                    if (((x + ry0) & 1) == 0) scan[x] = argb;
                }
            }
            if (ry1 >= y0 && ry1 < y1) {
                uint32_t* scan = row_ptr(ry1);
                for (int x = x0; x < x1; ++x) {
                    if (((x + ry1) & 1) == 0) scan[x] = argb;
                }
            }
            // Left and right edges
            for (int y = y0; y < y1; ++y) {
                uint32_t* scan = row_ptr(y);
                if (rx0 >= x0 && rx0 < x1 && (((rx0 + y) & 1) == 0)) {
                    scan[rx0] = argb;
                }
                if (rx1 >= x0 && rx1 < x1 && (((rx1 + y) & 1) == 0)) {
                    scan[rx1] = argb;
                }
            }
        } else if (r.style_flags == kRectSortAsc || r.style_flags == kRectSortDesc) {
            const float inv_w = 1.0f / (std::max)(1.0f, r.w);
            const float inv_h = 1.0f / (std::max)(1.0f, r.h);
            for (int y = y0; y < y1; ++y) {
                uint32_t* scan = row_ptr(y);
                const float v = (static_cast<float>(y) + 0.5f - r.y) * inv_h;
                const float thresh = (r.style_flags == kRectSortAsc) ? v : (1.0f - v);
                for (int x = x0; x < x1; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f - r.x) * inv_w;
                    if (std::abs(u - 0.5f) * 2.0f <= thresh) {
                        scan[x] = argb;
                    }
                }
            }
        }
    }

    // 2. Rasterize GlyphInstances directly from GlyphAtlas R8 bitmap
    const uint8_t* const atlas_px = atlas.pixels();
    const uint32_t atlas_w        = atlas.width();
    const uint32_t atlas_h        = atlas.height();
    if (atlas_px == nullptr || atlas_w == 0 || atlas_h == 0) {
        return;
    }

    const int atlas_w_i = static_cast<int>(atlas_w);
    const int atlas_h_i = static_cast<int>(atlas_h);

    for (const GlyphInstance& g : glyphs) {
        const int gx0 = static_cast<int>(std::floor(g.x));
        const int gy0 = static_cast<int>(std::floor(g.y));
        const int gw  = static_cast<int>(std::round(g.w));
        const int gh  = static_cast<int>(std::round(g.h));
        if (gw <= 0 || gh <= 0) {
            continue;
        }

        const int src_x0 = (g.sx != 0 || g.sy != 0)
            ? static_cast<int>(g.sx)
            : static_cast<int>(std::round(g.u0 * static_cast<float>(atlas_w)));
        const int src_y0 = (g.sx != 0 || g.sy != 0)
            ? static_cast<int>(g.sy)
            : static_cast<int>(std::round(g.v0 * static_cast<float>(atlas_h)));

        // Pre-clip destination rectangle against viewport, scissor rect, AND atlas bounds
        int x0 = (std::max)(0, (std::max)(gx0, static_cast<int>(std::ceil(g.clip_x0))));
        int y0 = (std::max)(0, (std::max)(gy0, static_cast<int>(std::ceil(g.clip_y0))));
        int x1 = (std::min)(W, (std::min)(gx0 + gw, static_cast<int>(std::floor(g.clip_x1))));
        int y1 = (std::min)(H, (std::min)(gy0 + gh, static_cast<int>(std::floor(g.clip_y1))));

        x0 = (std::max)(x0, gx0 - src_x0);
        y0 = (std::max)(y0, gy0 - src_y0);
        x1 = (std::min)(x1, gx0 - src_x0 + atlas_w_i);
        y1 = (std::min)(y1, gy0 - src_y0 + atlas_h_i);
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }

        const uint8_t sr = static_cast<uint8_t>((g.rgba      ) & 0xFFu);
        const uint8_t sg = static_cast<uint8_t>((g.rgba >>  8) & 0xFFu);
        const uint8_t sb = static_cast<uint8_t>((g.rgba >> 16) & 0xFFu);
        const ptrdiff_t ax_offset = static_cast<ptrdiff_t>(src_x0 - gx0);

        for (int y = y0; y < y1; ++y) {
            const int ay = src_y0 + (y - gy0);
            const uint8_t* const arow = atlas_px + static_cast<size_t>(ay) * atlas_w + ax_offset;
            uint32_t* const scan      = row_ptr(y);
            for (int x = x0; x < x1; ++x) {
                const uint8_t alpha = arow[x];
                if (alpha != 0u) {
                    scan[x] = blend_over_opaque(scan[x], sr, sg, sb, alpha);
                }
            }
        }
    }
}

} // namespace capybaradb::ui::grid
