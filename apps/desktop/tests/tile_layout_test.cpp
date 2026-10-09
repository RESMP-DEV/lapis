#include "tile_layout.hpp"

#include <QJsonArray>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <tuple>

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
    require(!layout.setRatio(QStringLiteral("0"), 0.5) &&
                !layout.setRatio(QStringLiteral("9"), 0.5),
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

TileLayout tiled(std::initializer_list<std::tuple<const char*, const char*, Edge>> steps) {
    TileLayout layout;
    for (const auto& [id, target, edge] : steps)
        require(layout.place(QString::fromLatin1(id), QString::fromLatin1(target), edge),
                "the fixture tiles");
    return layout;
}

QString near_tile(const TileLayout& layout, const char* id, Edge direction) {
    return layout.neighbor(QString::fromLatin1(id), direction);
}

void directions_follow_the_stage() {
    const auto pair = tiled({{"a", "", Edge::center}, {"b", "a", Edge::right}});
    require(near_tile(pair, "a", Edge::right) == "b" && near_tile(pair, "b", Edge::left) == "a",
            "two side by side reach each other");
    require(near_tile(pair, "a", Edge::left).isEmpty() &&
                near_tile(pair, "b", Edge::right).isEmpty() &&
                near_tile(pair, "a", Edge::top).isEmpty() &&
                near_tile(pair, "a", Edge::bottom).isEmpty(),
            "side by side, nothing is above, below or past the ends: no wraparound");

    const auto stacked = tiled({{"a", "", Edge::center}, {"b", "a", Edge::bottom}});
    require(near_tile(stacked, "a", Edge::bottom) == "b" &&
                near_tile(stacked, "b", Edge::top) == "a" &&
                near_tile(stacked, "a", Edge::right).isEmpty() &&
                near_tile(stacked, "b", Edge::bottom).isEmpty(),
            "two stacked reach each other up and down only");
    require(stacked.readingOrder() == QStringList({"a", "b"}), "stacked reads top first");

    // a over b on the left, c the full height on the right.
    const auto ell =
        tiled({{"a", "", Edge::center}, {"c", "a", Edge::right}, {"b", "a", Edge::bottom}});
    require(near_tile(ell, "a", Edge::right) == "c" && near_tile(ell, "b", Edge::right) == "c",
            "both left tiles reach the tall one");
    require(near_tile(ell, "c", Edge::left) == "a",
            "from the tall tile, left goes to the upper of two equally in-line tiles");
    require(near_tile(ell, "a", Edge::bottom) == "b" && near_tile(ell, "b", Edge::top) == "a" &&
                near_tile(ell, "c", Edge::top).isEmpty() &&
                near_tile(ell, "c", Edge::bottom).isEmpty(),
            "the L moves within its column");
    require(ell.readingOrder() == QStringList({"a", "c", "b"}),
            "reading order is by top edge, then left edge");

    // a b over c d.
    const auto grid = tiled({{"a", "", Edge::center},
                             {"b", "a", Edge::right},
                             {"c", "a", Edge::bottom},
                             {"d", "b", Edge::bottom}});
    require(grid.readingOrder() == QStringList({"a", "b", "c", "d"}),
            "a grid reads row by row, unlike its tree order");
    require(grid.sessions() == QStringList({"a", "c", "b", "d"}), "the tree is column first");
    require(near_tile(grid, "a", Edge::right) == "b" && near_tile(grid, "a", Edge::bottom) == "c" &&
                near_tile(grid, "d", Edge::left) == "c" && near_tile(grid, "d", Edge::top) == "b" &&
                near_tile(grid, "b", Edge::bottom) == "d" &&
                near_tile(grid, "c", Edge::right) == "d",
            "a grid moves one cell at a time");
    require(near_tile(grid, "b", Edge::right).isEmpty() &&
                near_tile(grid, "c", Edge::bottom).isEmpty() &&
                near_tile(grid, "a", Edge::top).isEmpty() &&
                near_tile(grid, "d", Edge::right).isEmpty(),
            "a grid stops at its edges");

    // Uneven: a wide left tile; a short b over a tall c on the right.
    auto uneven =
        tiled({{"a", "", Edge::center}, {"b", "a", Edge::right}, {"c", "b", Edge::bottom}});
    require(uneven.setRatio({}, 0.7) && uneven.setRatio(QStringLiteral("1"), 0.2),
            "the fixture resizes");
    require(near_tile(uneven, "a", Edge::right) == "c",
            "of two tiles sharing the span, the one most in line wins");
    require(near_tile(uneven, "b", Edge::left) == "a" && near_tile(uneven, "c", Edge::left) == "a",
            "a tall tile is beside both");
    // A grid whose rows do not line up: c spans both of the right column's tiles.
    auto offset = grid;
    require(offset.setRatio(QStringLiteral("0"), 0.3) && offset.setRatio(QStringLiteral("1"), 0.7),
            "the fixture misaligns the rows");
    require(near_tile(offset, "c", Edge::right) == "d" && near_tile(offset, "b", Edge::left) == "a",
            "only tiles overlapping the span count, the most in line first");
    require(offset.readingOrder() == QStringList({"a", "b", "c", "d"}),
            "misaligned rows still read by top edge");

    // Three bottom tiles have tops 1.8, 0.9 and 0 microseconds apart while
    // their left edges advance. Epsilon-equivalent tops make this comparator
    // cyclic; exact coordinates keep reading order deterministic.
    const auto leaf = [](const char* id) {
        return QJsonObject{{QStringLiteral("agent"), QString::fromLatin1(id)}};
    };
    const auto row = [&leaf](const char* top, const char* bottom, qreal ratio) {
        return QJsonObject{{QStringLiteral("stacked"), true},
                           {QStringLiteral("ratio"), ratio},
                           {QStringLiteral("children"), QJsonArray{leaf(top), leaf(bottom)}}};
    };
    const QJsonArray right{row("e", "b", 0.5000011), row("f", "c", 0.5000002)};
    const QJsonObject nested{
        {QStringLiteral("stacked"), false},
        {QStringLiteral("ratio"), 1.0 / 3.0},
        {QStringLiteral("children"),
         QJsonArray{row("d", "a", 0.500002), QJsonObject{{QStringLiteral("stacked"), false},
                                                         {QStringLiteral("ratio"), 0.5},
                                                         {QStringLiteral("children"), right}}}}};
    const auto exact = TileLayout::fromJson(nested, {"a", "b", "c", "d", "e", "f"});
    require(exact.readingOrder() == QStringList({"d", "e", "f", "c", "b", "a"}),
            "nearly coincident rows still have a strict order");
}

void next_and_previous_walk_the_tiles_first() {
    const auto home = tiled({{"a", "", Edge::center}, {"c", "a", Edge::right}});
    const QStringList strip{"a", "b", "c", "d"};
    const auto order = home.cycleOrder(strip);
    require(order == QStringList({"a", "c", "b", "d"}),
            "visible tiles in reading order, then untiled agents in strip order");
    require(TileLayout().cycleOrder(strip) == strip, "without tiles the strip order is kept");

    auto step = TileLayout::step(home, order, {}, QStringLiteral("a"), 1);
    require(step.selected == "c" && step.layout.toJson() == home.toJson(),
            "next from the first tile focuses the second without moving tiles");
    step = TileLayout::step(home, order, step.slot, QStringLiteral("c"), 1);
    require(step.selected == "b" && step.slot == "c" &&
                step.layout.readingOrder() == QStringList({"a", "b"}),
            "past the last tile, the focused tile shows the next untiled agent");
    step = TileLayout::step(home, order, step.slot, QStringLiteral("b"), 1);
    require(step.selected == "d" && step.slot == "c" &&
                step.layout.readingOrder() == QStringList({"a", "d"}),
            "the same tile shows each untiled agent in turn");
    step = TileLayout::step(home, order, step.slot, QStringLiteral("d"), 1);
    require(step.selected == "a" && step.slot.isEmpty() && step.layout.toJson() == home.toJson(),
            "wrapping to the tiles restores the stage the walk started from");

    step = TileLayout::step(home, order, {}, QStringLiteral("a"), -1);
    require(step.selected == "d" && step.slot == "a" &&
                step.layout.readingOrder() == QStringList({"d", "c"}),
            "previous from the first tile shows the last untiled agent in that tile");
    step = TileLayout::step(home, order, step.slot, QStringLiteral("d"), -1);
    require(step.selected == "b" && step.layout.readingOrder() == QStringList({"b", "c"}),
            "previous walks the untiled agents backwards in the same tile");
    step = TileLayout::step(home, order, step.slot, QStringLiteral("b"), -1);
    require(step.selected == "c" && step.layout.toJson() == home.toJson(),
            "and back onto the tiles with the stage restored");

    // Two tiles of three agents: every press reaches a different agent.
    const QStringList three_agents{"a", "b", "c"};
    const auto walk = home.cycleOrder(three_agents);
    QString current = QStringLiteral("a");
    QString slot;
    QStringList visited;
    for (int press = 0; press < 3; ++press) {
        step = TileLayout::step(home, walk, slot, current, 1);
        visited.append(step.selected);
        current = step.selected;
        slot = step.slot;
    }
    require(visited == QStringList({"c", "b", "a"}), "a full walk visits each agent once");

    const auto plain = TileLayout::step(TileLayout(), strip, {}, QStringLiteral("d"), 1);
    require(plain.selected == "a" && plain.layout.empty(), "an untiled category walks its strip");
    require(TileLayout::step(home, {}, {}, QStringLiteral("a"), 1).selected.isEmpty(),
            "an empty category has nothing to select");
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
        directions_follow_the_stage();
        next_and_previous_walk_the_tiles_first();
        layouts_are_saved_and_checked();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "tile layout tests passed\n";
    return 0;
}
