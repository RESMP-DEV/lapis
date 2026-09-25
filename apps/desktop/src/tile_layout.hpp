#ifndef LAPIS_DESKTOP_TILE_LAYOUT_HPP
#define LAPIS_DESKTOP_TILE_LAYOUT_HPP
#include <QJsonObject>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <vector>

namespace lapis::desktop {
namespace tile_detail {
struct Node {
    QString session;
    bool stacked{};
    qreal ratio{0.5};
    std::vector<Node> children; // none for a tile, two for a split
};
} // namespace tile_detail

// Agents tiled on a category's stage, as iTerm2 splits a tab into panes: a
// binary tree whose leaves are agents and whose inner nodes split their area
// side by side or stacked, `ratio` of it to the first child. A plain value, so
// it rolls back with the rest of the workspace registry.
class TileLayout {
  public:
    // Where a tile goes relative to the one it lands on; center takes its place.
    enum class Edge : std::uint8_t { left, right, top, bottom, center };
    static constexpr int kMaximumTiles = 8;
    struct Tile {
        QString session;
        QRectF rect;
    };
    // A split's dividing line, and the area its ratio divides (for dragging).
    struct Divider {
        QString path; // child indices from the root, as '0' and '1'
        bool stacked{};
        QRectF line;
        QRectF area;
    };

    [[nodiscard]] static std::optional<Edge> edge(const QString& name);
    [[nodiscard]] bool empty() const { return !root_.has_value(); }
    [[nodiscard]] int count() const;
    // Agents in reading order: left to right, then top to bottom within splits.
    [[nodiscard]] QStringList sessions() const;
    [[nodiscard]] bool contains(const QString& id) const;
    // Puts `id` beside `target` (or, at center, in its place). An agent already
    // tiled moves; the first agent on an empty layout needs no target.
    bool place(const QString& id, const QString& target, Edge edge);
    // `to` takes `from`'s tile; if `to` is tiled too, the two swap.
    bool replace(const QString& from, const QString& to);
    // Takes an agent off the stage; its sibling takes the space.
    bool remove(const QString& id);
    bool setRatio(const QString& path, qreal ratio);
    [[nodiscard]] std::vector<Tile> tiles(const QRectF& bounds) const;
    [[nodiscard]] std::vector<Divider> dividers(const QRectF& bounds) const;
    // The nearest tile beside `id` in `direction`, or empty.
    [[nodiscard]] QString neighbor(const QString& id, Edge direction) const;
    [[nodiscard]] QJsonObject toJson() const;
    // Agents not in `known`, repeats and malformed splits are dropped.
    [[nodiscard]] static TileLayout fromJson(const QJsonObject& json, const QSet<QString>& known);

  private:
    std::optional<tile_detail::Node> root_;
};
} // namespace lapis::desktop
#endif
