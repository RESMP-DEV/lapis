#include "tile_layout.hpp"

#include <QJsonArray>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::TileLayout;
using Edge = TileLayout::Edge;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool near(qreal a, qreal b) { return std::abs(a - b) < 1e-9; }

const QRectF kUnit(0, 0, 1, 1);

// a on the left, b over c on the right.
TileLayout three() {
    TileLayout layout;
    require(layout.place(QStringLiteral("a"), {}, Edge::center), "the first agent needs no target");
    require(layout.place(QStringLiteral("b"), QStringLiteral("a"), Edge::right),
            "an agent goes beside another");
    require(layout.place(QStringLiteral("c"), QStringLiteral("b"), Edge::bottom),
            "an agent goes below another");
    return layout;
}

void splits_place_agents_beside_each_other() {
    const auto layout = three();
    require(layout.sessions() == QStringList({"a", "b", "c"}), "reading order follows the splits");
    const auto tiles = layout.tiles(kUnit);
    require(tiles.size() == 3 && near(tiles[0].rect.width(), 0.5) &&
                near(tiles[0].rect.height(), 1) && near(tiles[1].rect.left(), 0.5) &&
                near(tiles[1].rect.height(), 0.5) && near(tiles[2].rect.top(), 0.5),
            "halves side by side, then stacked");
    const auto dividers = layout.dividers(kUnit);
    require(dividers.size() == 2 && dividers[0].path.isEmpty() && !dividers[0].stacked &&
                near(dividers[0].line.left(), 0.5) && dividers[1].path == QStringLiteral("1") &&
                dividers[1].stacked && near(dividers[1].line.top(), 0.5) &&
                near(dividers[1].area.left(), 0.5),
            "each split has a divider across its own area");
    auto left = TileLayout();
    left.place(QStringLiteral("a"), {}, Edge::center);
    left.place(QStringLiteral("b"), QStringLiteral("a"), Edge::left);
    require(left.sessions() == QStringList({"b", "a"}), "placing on the left puts it first");
    require(!left.place(QStringLiteral("a"), QStringLiteral("a"), Edge::right),
            "an agent cannot split its own tile");
    require(!left.place(QStringLiteral("z"), QStringLiteral("missing"), Edge::right),
            "the target must be on the stage");
}

void tiles_move_swap_and_leave() {
    auto layout = three();
    require(layout.place(QStringLiteral("a"), QStringLiteral("c"), Edge::right) &&
                layout.count() == 3 && layout.sessions() == QStringList({"b", "c", "a"}),
            "a tiled agent moves instead of appearing twice");
    require(layout.replace(QStringLiteral("b"), QStringLiteral("a")) &&
                layout.sessions() == QStringList({"a", "c", "b"}),
            "dropping on a tile's center swaps two tiled agents");
    require(layout.replace(QStringLiteral("c"), QStringLiteral("d")) && !layout.contains("c") &&
                layout.contains("d"),
            "an untiled agent takes the tile's place");
    require(layout.remove(QStringLiteral("d")) && layout.count() == 2 &&
                near(layout.tiles(kUnit)[0].rect.height(), 0.5),
            "a removed tile's sibling takes its space");
    require(layout.remove(QStringLiteral("a")) && layout.count() == 1 &&
                layout.remove(QStringLiteral("b")) && layout.empty(),
            "the last tile empties the layout");
}

void ratios_neighbors_and_limits() {
    auto layout = three();
    require(layout.setRatio({}, 0.7) && near(layout.tiles(kUnit)[0].rect.width(), 0.7),
            "the root divider moves");
    require(layout.setRatio({}, 0.99) && near(layout.tiles(kUnit)[0].rect.width(), 0.85),
            "a divider stops before a tile vanishes");
    require(!layout.setRatio(QStringLiteral("0"), 0.5) && !layout.setRatio(QStringLiteral("9"), 0.5),
            "only splits have ratios");
    require(layout.neighbor(QStringLiteral("a"), Edge::right) == QStringLiteral("b"),
            "right of a full-height tile is the upper one, most in line");
    require(layout.neighbor(QStringLiteral("c"), Edge::left) == QStringLiteral("a") &&
                layout.neighbor(QStringLiteral("b"), Edge::bottom) == QStringLiteral("c") &&
                layout.neighbor(QStringLiteral("c"), Edge::top) == QStringLiteral("b") &&
                layout.neighbor(QStringLiteral("a"), Edge::left).isEmpty(),
            "focus moves to the tile beside it");
    TileLayout full;
    full.place(QStringLiteral("0"), {}, Edge::center);
    for (int i = 1; i < TileLayout::kMaximumTiles; ++i)
        require(full.place(QString::number(i), QString::number(i - 1), Edge::right),
                "up to the limit, agents tile");
    require(!full.place(QStringLiteral("extra"), QStringLiteral("0"), Edge::bottom),
            "the stage holds at most eight tiles");
}

void layouts_are_saved_and_checked() {
    const auto layout = three();
    const auto json = layout.toJson();
    const auto again = TileLayout::fromJson(json, {"a", "b", "c"});
    require(again.sessions() == layout.sessions() && again.toJson() == json,
            "a layout comes back as saved");
    require(TileLayout::fromJson(json, {"a", "c"}).sessions() == QStringList({"a", "c"}),
            "an agent that is gone leaves the layout");
    auto repeated = json;
    auto children = repeated.value(QStringLiteral("children")).toArray();
    children[0] = QJsonObject{{"agent", "b"}};
    repeated.insert(QStringLiteral("children"), children);
    require(TileLayout::fromJson(repeated, {"a", "b", "c"}).count() == 2,
            "an agent tiled twice keeps one tile");
    require(TileLayout::fromJson(QJsonObject{{"children", QJsonArray{}}}, {"a"}).empty() &&
                TileLayout::fromJson({}, {"a"}).empty(),
            "a malformed layout is dropped");
}
} // namespace

int main() {
    try {
        splits_place_agents_beside_each_other();
        tiles_move_swap_and_leave();
        ratios_neighbors_and_limits();
        layouts_are_saved_and_checked();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "tile layout tests passed\n";
    return 0;
}
