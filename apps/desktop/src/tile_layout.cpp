#include "tile_layout.hpp"

#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <utility>

namespace lapis::desktop {
namespace {
using tile_detail::Node;
constexpr qreal kMinimumRatio = 0.15;
constexpr qreal kMaximumRatio = 0.85;
constexpr int kMaximumDepth = 16;
constexpr qreal kEpsilon = 1e-6;

// The tile holding `id`, for a Node or a const Node.
template <typename Tree> Tree* find(Tree& node, const QString& id) {
    if (node.children.empty())
        return node.session == id ? &node : nullptr;
    for (auto& child : node.children)
        if (auto* found = find(child, id))
            return found;
    return nullptr;
}

// The split whose child is the tile `id`, and that child's index.
std::pair<Node*, std::size_t> parent_of(Node& node, const QString& id) {
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        auto& child = node.children[index];
        if (child.children.empty() && child.session == id)
            return {&node, index};
        if (auto found = parent_of(child, id); found.first)
            return found;
    }
    return {nullptr, 0};
}

Node* at_path(Node& node, QStringView path) {
    Node* current = &node;
    for (const auto step : path) {
        const auto index = step == u'0' ? 0U : step == u'1' ? 1U : 2U;
        if (index > 1 || current->children.size() != 2)
            return nullptr;
        current = &current->children[index];
    }
    return current;
}

std::pair<QRectF, QRectF> halves(const Node& node, const QRectF& area) {
    if (node.stacked) {
        const qreal first = area.height() * node.ratio;
        return {QRectF(area.left(), area.top(), area.width(), first),
                QRectF(area.left(), area.top() + first, area.width(), area.height() - first)};
    }
    const qreal first = area.width() * node.ratio;
    return {QRectF(area.left(), area.top(), first, area.height()),
            QRectF(area.left() + first, area.top(), area.width() - first, area.height())};
}

void collect(const Node& node, QStringList& sessions) {
    if (node.children.empty()) {
        sessions.append(node.session);
        return;
    }
    for (const auto& child : node.children)
        collect(child, sessions);
}

void lay_out(const Node& node, const QRectF& area, std::vector<TileLayout::Tile>& tiles) {
    if (node.children.empty()) {
        tiles.push_back({node.session, area});
        return;
    }
    const auto [first, second] = halves(node, area);
    lay_out(node.children[0], first, tiles);
    lay_out(node.children[1], second, tiles);
}

void lay_out_dividers(const Node& node, const QRectF& area, const QString& path,
                      std::vector<TileLayout::Divider>& dividers) {
    if (node.children.empty())
        return;
    const auto [first, second] = halves(node, area);
    const QRectF line = node.stacked ? QRectF(area.left(), second.top(), area.width(), 0)
                                     : QRectF(second.left(), area.top(), 0, area.height());
    dividers.push_back({path, node.stacked, line, area});
    lay_out_dividers(node.children[0], first, path + QLatin1Char('0'), dividers);
    lay_out_dividers(node.children[1], second, path + QLatin1Char('1'), dividers);
}

QJsonObject to_json(const Node& node) {
    if (node.children.empty())
        return {{QStringLiteral("agent"), node.session}};
    return {{QStringLiteral("stacked"), node.stacked},
            {QStringLiteral("ratio"), node.ratio},
            {QStringLiteral("children"),
             QJsonArray{to_json(node.children[0]), to_json(node.children[1])}}};
}

// Unknown or repeated agents come back as empty tiles, removed afterwards.
std::optional<Node> from_json(const QJsonObject& json, const QSet<QString>& known,
                              QSet<QString>& seen, int depth) {
    if (depth > kMaximumDepth)
        return std::nullopt;
    if (const auto agent = json.value(QStringLiteral("agent")); agent.isString()) {
        auto id = agent.toString();
        if (!known.contains(id) || seen.contains(id))
            id.clear();
        seen.insert(id);
        return Node{.session = id, .stacked = false, .ratio = 0.5, .children = {}};
    }
    const auto children = json.value(QStringLiteral("children")).toArray();
    const auto ratio = json.value(QStringLiteral("ratio")).toDouble(0.5);
    if (children.size() != 2 || !std::isfinite(ratio))
        return std::nullopt;
    auto first = from_json(children[0].toObject(), known, seen, depth + 1);
    auto second = from_json(children[1].toObject(), known, seen, depth + 1);
    if (!first || !second)
        return std::nullopt;
    Node split{.session = {},
               .stacked = json.value(QStringLiteral("stacked")).toBool(),
               .ratio = std::clamp(ratio, kMinimumRatio, kMaximumRatio),
               .children = {}};
    split.children.push_back(std::move(*first));
    split.children.push_back(std::move(*second));
    return split;
}

bool beside(const QRectF& from, const QRectF& to, TileLayout::Edge direction) {
    const bool across = direction == TileLayout::Edge::left || direction == TileLayout::Edge::right;
    const qreal overlap = across ? std::min(from.bottom(), to.bottom()) - std::max(from.top(), to.top())
                                 : std::min(from.right(), to.right()) - std::max(from.left(), to.left());
    if (overlap <= kEpsilon)
        return false;
    switch (direction) {
    case TileLayout::Edge::left:
        return to.right() <= from.left() + kEpsilon;
    case TileLayout::Edge::right:
        return to.left() >= from.right() - kEpsilon;
    case TileLayout::Edge::top:
        return to.bottom() <= from.top() + kEpsilon;
    case TileLayout::Edge::bottom:
        return to.top() >= from.bottom() - kEpsilon;
    case TileLayout::Edge::center:
        break;
    }
    return false;
}

qreal gap(const QRectF& from, const QRectF& to, TileLayout::Edge direction) {
    switch (direction) {
    case TileLayout::Edge::left:
        return from.left() - to.right();
    case TileLayout::Edge::right:
        return to.left() - from.right();
    case TileLayout::Edge::top:
        return from.top() - to.bottom();
    case TileLayout::Edge::bottom:
        return to.top() - from.bottom();
    case TileLayout::Edge::center:
        break;
    }
    return 0;
}
} // namespace

