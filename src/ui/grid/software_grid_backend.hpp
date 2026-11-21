#pragma once

#include "ui/grid/glyph_atlas.hpp"
#include "ui/grid/grid_frame_builder.hpp"

#include <QImage>
#include <span>

namespace capybaradb::ui::grid {

/// Deterministic CPU rasterizer over QImage(Format_ARGB32_Premultiplied) that consumes
/// the RectInstance[] and GlyphInstance[] buffers and GlyphAtlas R8 bitmap.
class SoftwareGridBackend final {
public:
    void render(
        int viewport_w_phys,
        int viewport_h_phys,
        std::span<const RectInstance> rects,
        std::span<const GlyphInstance> glyphs,
        const GlyphAtlas& atlas);

    [[nodiscard]] const QImage& image() const noexcept { return image_; }

private:
    QImage image_;
};

} // namespace capybaradb::ui::grid
