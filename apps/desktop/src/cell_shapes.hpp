#ifndef LAPIS_DESKTOP_CELL_SHAPES_HPP
#define LAPIS_DESKTOP_CELL_SHAPES_HPP
#include <QPointF>
#include <QRectF>
#include <vector>

namespace lapis::desktop {

// A block element or box-drawing character as shapes filling its cell, as
// terminals draw them: font glyphs for them are shorter than a row (lapis rows
// add line spacing), which leaves lines through logos and borders. The same
// characters and geometry as the iPhone's CellGlyphs.
struct CellShapes {
    struct Fill {
        QRectF rect;
        qreal alpha{1}; // shades are partly transparent
    };
    std::vector<Fill> fills;
    // A rounded corner: a line through these points, `width` wide.
    std::vector<QPointF> stroke;
    qreal width{};
    [[nodiscard]] bool empty() const { return fills.empty() && stroke.empty(); }
};

// The shapes for one code point in `cell`, edges on device pixels (`ratio`
// device pixels to a logical one) so neighbouring cells meet without a seam;
// empty for any character drawn from the font.
[[nodiscard]] CellShapes cell_shapes(char32_t code_point, const QRectF& cell, qreal ratio);
} // namespace lapis::desktop
#endif
