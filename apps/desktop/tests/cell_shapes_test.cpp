#include "cell_shapes.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::cell_shapes;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool near(qreal a, qreal b) { return std::abs(a - b) < 1e-6; }

// A cell of a lapis row: font height plus line spacing, at a fractional
// width, on a Retina display.
const QRectF kCell(3 * 9.6, 2 * 22.5, 9.6, 22.5);
constexpr qreal kRatio = 2;

void blocks_fill_their_cells() {
    const auto full = cell_shapes(U'█', kCell, kRatio);
    require(full.fills.size() == 1 && near(full.fills[0].rect.top(), 45) &&
                near(full.fills[0].rect.bottom(), 67.5) && near(full.fills[0].rect.left(), 29) &&
                near(full.fills[0].rect.right(), 38.5),
            "a full block fills its cell to the row's edges, on device pixels");
    // The next row's cell starts where this one ends: no seam.
    const auto below = cell_shapes(U'█', kCell.translated(0, 22.5), kRatio);
    require(near(below.fills[0].rect.top(), full.fills[0].rect.bottom()),
            "blocks in neighbouring rows meet");
    const auto next = cell_shapes(U'█', kCell.translated(9.6, 0), kRatio);
    require(near(next.fills[0].rect.left(), full.fills[0].rect.right()),
            "blocks in neighbouring columns meet");
    const auto upper = cell_shapes(U'▀', kCell, kRatio);
    const auto lower = cell_shapes(U'▄', kCell, kRatio);
    require(upper.fills.size() == 1 && lower.fills.size() == 1 &&
                near(upper.fills[0].rect.bottom(), lower.fills[0].rect.top()) &&
                near(upper.fills[0].rect.top(), 45) && near(lower.fills[0].rect.bottom(), 67.5),
            "halves meet in the middle and reach the row's edges");
    const auto eye = cell_shapes(U'▛', kCell, kRatio);
    require(eye.fills.size() == 3, "a quadrant character is its quadrants");
    const auto shade = cell_shapes(U'▒', kCell, kRatio);
    require(shade.fills.size() == 1 && near(shade.fills[0].alpha, 0.5), "shades are translucent");
}

void box_lines_run_through_their_cells() {
    const auto vertical = cell_shapes(U'│', kCell, kRatio);
    require(vertical.fills.size() == 2 && near(vertical.fills[0].rect.top(), 45) &&
                near(vertical.fills[1].rect.bottom(), 67.5),
            "a vertical line reaches the top and bottom of its cell, so rows join");
    const auto heavy = cell_shapes(U'┃', kCell, kRatio);
    require(near(heavy.fills[0].rect.width(), 2 * vertical.fills[0].rect.width()),
            "heavy lines are twice as thick");
    require(cell_shapes(U'║', kCell, kRatio).fills.size() == 4, "double lines are two strokes");
    const auto corner = cell_shapes(U'╭', kCell, kRatio);
    require(corner.fills.empty() && corner.stroke.size() > 4 && corner.width > 0 &&
                near(corner.stroke.front().x(), kCell.right()) &&
                near(corner.stroke.back().y(), kCell.bottom()),
            "a rounded corner runs from the right edge round to the bottom edge");
    require(cell_shapes(U'A', kCell, kRatio).empty() && cell_shapes(U'⣿', kCell, kRatio).empty(),
            "anything else is drawn from the font");
}
} // namespace

int main() {
    try {
        blocks_fill_their_cells();
        box_lines_run_through_their_cells();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "cell shapes tests passed\n";
    return 0;
}
