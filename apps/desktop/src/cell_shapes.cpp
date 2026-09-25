#include "cell_shapes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>

namespace lapis::desktop {
namespace {
// Edges on device pixels, so cells that meet leave no seam.
QRectF snapped(const QRectF& rect, qreal ratio) {
    const auto snap = [ratio](qreal value) { return std::round(value * ratio) / ratio; };
    const qreal left = snap(rect.left());
    const qreal top = snap(rect.top());
    const qreal right = snap(rect.right());
    const qreal bottom = snap(rect.bottom());
    return {left, top, std::max(right - left, 1 / ratio), std::max(bottom - top, 1 / ratio)};
}

// Arms up, right, down, left: 0 none, 1 light, 2 heavy, 3 double.
struct Arms {
    char32_t code;
    std::uint8_t up, right, down, left;
};
constexpr std::array<Arms, 55> kArms{{
    {0x2500, 0, 1, 0, 1},
    {0x2501, 0, 2, 0, 2},
    {0x2502, 1, 0, 1, 0},
    {0x2503, 2, 0, 2, 0},
    {0x250C, 0, 1, 1, 0},
    {0x250F, 0, 2, 2, 0},
    {0x2510, 0, 0, 1, 1},
    {0x2513, 0, 0, 2, 2},
    {0x2514, 1, 1, 0, 0},
    {0x2517, 2, 2, 0, 0},
    {0x2518, 1, 0, 0, 1},
    {0x251B, 2, 0, 0, 2},
    {0x251C, 1, 1, 1, 0},
    {0x2523, 2, 2, 2, 0},
    {0x2524, 1, 0, 1, 1},
    {0x252B, 2, 0, 2, 2},
    {0x252C, 0, 1, 1, 1},
    {0x2533, 0, 2, 2, 2},
    {0x2534, 1, 1, 0, 1},
    {0x253B, 2, 2, 0, 2},
    {0x253C, 1, 1, 1, 1},
    {0x254B, 2, 2, 2, 2},
    {0x2550, 0, 3, 0, 3},
    {0x2551, 3, 0, 3, 0},
    {0x2554, 0, 3, 3, 0},
    {0x2557, 0, 0, 3, 3},
    {0x255A, 3, 3, 0, 0},
    {0x255D, 3, 0, 0, 3},
    {0x2560, 3, 3, 3, 0},
    {0x2563, 3, 0, 3, 3},
    {0x2566, 0, 3, 3, 3},
    {0x2569, 3, 3, 0, 3},
    {0x256C, 3, 3, 3, 3},
    {0x2574, 0, 0, 0, 1},
    {0x2575, 1, 0, 0, 0},
    {0x2576, 0, 1, 0, 0},
    {0x2577, 0, 0, 1, 0},
    {0x2578, 0, 0, 0, 2},
    {0x2579, 2, 0, 0, 0},
    {0x257A, 0, 2, 0, 0},
    {0x257B, 0, 0, 2, 0},
    // Dashed lines are drawn solid.
    {0x2504, 0, 1, 0, 1},
    {0x2505, 0, 2, 0, 2},
    {0x2506, 1, 0, 1, 0},
    {0x2507, 2, 0, 2, 0},
    {0x2508, 0, 1, 0, 1},
    {0x2509, 0, 2, 0, 2},
    {0x250A, 1, 0, 1, 0},
    {0x250B, 2, 0, 2, 0},
    {0x254C, 0, 1, 0, 1},
    {0x254D, 0, 2, 0, 2},
    {0x254E, 1, 0, 1, 0},
    {0x254F, 2, 0, 2, 0},
    // Heavy and light mixes that fonts often lack are drawn as their weights.
    {0x257C, 0, 2, 0, 1},
    {0x257E, 0, 1, 0, 2},
}};

std::optional<Arms> arms_of(char32_t code) {
    const auto found = std::find_if(kArms.begin(), kArms.end(),
                                    [code](const Arms& arms) { return arms.code == code; });
    return found == kArms.end() ? std::nullopt : std::optional<Arms>(*found);
}

qreal light_width(const QRectF& cell, qreal ratio) {
    return std::max(1 / ratio, std::round(cell.width() * 0.12 * ratio) / ratio);
}

void blocks(char32_t code, const QRectF& cell, qreal ratio, CellShapes& shapes) {
    const qreal w = cell.width();
    const qreal h = cell.height();
    const auto fill = [&](qreal x, qreal y, qreal width, qreal height, qreal alpha = 1) {
        shapes.fills.push_back(
            {snapped(QRectF(cell.left() + x, cell.top() + y, width, height), ratio), alpha});
    };
    if (code == 0x2580) {
        fill(0, 0, w, h / 2);
    } else if (code >= 0x2581 && code <= 0x2588) {
        const qreal part = h * static_cast<qreal>(code - 0x2580) / 8;
        fill(0, h - part, w, part);
    } else if (code >= 0x2589 && code <= 0x258F) {
        fill(0, 0, w * static_cast<qreal>(0x2590 - code) / 8, h);
    } else if (code == 0x2590) {
        fill(w / 2, 0, w / 2, h);
    } else if (code >= 0x2591 && code <= 0x2593) {
        fill(0, 0, w, h, 0.25 * static_cast<qreal>(code - 0x2590));
    } else if (code == 0x2594) {
        fill(0, 0, w, h / 8);
    } else if (code == 0x2595) {
        fill(w * 7 / 8, 0, w / 8, h);
    } else {
        // Quadrants, as upper left, upper right, lower left, lower right bits.
        constexpr std::array<std::uint8_t, 10> quadrants{0b0010, 0b0001, 0b1000, 0b1011, 0b1001,
                                                         0b1110, 0b1101, 0b0100, 0b0110, 0b0111};
        const auto bits = quadrants.at(code - 0x2596);
        if ((bits & 0b1000) != 0)
            fill(0, 0, w / 2, h / 2);
        if ((bits & 0b0100) != 0)
            fill(w / 2, 0, w / 2, h / 2);
        if ((bits & 0b0010) != 0)
            fill(0, h / 2, w / 2, h / 2);
        if ((bits & 0b0001) != 0)
            fill(w / 2, h / 2, w / 2, h / 2);
    }
}

// Each arm runs from its far edge to past the centre, so joints close.
void box(const Arms& arms, const QRectF& cell, qreal ratio, CellShapes& shapes) {
    const qreal light = light_width(cell, ratio);
    const qreal cx = cell.center().x();
    const qreal cy = cell.center().y();
    const auto thickness = [light](std::uint8_t weight) { return weight == 2 ? light * 2 : light; };
    const auto offsets = [](std::uint8_t weight, qreal t) {
        return weight == 3 ? std::vector<qreal>{-t * 1.5, t * 0.5} : std::vector<qreal>{-t / 2};
    };
    // An arm between two points along its line, `weight` wide.
    struct Arm {
        qreal from;
        qreal to;
        std::uint8_t weight;
    };
    const auto horizontal = [&](const Arm& arm) {
        if (arm.weight == 0)
            return;
        const qreal t = thickness(arm.weight);
        for (const qreal offset : offsets(arm.weight, t))
            shapes.fills.push_back({snapped(QRectF(std::min(arm.from, arm.to), cy + offset,
                                                   std::abs(arm.to - arm.from), t),
                                            ratio),
                                    1});
    };
    const auto vertical = [&](const Arm& arm) {
        if (arm.weight == 0)
            return;
        const qreal t = thickness(arm.weight);
        for (const qreal offset : offsets(arm.weight, t))
            shapes.fills.push_back({snapped(QRectF(cx + offset, std::min(arm.from, arm.to), t,
                                                   std::abs(arm.to - arm.from)),
                                            ratio),
                                    1});
    };
    const qreal reach = light * 2;
    horizontal({cell.left(), cx + (arms.left > 0 ? reach : 0), arms.left});
    horizontal({cx - (arms.right > 0 ? reach : 0), cell.right(), arms.right});
    vertical({cell.top(), cy + (arms.up > 0 ? reach : 0), arms.up});
    vertical({cy - (arms.down > 0 ? reach : 0), cell.bottom(), arms.down});
}

// Rounded corners ╭ ╮ ╯ ╰: a quarter circle joining the middle of the side
// it opens to and the middle of the edge it leaves by, on the same pixel
// lines as the straight lines they meet.
void rounded(char32_t code, const QRectF& cell, qreal ratio, CellShapes& shapes) {
    const qreal light = light_width(cell, ratio);
    const qreal cx =
        snapped(QRectF(cell.center().x() - light / 2, cell.top(), light, 1), ratio).center().x();
    const qreal cy =
        snapped(QRectF(cell.left(), cell.center().y() - light / 2, 1, light), ratio).center().y();
    const qreal hx = code == 0x256D || code == 0x2570 ? 1 : -1; // towards the right edge
    const qreal vy = code == 0x256D || code == 0x256E ? 1 : -1; // towards the bottom edge
    const qreal radius = std::min(cell.width(), cell.height()) / 2;
    const QPointF centre(cx + hx * radius, cy + vy * radius);
    shapes.stroke.emplace_back(hx > 0 ? cell.right() : cell.left(), cy);
    const qreal from = std::atan2(-vy, 0);
    const qreal to = std::atan2(0, -hx);
    // The short way round, a quarter turn.
    const qreal span = std::remainder(to - from, 2 * std::numbers::pi);
    constexpr int kSteps = 12;
    for (int step = 0; step <= kSteps; ++step) {
        const qreal angle = from + span * step / kSteps;
        shapes.stroke.emplace_back(centre.x() + radius * std::cos(angle),
                                   centre.y() + radius * std::sin(angle));
    }
    shapes.stroke.emplace_back(cx, vy > 0 ? cell.bottom() : cell.top());
    shapes.width = light;
}
} // namespace

CellShapes cell_shapes(char32_t code_point, const QRectF& cell, qreal ratio) {
    CellShapes shapes;
    if (ratio <= 0 || cell.isEmpty())
        return shapes;
    if (code_point >= 0x2580 && code_point <= 0x259F)
        blocks(code_point, cell, ratio, shapes);
    else if (const auto arms = arms_of(code_point))
        box(*arms, cell, ratio, shapes);
    else if (code_point >= 0x256D && code_point <= 0x2570)
        rounded(code_point, cell, ratio, shapes);
    return shapes;
}
} // namespace lapis::desktop