std::optional<TileLayout::Edge> TileLayout::edge(const QString& name) {
    if (name == QLatin1String("left"))
        return Edge::left;
    if (name == QLatin1String("right"))
        return Edge::right;
    if (name == QLatin1String("top"))
        return Edge::top;
    if (name == QLatin1String("bottom"))
        return Edge::bottom;
    if (name == QLatin1String("center"))
        return Edge::center;
    return std::nullopt;
}

int TileLayout::count() const { return static_cast<int>(sessions().size()); }

QStringList TileLayout::sessions() const {
    QStringList result;
    if (root_)
        collect(*root_, result);
    return result;
}

bool TileLayout::contains(const QString& id) const {
    return root_ && !id.isEmpty() && find(*root_, id) != nullptr;
}

bool TileLayout::place(const QString& id, const QString& target, Edge edge) {
    if (id.isEmpty())
        return false;
    if (!root_) {
        if (!target.isEmpty() && target != id)
            return false;
        root_ = Node{.session = id, .stacked = false, .ratio = 0.5, .children = {}};
        return true;
    }
    if (id == target || !contains(target))
        return false;
    if (edge == Edge::center)
        return replace(target, id);
    auto next = *this;
    if (next.contains(id))
        next.remove(id);
    else if (count() >= kMaximumTiles)
        return false;
    auto* tile = find(*next.root_, target);
    if (tile == nullptr)
        return false;
    const bool first = edge == Edge::left || edge == Edge::top;
    Node existing = std::move(*tile);
    Node added{.session = id, .stacked = false, .ratio = 0.5, .children = {}};
    *tile = Node{.session = {},
                 .stacked = edge == Edge::top || edge == Edge::bottom,
                 .ratio = 0.5,
                 .children = {}};
    tile->children.push_back(first ? std::move(added) : std::move(existing));
    tile->children.push_back(first ? std::move(existing) : std::move(added));
    *this = std::move(next);
    return true;
}

bool TileLayout::replace(const QString& from, const QString& to) {
    if (!root_ || to.isEmpty())
        return false;
    auto* tile = find(*root_, from);
    if (tile == nullptr)
        return false;
    if (auto* other = find(*root_, to); other != nullptr)
        other->session = from;
    tile->session = to;
    return true;
}

bool TileLayout::remove(const QString& id) {
    if (!root_)
        return false;
    if (root_->children.empty()) {
        if (root_->session != id)
            return false;
        root_.reset();
        return true;
    }
    const auto [parent, index] = parent_of(*root_, id);
    if (parent == nullptr)
        return false;
    Node sibling = std::move(parent->children[1 - index]);
    *parent = std::move(sibling);
    return true;
}

bool TileLayout::setRatio(const QString& path, qreal ratio) {
    if (!root_ || !std::isfinite(ratio))
        return false;
    auto* split = at_path(*root_, path);
    if (split == nullptr || split->children.size() != 2)
        return false;
    split->ratio = std::clamp(ratio, kMinimumRatio, kMaximumRatio);
    return true;
}

std::vector<TileLayout::Tile> TileLayout::tiles(const QRectF& bounds) const {
    std::vector<Tile> result;
    if (root_)
        lay_out(*root_, bounds, result);
    return result;
}

std::vector<TileLayout::Divider> TileLayout::dividers(const QRectF& bounds) const {
    std::vector<Divider> result;
    if (root_)
        lay_out_dividers(*root_, bounds, {}, result);
    return result;
}

QString TileLayout::neighbor(const QString& id, Edge direction) const {
    const auto all = tiles(QRectF(0, 0, 1, 1));
    const auto from = std::find_if(all.begin(), all.end(),
                                   [&](const Tile& tile) { return tile.session == id; });
    if (from == all.end() || direction == Edge::center)
        return {};
    QString best;
    qreal best_gap = 2;
    qreal best_offset = 2;
    for (const auto& tile : all) {
        if (tile.session == id || !beside(from->rect, tile.rect, direction))
            continue;
        // Nearest first; among equals, the one most in line with this tile.
        const qreal distance = gap(from->rect, tile.rect, direction);
        const qreal offset = (from->rect.center() - tile.rect.center()).manhattanLength();
        if (distance < best_gap - kEpsilon ||
            (std::abs(distance - best_gap) <= kEpsilon && offset < best_offset)) {
            best = tile.session;
            best_gap = distance;
            best_offset = offset;
        }
    }
    return best;
}

QJsonObject TileLayout::toJson() const { return root_ ? to_json(*root_) : QJsonObject{}; }

TileLayout TileLayout::fromJson(const QJsonObject& json, const QSet<QString>& known) {
    TileLayout layout;
    if (json.isEmpty())
        return layout;
    QSet<QString> seen;
    layout.root_ = from_json(json, known, seen, 0);
    while (layout.root_ && find(*layout.root_, QString()) != nullptr && layout.remove(QString())) {
    }
    if (layout.count() > kMaximumTiles)
        layout.root_.reset();
    return layout;
}
} // namespace lapis::desktop
