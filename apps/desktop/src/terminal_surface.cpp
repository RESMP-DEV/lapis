#include "terminal_surface.hpp"
#include <QScopeGuard>

#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeySequence>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

namespace lapis::desktop {
namespace {

QColor color(std::uint32_t rgb) { return QColor::fromRgb(rgb | 0xff000000U); }

// An empty family keeps the platform's fixed-width system font. Callers pass
// only families already resolved as installed and fixed-pitch on the GUI thread.
bool modifier_key(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta ||
           key == Qt::Key_Alt || key == Qt::Key_CapsLock;
}

QFont terminal_font(const QString& family, int pixel_size) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!family.isEmpty())
        font.setFamily(family);
    font.setPixelSize(pixel_size);
    font.setStyleHint(QFont::Monospace);
    return font;
}

QFont styled_font(const QFont& font, const session::TerminalStyle& style) {
    QFont result = font;
    result.setBold(style.bold);
    result.setItalic(style.italic);
    return result;
}

QString grapheme_text(const session::TerminalSnapshot& snapshot, std::size_t index) {
    const auto grapheme = snapshot.text(index);
    return grapheme.empty()
               ? QStringLiteral(" ")
               : QString::fromUcs4(grapheme.data(), static_cast<qsizetype>(grapheme.size()));
}

std::uint32_t effective_color(const session::TerminalSnapshot& snapshot,
                              const session::TerminalColor& value, bool background) {
    switch (value.kind) {
    case session::ColorKind::default_color:
        return background ? snapshot.background_rgb : snapshot.foreground_rgb;
    case session::ColorKind::indexed:
        if (value.value >= snapshot.palette.size())
            return background ? snapshot.background_rgb : snapshot.foreground_rgb;
        return snapshot.palette[value.value];
    case session::ColorKind::rgb:
        return value.value;
    }
    return background ? snapshot.background_rgb : snapshot.foreground_rgb;
}

QTextCharFormat text_format(const session::TerminalSnapshot& snapshot, std::size_t index) {
    const auto& cell = snapshot.cells[index];
    QTextCharFormat format;
    QColor foreground = color(snapshot.cell_foreground(index));
    if (cell.style.faint)
        foreground.setAlphaF(0.6F);
    if (cell.style.invisible)
        foreground.setAlpha(0);
    format.setForeground(foreground);
    format.setFontWeight(cell.style.bold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(cell.style.italic);
    return format;
}

void add_rectangle(QSGNode& node, const QRectF& bounds, const QColor& value) {
    auto rectangle = std::make_unique<QSGSimpleRectNode>(bounds, value);
    node.appendChildNode(rectangle.release());
}

struct RowGeometry {
    qreal cell_width;
    qreal row_height;
};

enum class Decoration : std::uint8_t { underline, strike, overline };

struct DecorationStyle {
    session::Underline line{session::Underline::none};
    QColor color;
    bool operator==(const DecorationStyle&) const = default;
};

DecorationStyle decoration_style(const session::TerminalSnapshot& snapshot, std::size_t index,
                                 Decoration decoration) {
    const auto& style = snapshot.cells[index].style;
    session::Underline line = session::Underline::none;
    switch (decoration) {
    case Decoration::underline:
        line = style.underline;
        break;
    case Decoration::strike:
        if (style.strikethrough)
            line = session::Underline::single;
        break;
    case Decoration::overline:
        if (style.overline)
            line = session::Underline::single;
        break;
    }
    QColor value = color(snapshot.cell_foreground(index));
    if (decoration == Decoration::underline &&
        style.underline_color.kind != session::ColorKind::default_color)
        value = color(effective_color(snapshot, style.underline_color, false));
    if (style.faint)
        value.setAlphaF(0.6F);
    if (style.invisible)
        line = session::Underline::none;
    return {line, value};
}

void draw_decoration(QSGNode& node, const QRectF& bounds, const DecorationStyle& style) {
    if (style.line == session::Underline::none)
        return;
    // Preserve the existing single-line fallback for curly underlines until a
    // proper wave primitive is qualified. Other patterns use cell-grid geometry.
    if (style.line == session::Underline::dotted || style.line == session::Underline::dashed) {
        const qreal dash = style.line == session::Underline::dotted ? 1 : 3;
        const auto count = static_cast<std::size_t>(std::ceil(bounds.width() / (dash + 2)));
        for (std::size_t i = 0; i < count; ++i) {
            const qreal x = bounds.left() + static_cast<qreal>(i) * (dash + 2);
            add_rectangle(
                node, QRectF(x, bounds.top(), std::min(dash, bounds.right() - x), bounds.height()),
                style.color);
        }
        return;
    }
    add_rectangle(node, bounds, style.color);
    if (style.line == session::Underline::double_line)
        add_rectangle(node, bounds.translated(0, bounds.height() + 1), style.color);
}

void add_decoration_spans(QSGNode& node, const session::TerminalSnapshot& snapshot, std::size_t row,
                          const RowGeometry& geometry, Decoration decoration, qreal y) {
    std::size_t start = 0;
    while (start < snapshot.size.columns) {
        const auto style =
            decoration_style(snapshot, row * snapshot.size.columns + start, decoration);
        std::size_t end = start + 1;
        while (end < snapshot.size.columns &&
               decoration_style(snapshot, row * snapshot.size.columns + end, decoration) == style)
            ++end;
        draw_decoration(node,
                        QRectF(static_cast<qreal>(start) * geometry.cell_width, y,
                               static_cast<qreal>(end - start) * geometry.cell_width, 1),
                        style);
        start = end;
    }
}

void add_decorations(QSGNode& node, const session::TerminalSnapshot& snapshot, std::size_t row,
                     const QFont& font, const RowGeometry& geometry) {
    const QFontMetricsF metrics(font);
    const qreal top = static_cast<qreal>(row) * geometry.row_height;
    const qreal baseline = top + metrics.ascent();
    add_decoration_spans(node, snapshot, row, geometry, Decoration::underline, baseline + 1);
    add_decoration_spans(node, snapshot, row, geometry, Decoration::strike,
                         baseline - metrics.strikeOutPos());
    add_decoration_spans(node, snapshot, row, geometry, Decoration::overline, top + 1);
}

bool safe_ascii_cell(const session::TerminalCell& cell, const QString& value,
                     const QFontMetricsF& metrics, const QFontMetricsF& bold_metrics,
                     const QFontMetricsF& italic_metrics, const QFontMetricsF& bold_italic_metrics,
                     qreal cell_width) {
    if (cell.kind != session::CellKind::narrow || value.size() != 1 ||
        value.front().unicode() <= 0x1f || value.front().unicode() > 0x7e)
        return false;
    const QFontMetricsF& styled = cell.style.bold && cell.style.italic ? bold_italic_metrics
                                  : cell.style.bold                    ? bold_metrics
                                  : cell.style.italic                  ? italic_metrics
                                                                       : metrics;
    return styled.horizontalAdvance(value) == cell_width;
}

void add_row(QSGNode& backgrounds, QSGTextNode& glyphs, const session::TerminalSnapshot& snapshot,
             std::size_t row, const QFont& font, qreal cell_width, qreal row_height) {
    QString text;
    QList<QTextLayout::FormatRange> formats;
    session::TerminalStyle previous{};
    std::size_t run_start = 0;
    bool run_safe = true;
    const QFontMetricsF metrics(font);
    const QFontMetricsF bold_metrics = QFontMetricsF(styled_font(font, [] {
        session::TerminalStyle style;
        style.bold = true;
        return style;
    }()));
    const QFontMetricsF italic_metrics = QFontMetricsF(styled_font(font, [] {
        session::TerminalStyle style;
        style.italic = true;
        return style;
    }()));
    const QFontMetricsF bold_italic_metrics = QFontMetricsF(styled_font(font, [] {
        session::TerminalStyle style;
        style.bold = true;
        style.italic = true;
        return style;
    }()));
    const qreal top = static_cast<qreal>(row) * row_height;
    QFont safe_font = font;
    safe_font.setKerning(false);
    safe_font.setFeature(QFont::Tag("liga"), 0);
    safe_font.setFeature(QFont::Tag("clig"), 0);
    safe_font.setFeature(QFont::Tag("dlig"), 0);
    safe_font.setFeature(QFont::Tag("calt"), 0);
    const auto flush_run = [&]() {
        if (text.isEmpty())
            return;
        QTextLayout layout(text, run_safe ? safe_font : font);
        QTextOption option;
        option.setTextDirection(Qt::LeftToRight);
        option.setAlignment(Qt::AlignLeft);
        layout.setTextOption(option);
        layout.setFormats(formats);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (line.isValid())
            line.setLineWidth(100000);
        layout.endLayout();
        // Fallback fonts may have different ascents. Keep every run on the
        // terminal baseline rather than aligning their layout boxes at the top.
        const qreal baseline_offset = line.isValid() ? metrics.ascent() - line.ascent() : 0;
        glyphs.addTextLayout(
            QPointF(static_cast<qreal>(run_start) * cell_width, top + baseline_offset), &layout);
        text.clear();
        formats.clear();
    };
    QColor background = color(snapshot.background_rgb);
    std::size_t background_start = 0;
    const auto flush_background = [&](std::size_t end) {
        if (background != color(snapshot.background_rgb))
            add_rectangle(backgrounds,
                          QRectF(static_cast<qreal>(background_start) * cell_width, top,
                                 static_cast<qreal>(end - background_start) * cell_width,
                                 row_height),
                          background);
        background_start = end;
    };
    for (std::size_t column = 0; column < snapshot.size.columns; ++column) {
        const std::size_t index = row * snapshot.size.columns + column;
        const auto& cell = snapshot.cells[index];
        const QColor cell_background = color(snapshot.cell_background(index));
        if (cell_background != background) {
            flush_background(column);
            background = cell_background;
        }
        if (cell.kind == session::CellKind::wide_tail) {
            flush_run();
            continue;
        }
        const QString value = grapheme_text(snapshot, index);
        const bool safe_cell = safe_ascii_cell(cell, value, metrics, bold_metrics, italic_metrics,
                                               bold_italic_metrics, cell_width);
        if (!safe_cell || !run_safe || text.isEmpty() || cell.style != previous) {
            flush_run();
            run_start = column;
        }
        run_safe = safe_cell;
        const int start = static_cast<int>(text.size());
        text += value;
        if (!formats.empty() && previous == cell.style) {
            formats.back().length += static_cast<int>(value.size());
            continue;
        }
        formats.push_back({start, static_cast<int>(value.size()), text_format(snapshot, index)});
        previous = cell.style;
    }
    flush_background(snapshot.size.columns);
    flush_run();
}

void add_cursor(QSGNode& overlays, QQuickWindow& window, const session::TerminalSnapshot& snapshot,
                const QFont& font, qreal cell_width, qreal row_height) {
    if (!snapshot.cursor.visible || !snapshot.cursor.in_viewport ||
        snapshot.cursor.column >= snapshot.size.columns ||
        snapshot.cursor.row >= snapshot.size.rows)
        return;
    const auto index = static_cast<std::size_t>(snapshot.cursor.row) * snapshot.size.columns +
                       snapshot.cursor.column;
    const auto& cell = snapshot.cells.at(index);
    const qreal width = cell.kind == session::CellKind::wide ? 2 * cell_width : cell_width;
    const QRectF rectangle(snapshot.cursor.column * cell_width, snapshot.cursor.row * row_height,
                           width, row_height);
    const QColor cursor_color = color(snapshot.cursor_rgb.value_or(snapshot.foreground_rgb));
    const auto add_rectangle = [&](const QRectF& bounds) {
        auto node = std::make_unique<QSGSimpleRectNode>(bounds, cursor_color);
        overlays.appendChildNode(node.release());
    };
    using session::CursorShape;
    switch (snapshot.cursor.shape) {
    case CursorShape::bar:
        add_rectangle(QRectF(rectangle.topLeft(), QSizeF(2, row_height)));
        return;
    case CursorShape::underline:
        add_rectangle(QRectF(rectangle.left(), rectangle.bottom() - 2, width, 2));
        return;
    case CursorShape::hollow_block:
        add_rectangle(QRectF(rectangle.topLeft(), QSizeF(width, 2)));
        add_rectangle(QRectF(rectangle.left(), rectangle.bottom() - 2, width, 2));
        add_rectangle(QRectF(rectangle.topLeft(), QSizeF(2, row_height)));
        add_rectangle(QRectF(rectangle.right() - 2, rectangle.top(), 2, row_height));
        return;
    case CursorShape::block:
        add_rectangle(rectangle);
        break;
    }
    // Repaint the covered grapheme in the cell background color so a block
    // cursor does not obscure the character beneath it.
    const auto grapheme = snapshot.text(index);
    if (grapheme.empty() || cell.style.invisible || cell.kind == session::CellKind::wide_tail)
        return;
    QFont cursor_font = font;
    cursor_font.setBold(cell.style.bold);
    cursor_font.setItalic(cell.style.italic);
    QTextLayout layout(QString::fromUcs4(grapheme.data(), static_cast<qsizetype>(grapheme.size())),
                       cursor_font);
    layout.beginLayout();
    auto line = layout.createLine();
    if (line.isValid())
        line.setLineWidth(width);
    layout.endLayout();
    auto glyph = std::unique_ptr<QSGTextNode>(window.createTextNode());
    glyph->setColor(color(snapshot.cell_background(index)));
    glyph->setRenderType(QSGTextNode::QtRendering);
    const qreal baseline_offset = line.isValid() ? QFontMetricsF(font).ascent() - line.ascent() : 0;
    glyph->addTextLayout(rectangle.topLeft() + QPointF(0, baseline_offset), &layout);
    overlays.appendChildNode(glyph.release());
}

bool same_row(const session::TerminalSnapshot& left, const session::TerminalSnapshot& right,
              std::size_t row) {
    if (left.size != right.size || left.foreground_rgb != right.foreground_rgb ||
        left.background_rgb != right.background_rgb || left.palette != right.palette)
        return false;
    for (std::size_t column = 0; column < left.size.columns; ++column) {
        const auto index = row * left.size.columns + column;
        const auto& a = left.cells[index];
        const auto& b = right.cells[index];
        if (a.kind != b.kind || a.style != b.style || left.text(index) != right.text(index))
            return false;
    }
    return true;
}

// Selection endpoints in reading order.
std::pair<QPoint, QPoint> ordered(QPoint first, QPoint second) {
    const bool swap = first.y() > second.y() || (first.y() == second.y() && first.x() > second.x());
    return swap ? std::pair{second, first} : std::pair{first, second};
}

void add_selection(QSGNode& overlays, const session::TerminalSnapshot& snapshot, QPoint start,
                   QPoint end, qreal cell_width, qreal row_height) {
    QColor tint = color(snapshot.foreground_rgb);
    tint.setAlpha(80);
    const int rows = snapshot.size.rows;
    const int columns = snapshot.size.columns;
    for (int row = start.y(); row <= end.y() && row < rows; ++row) {
        const int first = row == start.y() ? start.x() : 0;
        const int last = std::min(row == end.y() ? end.x() : columns - 1, columns - 1);
        if (last >= first)
            add_rectangle(overlays,
                          QRectF(first * cell_width, row * row_height,
                                 (last - first + 1) * cell_width, row_height),
                          tint);
    }
}

class TerminalNode final : public QSGTransformNode {
  public:
    std::shared_ptr<const session::TerminalSnapshot> snapshot;
    // Scene graph ownership stays with each parent; these are observers only.
    QSGSimpleRectNode* background{};
    QSGNode* rows{};
    QSGNode* overlays{};
    // Rows are laid out for one font; a different font rebuilds every row.
    QString font_family;
    int font_pixel_size{};

    TerminalNode() {
        auto background_node = std::make_unique<QSGSimpleRectNode>();
        background = background_node.get();
        appendChildNode(background_node.release());
        auto row_nodes = std::make_unique<QSGNode>();
        rows = row_nodes.get();
        appendChildNode(row_nodes.release());
        auto overlay_nodes = std::make_unique<QSGNode>();
        overlays = overlay_nodes.get();
        appendChildNode(overlay_nodes.release());
    }
    void updateRows(QQuickWindow& window, const session::TerminalSnapshot& next, const QFont& font,
                    qreal cell_width, qreal row_height) {
        if (!snapshot || snapshot->size != next.size) {
            while (auto* child = rows->firstChild()) {
                rows->removeChildNode(child);
                delete child;
            }
        }
        auto* previous = rows->firstChild();
        for (std::size_t row = 0; row < next.size.rows; ++row) {
            auto* following = previous ? previous->nextSibling() : nullptr;
            if (!previous || !same_row(*snapshot, next, row)) {
                auto row_node = std::make_unique<QSGNode>();
                auto backgrounds = std::make_unique<QSGNode>();
                auto decorations = std::make_unique<QSGNode>();
                auto text = std::unique_ptr<QSGTextNode>(window.createTextNode());
                text->setColor(color(next.foreground_rgb));
                text->setRenderType(QSGTextNode::QtRendering);
                add_row(*backgrounds, *text, next, row, font, cell_width, row_height);
                add_decorations(*decorations, next, row, font, RowGeometry{cell_width, row_height});
                row_node->appendChildNode(backgrounds.release());
                row_node->appendChildNode(text.release());
                row_node->appendChildNode(decorations.release());
                if (previous) {
                    rows->insertChildNodeBefore(row_node.release(), previous);
                    rows->removeChildNode(previous);
                    delete previous;
                } else
                    rows->appendChildNode(row_node.release());
            }
            previous = following;
        }
        while (previous) {
            auto* following = previous->nextSibling();
            rows->removeChildNode(previous);
            delete previous;
            previous = following;
        }
    }
};

} // namespace

struct TerminalSurface::RenderState {
    std::shared_ptr<const session::TerminalSnapshot> snapshot;
    QString preedit;
    QSizeF viewport;
    // Plain values: the render thread builds its own QFont from them.
    QString font_family;
    int font_pixel_size{};
    qreal minimum_scale{};
    std::optional<std::pair<QPoint, QPoint>> selection;
};

void TerminalSurface::publishFrame(bool snapshot_changed) {
    // Only the GUI thread touches document_, preedit_, or item geometry. The
    // render thread gets owned immutable values through an explicit C++ handoff.
    auto frame = std::make_shared<RenderState>();
    frame->preedit = preedit_;
    frame->viewport = size();
    frame->font_family = use_system_font_ ? QString() : resolved_font_family_;
    frame->font_pixel_size = font_pixel_size_;
    frame->minimum_scale = minimum_scale_;
    if (selection_anchor_ && selection_head_)
        frame->selection = ordered(*selection_anchor_, *selection_head_);
    {
        const std::lock_guard lock(render_mutex_);
        if (!snapshot_changed && render_state_)
            frame->snapshot = render_state_->snapshot;
    }
    if (snapshot_changed && document_)
        frame->snapshot = std::make_shared<const session::TerminalSnapshot>(document_->snapshot());
    {
        const std::lock_guard lock(render_mutex_);
        render_state_ = std::move(frame);
    }
    update();
}

void TerminalSurface::updateInputContext(Qt::InputMethodQueries queries) {
    if (hasActiveFocus())
        if (auto* method = qApp ? qApp->inputMethod() : nullptr)
            method->update(queries);
}

void TerminalSurface::resetInputContext() {
    if (resetting_input_)
        return;
    resetting_input_ = true;
    if (composition_state_ == CompositionState::active)
        composition_state_ = CompositionState::stale;
    preedit_.clear();
    emit inputOwnershipChanged();
    if (qApp && qApp->focusObject() == this)
        qApp->inputMethod()->reset();
    resetting_input_ = false;
    publishFrame(false);
    updateInputContext(Qt::ImEnabled | Qt::ImCursorRectangle);
}

bool TerminalSurface::acceptsTerminalInput() const {
    return !resetting_input_ && interactive_ && document_ && document_->live() &&
           document_->inputReady() && hasActiveFocus() && window() && window()->isActive();
}

TerminalSurface::TerminalSurface(QQuickItem* parent)
    : QQuickItem(parent),
      resolved_font_family_(QFontDatabase::systemFont(QFontDatabase::FixedFont).family()) {
    setFlag(ItemHasContents);
    setClip(true);
    window_changed_connection_ =
        connect(this, &QQuickItem::windowChanged, this, &TerminalSurface::bindWindow);
    throttle_.setSingleShot(true);
    connect(&throttle_, &QTimer::timeout, this, [this] { publishFrame(true); });
    bindWindow(window());
    publishFrame(true);
}

void TerminalSurface::bindWindow(QQuickWindow* current) {
    if (window_active_connection_)
        disconnect(window_active_connection_);
    if (current)
        window_active_connection_ = connect(current, &QWindow::activeChanged, this, [this] {
            if (!window() || !window()->isActive()) {
                ++ime_epoch_;
                resetInputContext();
            } else {
                updateInputContext(Qt::ImEnabled | Qt::ImCursorRectangle);
            }
        });
}

TerminalSurface::~TerminalSurface() {
    disconnect(window_changed_connection_);
    disconnect(window_active_connection_);
    const std::lock_guard lock(render_mutex_);
    render_state_.reset();
}

void TerminalSurface::setDocument(SessionPreview* document) {
    if (document_ == document)
        return;
    if (document_)
        disconnect(document_, nullptr, this, nullptr);
    document_ = document;
    clearSelection();
    if (document_) {
        connect(document_, &SessionPreview::snapshotChanged, this, [this] {
            // A selection names what was on screen; once that text moves or
            // changes, the highlight would point at something else.
            if (selection_anchor_ && selection_head_ &&
                textBetween(*selection_anchor_, *selection_head_) != selection_text_)
                clearSelection();
            if (frame_interval_ > 0) {
                // The first change opens the window; later ones share its frame.
                if (!throttle_.isActive())
                    throttle_.start(frame_interval_);
                return;
            }
            publishFrame(true);
            updateInputContext(Qt::ImCursorRectangle);
        });
        connect(document_, &SessionPreview::connectionChanged, this, [this] {
            if (!document_ || !document_->inputReady()) {
                ++ime_epoch_;
                resetInputContext();
            } else {
                updateInputContext(Qt::ImEnabled | Qt::ImCursorRectangle);
            }
        });
        connect(document_, &QObject::destroyed, this, [this] {
            document_ = nullptr;
            ++ime_epoch_;
            resetInputContext();
            publishFrame(true);
            emit documentChanged();
        });
    }
    ++ime_epoch_;
    resetInputContext();
    requestResize();
    publishFrame(true);
    emit documentChanged();
}

void TerminalSurface::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        requestResize();
        publishFrame(false);
        updateInputContext(Qt::ImCursorRectangle);
    }
}

QSGNode* TerminalSurface::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    // Qt owns the returned nodes. No GUI-owned document or mutable text is
    // dereferenced here, including on an empty/reloaded surface.
    std::shared_ptr<const RenderState> frame;
    {
        const std::lock_guard lock(render_mutex_);
        frame = render_state_;
    }
    if (!frame || !frame->snapshot || frame->viewport.isEmpty()) {
        delete old_node;
        return nullptr;
    }
    const auto& snapshot = *frame->snapshot;
    const QFont font = terminal_font(frame->font_family, frame->font_pixel_size);
    const QFontMetricsF metrics(font);
    const qreal cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
    const qreal row_height = metrics.height() + 3;
    auto* root = static_cast<TerminalNode*>(old_node);
    if (!root)
        root = new TerminalNode(); // Qt takes ownership of the returned root.
    if (root->font_family != frame->font_family ||
        root->font_pixel_size != frame->font_pixel_size) {
        root->font_family = frame->font_family;
        root->font_pixel_size = frame->font_pixel_size;
        root->snapshot.reset(); // Forces updateRows to discard every cached row.
    }
    root->background->setRect(
        QRectF(0, 0, snapshot.size.columns * cell_width, snapshot.size.rows * row_height));
    root->background->setColor(color(snapshot.background_rgb));
    if (root->snapshot != frame->snapshot)
        root->updateRows(*window(), snapshot, font, cell_width, row_height);
    root->snapshot = frame->snapshot;
    while (auto* child = root->overlays->firstChild()) {
        root->overlays->removeChildNode(child);
        delete child;
    }
    add_cursor(*root->overlays, *window(), snapshot, font, cell_width, row_height);
    if (frame->selection)
        add_selection(*root->overlays, snapshot, frame->selection->first, frame->selection->second,
                      cell_width, row_height);
    if (!frame->preedit.isEmpty() && snapshot.cursor.in_viewport) {
        auto composition = std::unique_ptr<QSGTextNode>(window()->createTextNode());
        composition->setColor(color(snapshot.foreground_rgb));
        QTextLayout layout(frame->preedit, font);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid())
            line.setLineWidth(10000);
        layout.endLayout();
        composition->addTextLayout(
            QPointF(snapshot.cursor.column * cell_width, snapshot.cursor.row * row_height),
            &layout);
        root->overlays->appendChildNode(composition.release());
    }
    qreal scale = std::min(frame->viewport.width() / (snapshot.size.columns * cell_width),
                           frame->viewport.height() / (snapshot.size.rows * row_height));
    QMatrix4x4 matrix;
    if (frame->minimum_scale > 0 && scale < frame->minimum_scale) {
        // Keep the rows up to the cursor readable, where agent TUIs keep
        // their prompt and latest output; the item clips the rest.
        scale = frame->minimum_scale;
        const auto rows = static_cast<qreal>(snapshot.size.rows);
        const qreal visible_rows = frame->viewport.height() / (row_height * scale);
        const qreal last_row = snapshot.cursor.visible && snapshot.cursor.in_viewport
                                   ? std::min(rows, static_cast<qreal>(snapshot.cursor.row) + 3)
                                   : rows;
        const qreal first_row = std::max(0.0, std::floor(last_row - visible_rows));
        matrix.translate(0, static_cast<float>(-first_row * row_height * scale));
    }
    matrix.scale(static_cast<float>(scale));
    root->setMatrix(matrix);
    return root;
}

void TerminalSurface::setInteractive(bool enabled) {
    if (interactive_ == enabled)
        return;
    interactive_ = enabled;
    if (!enabled) {
        ++ime_epoch_;
        resetInputContext();
        publishFrame(false);
    }
    setFlag(ItemAcceptsInputMethod, enabled);
    setAcceptedMouseButtons(enabled ? Qt::LeftButton : Qt::NoButton);
    // A dialog disables the stage while it still holds focus; Qt refuses to
    // drop tab focus from the focused item, so that waits for focusOutEvent.
    if (enabled || !hasActiveFocus())
        setActiveFocusOnTab(enabled);
    requestResize();
    emit interactiveChanged();
}

void TerminalSurface::setFrameInterval(int milliseconds) {
    const int bounded = std::clamp(milliseconds, 0, 5000);
    if (frame_interval_ == bounded)
        return;
    frame_interval_ = bounded;
    if (frame_interval_ == 0 && throttle_.isActive()) {
        throttle_.stop();
        publishFrame(true);
    }
    emit frameIntervalChanged();
}

void TerminalSurface::setMinimumScale(qreal scale) {
    const qreal bounded = std::clamp(scale, 0.0, 1.0);
    if (qFuzzyCompare(minimum_scale_ + 1, bounded + 1))
        return;
    minimum_scale_ = bounded;
    publishFrame(false);
    emit minimumScaleChanged();
}

void TerminalSurface::focusInEvent(QFocusEvent* event) {
    QQuickItem::focusInEvent(event);
    if (!hasActiveFocus())
        return;
    updateInputContext(Qt::ImEnabled | Qt::ImCursorRectangle);
}

void TerminalSurface::focusOutEvent(QFocusEvent* event) {
    QQuickItem::focusOutEvent(event);
    if (hasActiveFocus())
        return;
    if (!interactive_ && activeFocusOnTab())
        setActiveFocusOnTab(false);
    ++ime_epoch_;
    resetInputContext();
}
QFont TerminalSurface::cellFont() const {
    return terminal_font(use_system_font_ ? QString() : resolved_font_family_, font_pixel_size_);
}

void TerminalSurface::setFontFamily(const QString& family) {
    if (font_family_ == family)
        return;
    font_family_ = family;
    applyFont();
}

void TerminalSurface::setFontPixelSize(int pixels) {
    const int bounded = std::clamp(pixels, kTerminalFontSizeMinimum, kTerminalFontSizeMaximum);
    if (font_pixel_size_ == bounded)
        return;
    font_pixel_size_ = bounded;
    applyFont();
}

// Resolve on the GUI thread, then commit the new cell grid as one resize.
void TerminalSurface::applyFont() {
    const bool usable = !font_family_.isEmpty() && QFontDatabase::hasFamily(font_family_) &&
                        QFontDatabase::isFixedPitch(font_family_);
    use_system_font_ = !usable;
    resolved_font_family_ =
        usable ? font_family_ : QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    emit fontChanged();
    requestResize();
    publishFrame(false);
    updateInputContext(Qt::ImCursorRectangle);
}

void TerminalSurface::requestResize() {
    QSize grid;
    if (width() > 0 && height() > 0) {
        const QFontMetricsF metrics(cellFont());
        grid = QSize(static_cast<int>(std::clamp(
                         width() / metrics.horizontalAdvance(QLatin1Char('M')), 2.0, 300.0)),
                     static_cast<int>(std::clamp(height() / (metrics.height() + 3), 2.0, 100.0)));
    }
    if (grid != grid_size_) {
        grid_size_ = grid;
        emit gridSizeChanged();
    }
    if (!interactive_ || !document_ || !document_->live() || grid.isEmpty())
        return;
    document_->resizeTerminal(
        {static_cast<std::uint16_t>(grid.width()), static_cast<std::uint16_t>(grid.height())});
}
void TerminalSurface::mousePressEvent(QMouseEvent* event) {
    if (!interactive_) {
        event->ignore();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
#ifdef Q_OS_MACOS
    const auto link_modifier = Qt::MetaModifier;
#else
    const auto link_modifier = Qt::ControlModifier;
#endif
    if (event->button() == Qt::LeftButton && event->modifiers() == link_modifier && document_) {
        // Command-click (Control-click elsewhere) opens a web link in the browser.
        const auto cell = cellAt(event->position());
        const QUrl url(terminal_url_at(document_->snapshot(), cell.x(), cell.y()),
                       QUrl::StrictMode);
        if (url.isValid() &&
            (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")))
            QDesktopServices::openUrl(url);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        clearSelection();
        press_cell_ = cellAt(event->position());
        selecting_ = true;
    }
    event->accept();
}
void TerminalSurface::mouseMoveEvent(QMouseEvent* event) {
    if (!selecting_) {
        event->ignore();
        return;
    }
    const auto cell = cellAt(event->position());
    if (selection_anchor_ || cell != press_cell_)
        setSelection(press_cell_, cell);
    event->accept();
}
void TerminalSurface::mouseReleaseEvent(QMouseEvent* event) {
    selecting_ = false;
    event->accept();
}
// Double-click selects the run of non-blank cells under the pointer.
void TerminalSurface::mouseDoubleClickEvent(QMouseEvent* event) {
    const auto grid = cellGrid();
    if (!interactive_ || !grid || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    selecting_ = false;
    const auto cell = cellAt(event->position());
    const auto& snapshot = document_->snapshot();
    const auto blank = [&](int column) {
        const auto index =
            static_cast<std::size_t>(cell.y()) * static_cast<std::size_t>(grid->columns) +
            static_cast<std::size_t>(column);
        if (snapshot.cells[index].kind == session::CellKind::wide_tail)
            return false;
        const auto text = snapshot.text(index);
        return text.empty() || text == U" ";
    };
    if (!blank(cell.x())) {
        int first = cell.x();
        int last = cell.x();
        while (first > 0 && !blank(first - 1))
            --first;
        while (last + 1 < grid->columns && !blank(last + 1))
            ++last;
        setSelection({first, cell.y()}, {last, cell.y()});
    }
    event->accept();
}
void TerminalSurface::wheelEvent(QWheelEvent* event) {
    if (!interactive_ || !document_) {
        event->ignore();
        return;
    }
    wheel_remainder_ += event->angleDelta().y();
    const int steps = wheel_remainder_ / 120;
    wheel_remainder_ -= steps * 120;
    if (steps != 0)
        scrollHistory(steps);
    event->accept();
}
// Positive steps scroll back. Full-screen programs scroll themselves, so on
// the alternate screen the wheel becomes arrow keys, as in other terminals.
void TerminalSurface::scrollHistory(int steps) {
    if (document_->snapshot().alternate_screen && !document_->historyActive()) {
        if (!acceptsTerminalInput())
            return;
        const auto key = steps > 0 ? session::TerminalKey::up : session::TerminalKey::down;
        for (int line = 0; line < std::abs(steps) * 3; ++line)
            document_->sendKey(key, {});
        return;
    }
    if (steps > 0) {
        document_->olderHistory();
        return;
    }
    if (!document_->historyActive())
        return;
    // The newest page answers with a message instead of a page.
    if (!document_->historyRequestPending() && !document_->historyMessage().isEmpty())
        document_->returnToLive();
    else
        document_->newerHistory();
}
std::optional<TerminalSurface::CellGrid> TerminalSurface::cellGrid() const {
    if (!document_)
        return std::nullopt;
    const auto size = document_->snapshot().size;
    if (size.columns == 0 || size.rows == 0 || width() <= 0 || height() <= 0)
        return std::nullopt;
    const QFontMetricsF metrics(cellFont());
    const qreal cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
    const qreal row_height = metrics.height() + 3;
    // Matches updatePaintNode for a surface without a minimum scale.
    const qreal scale =
        std::min(width() / (size.columns * cell_width), height() / (size.rows * row_height));
    return CellGrid{cell_width * scale, row_height * scale, size.columns, size.rows};
}
QPoint TerminalSurface::cellAt(QPointF position) const {
    const auto grid = cellGrid();
    if (!grid)
        return {};
    return {std::clamp(static_cast<int>(position.x() / grid->width), 0, grid->columns - 1),
            std::clamp(static_cast<int>(position.y() / grid->height), 0, grid->rows - 1)};
}
QRectF TerminalSurface::cellRect(int column, int row) const {
    const auto grid = cellGrid();
    if (!grid)
        return {};
    return {column * grid->width, row * grid->height, grid->width, grid->height};
}
QString TerminalSurface::textBetween(QPoint start, QPoint end) const {
    const auto grid = cellGrid();
    if (!grid)
        return {};
    const auto& snapshot = document_->snapshot();
    const auto [from, to] = ordered(start, end);
    QStringList lines;
    for (int row = from.y(); row <= to.y() && row < grid->rows; ++row) {
        const int first = row == from.y() ? from.x() : 0;
        const int last = std::min(row == to.y() ? to.x() : grid->columns - 1, grid->columns - 1);
        QString line;
        for (int column = first; column <= last; ++column) {
            const auto index =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(grid->columns) +
                static_cast<std::size_t>(column);
            const auto kind = snapshot.cells[index].kind;
            if (kind == session::CellKind::wide_tail || kind == session::CellKind::wrap_spacer)
                continue;
            const auto text = snapshot.text(index);
            line += text.empty()
                        ? QStringLiteral(" ")
                        : QString::fromUcs4(text.data(), static_cast<qsizetype>(text.size()));
        }
        // Trailing blanks are the rest of the row, not copied text.
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}
void TerminalSurface::setSelection(QPoint anchor, QPoint head) {
    selection_anchor_ = anchor;
    selection_head_ = head;
    selection_text_ = textBetween(anchor, head);
    emit selectionChanged();
    publishFrame(false);
}
void TerminalSurface::clearSelection() {
    selecting_ = false;
    if (!selection_anchor_ && selection_text_.isEmpty())
        return;
    selection_anchor_.reset();
    selection_head_.reset();
    selection_text_.clear();
    emit selectionChanged();
    publishFrame(false);
}
// Typing while reading history returns to the live screen and is delivered
// there, as in other terminals. Modifiers and Command shortcuts other than
// paste do not.
void TerminalSurface::resumeLiveForTyping(const QKeyEvent& event) {
    if (!interactive_ || !document_ || !document_->historyActive() || modifier_key(event.key()))
        return;
    if ((event.modifiers() & Qt::MetaModifier) == 0 || event.matches(QKeySequence::Paste))
        document_->returnToLive();
}
// Command-C on macOS; Control-Shift-C elsewhere, where Control-C belongs to
// the terminal. Either copies the selection, and never reaches the agent.
bool TerminalSurface::copySelection(const QKeyEvent& event) {
#ifdef Q_OS_MACOS
    const bool copy = event.key() == Qt::Key_C && event.modifiers() == Qt::MetaModifier;
#else
    const bool copy =
        event.key() == Qt::Key_C && event.modifiers() == (Qt::ControlModifier | Qt::ShiftModifier);
#endif
    if (!copy)
        return false;
    if (!selection_text_.isEmpty())
        QGuiApplication::clipboard()->setText(selection_text_);
    return true;
}
namespace {
// One screen row as text, with the column where each character starts.
struct RowText {
    QString text;
    QList<int> columns;
};
RowText row_text(const session::TerminalSnapshot& snapshot, int row) {
    RowText result;
    const int columns = snapshot.size.columns;
    for (int column = 0; column < columns; ++column) {
        const auto index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                           static_cast<std::size_t>(column);
        const auto kind = snapshot.cells[index].kind;
        if (kind == session::CellKind::wide_tail || kind == session::CellKind::wrap_spacer)
            continue;
        const auto text = snapshot.text(index);
        const auto value =
            text.empty() ? QStringLiteral(" ")
                         : QString::fromUcs4(text.data(), static_cast<qsizetype>(text.size()));
        for (qsizetype unit = 0; unit < value.size(); ++unit)
            result.columns.append(column);
        result.text += value;
    }
    return result;
}
} // namespace

QString terminal_url_at(const session::TerminalSnapshot& snapshot, int column, int row) {
    const int rows = snapshot.size.rows;
    if (row < 0 || row >= rows || column < 0 || column >= snapshot.size.columns)
        return {};
    // A row that runs to the last column is treated as wrapping onto the next
    // one, which is how agents print long links in a narrow terminal.
    const auto fills = [&](const RowText& line) { return !line.text.endsWith(QLatin1Char(' ')); };
    int first = row;
    while (first > 0 && fills(row_text(snapshot, first - 1)))
        --first;
    QString joined;
    qsizetype target = -1;
    for (int current = first; current < rows; ++current) {
        const auto line = row_text(snapshot, current);
        if (current == row)
            for (qsizetype unit = 0; unit < line.columns.size(); ++unit)
                if (line.columns[unit] == column) {
                    target = joined.size() + unit;
                    break;
                }
        joined += line.text;
        if (current >= row && !fills(line))
            break;
    }
    if (target < 0)
        return {};
    static const QRegularExpression pattern(QStringLiteral(R"(https?://[^\s<>"'`]+)"));
    for (auto matches = pattern.globalMatch(joined); matches.hasNext();) {
        const auto match = matches.next();
        auto url = match.captured();
        // Sentence punctuation and unbalanced closing brackets end the link.
        while (!url.isEmpty() && QStringLiteral(".,;:!?)]}").contains(url.back()) &&
               !(url.back() == QLatin1Char(')') &&
                 url.count(QLatin1Char('(')) > url.count(QLatin1Char(')')) - 1))
            url.chop(1);
        if (target >= match.capturedStart() && target < match.capturedStart() + url.size())
            return url;
    }
    return {};
}

QByteArray terminal_text_key(const QKeyEvent& event) {
    const auto mods = event.modifiers();
    if (mods.testFlag(Qt::MetaModifier))
        return {};
    if (mods.testFlag(Qt::GroupSwitchModifier))
        return event.text().toUtf8();
    QByteArray text;
    if (mods.testFlag(Qt::ControlModifier)) {
        const int key = event.key();
        int control = -1;
        if (key >= Qt::Key_A && key <= Qt::Key_Underscore)
            control = static_cast<int>(static_cast<unsigned int>(key) & 0x1fU);
        else if (key == Qt::Key_Space || key == Qt::Key_At || key == Qt::Key_2)
            control = 0;
        else if (key == Qt::Key_6)
            control = 30;
        else if (key == Qt::Key_Minus)
            control = 31;
        if (control >= 0)
            text.append(static_cast<char>(control));
    } else if (mods.testFlag(Qt::AltModifier) && event.key() >= Qt::Key_Space &&
               event.key() <= Qt::Key_AsciiTilde) {
        // Option's composed text (for example Option+B -> integral sign) is
        // replaced with the base letter for conventional terminal Meta keys.
        const bool lower = event.key() >= Qt::Key_A && event.key() <= Qt::Key_Z &&
                           !mods.testFlag(Qt::ShiftModifier);
        const int letter = event.key() + (lower ? 32 : 0);
        text.append(static_cast<char>(letter));
    } else
        text = event.text().toUtf8();
    if (mods.testFlag(Qt::AltModifier) && !text.isEmpty())
        text.prepend('\x1b');
    return text;
}

void TerminalSurface::keyPressEvent(QKeyEvent* event) {
    // Copying also works on a read-only history page.
    if (interactive_ && copySelection(*event)) {
        event->accept();
        return;
    }
    resumeLiveForTyping(*event);
    if (!acceptsTerminalInput()) {
        event->ignore();
        return;
    }
    // New input replaces what was selected; a modifier alone does not.
    if (!modifier_key(event->key()))
        clearSelection();
    if (composition_state_ == CompositionState::stale)
        composition_state_ = CompositionState::idle;
    if (event->matches(QKeySequence::Paste)) {
        pasting_ = true;
        emit inputOwnershipChanged();
        const auto release_paste = qScopeGuard([this] {
            pasting_ = false;
            emit inputOwnershipChanged();
        });
        const auto owner = document_;
        const QString text = QGuiApplication::clipboard()->text();
        ++ime_epoch_;
        resetInputContext();
        if (document_ == owner && acceptsTerminalInput())
            document_->sendText(text.toUtf8(), true);
        event->accept();
        return;
    }
    if (!preedit_.isEmpty()) {
        if (event->key() == Qt::Key_Escape) {
            ++ime_epoch_;
            resetInputContext();
        }
        event->accept();
        return;
    }
    if (event->modifiers().testFlag(Qt::MetaModifier)) {
        commandKey(*event);
        return;
    }
    std::optional<session::TerminalKey> key;
    switch (event->key()) {
    case Qt::Key_Up:
        key = session::TerminalKey::up;
        break;
    case Qt::Key_Down:
        key = session::TerminalKey::down;
        break;
    case Qt::Key_Left:
        key = session::TerminalKey::left;
        break;
    case Qt::Key_Right:
        key = session::TerminalKey::right;
        break;
    case Qt::Key_Home:
        key = session::TerminalKey::home;
        break;
    case Qt::Key_End:
        key = session::TerminalKey::end;
        break;
    case Qt::Key_PageUp:
        key = session::TerminalKey::page_up;
        break;
    case Qt::Key_PageDown:
        key = session::TerminalKey::page_down;
        break;
    case Qt::Key_Insert:
        key = session::TerminalKey::insert;
        break;
    case Qt::Key_Delete:
        key = session::TerminalKey::delete_key;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        key = session::TerminalKey::enter;
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        key = session::TerminalKey::tab;
        break;
    case Qt::Key_Backspace:
        key = session::TerminalKey::backspace;
        break;
    case Qt::Key_Escape:
        key = session::TerminalKey::escape;
        break;
    default:
        break;
    }
    if (key) {
        const auto mods = event->modifiers();
        document_->sendKey(*key,
                           {mods.testFlag(Qt::ShiftModifier), mods.testFlag(Qt::ControlModifier),
                            mods.testFlag(Qt::AltModifier), false});
    } else {
        const auto text = terminal_text_key(*event);
        if (!text.isEmpty())
            document_->sendText(text);
    }
    event->accept();
}
// Command chords: macOS line editing, or left to the window's shortcuts.
void TerminalSurface::commandKey(QKeyEvent& event) {
    // macOS editing convention: Command-Left/Right go to line start/end.
    // The shells people actually run (zsh with emacs bindings, bash, fish)
    // implement that as Ctrl-A and Ctrl-E, so translate rather than
    // reimplement line editing inside the terminal.
    // AppKit also marks physical arrow keys as keypad keys.
    const bool bare_command = (event.modifiers() & ~Qt::KeypadModifier) == Qt::MetaModifier;
    if (bare_command && (event.key() == Qt::Key_Left || event.key() == Qt::Key_Right)) {
        const bool line_start = event.key() == Qt::Key_Left;
        document_->sendText(QByteArray(1, line_start ? '\x01' : '\x05'));
        event.accept();
        return;
    }
    // Command-Backspace deletes to the line start (Ctrl-U) and
    // Command-Delete to the line end (Ctrl-K), as in iTerm2's natural text
    // editing and the shells' emacs bindings.
    if (bare_command && (event.key() == Qt::Key_Backspace || event.key() == Qt::Key_Delete)) {
        document_->sendText(QByteArray(1, event.key() == Qt::Key_Backspace ? '\x15' : '\x0b'));
        event.accept();
        return;
    }
    // Every other Command combination stays available to the window for
    // navigation and menu shortcuts.
    event.ignore();
}
void TerminalSurface::inputMethodEvent(QInputMethodEvent* event) {
    const quint64 composition_epoch = ime_epoch_;
    if (!acceptsTerminalInput()) {
        ++ime_epoch_;
        resetInputContext();
        event->ignore();
        return;
    }
    const bool valid_replacement =
        event->replacementStart() == 0 && event->replacementLength() == 0;
    if (!valid_replacement) {
        ++ime_epoch_;
        resetInputContext();
        event->accept();
        return;
    }
    if (!event->preeditString().isEmpty())
        composition_state_ = CompositionState::active;
    if (!event->commitString().isEmpty()) {
        if (composition_state_ == CompositionState::stale) {
            event->ignore();
            return;
        }
        document_->sendText(event->commitString().toUtf8());
    }
    if (composition_epoch != ime_epoch_) {
        event->accept();
        return;
    }
    preedit_ = event->preeditString();
    emit inputOwnershipChanged();
    if (!preedit_.isEmpty())
        composition_state_ = CompositionState::active;
    else if (!event->commitString().isEmpty())
        composition_state_ = CompositionState::idle;
    else if (composition_state_ == CompositionState::active)
        composition_state_ = CompositionState::stale;
    publishFrame(false);
    updateInputContext(Qt::ImCursorRectangle);
    event->accept();
}
QVariant TerminalSurface::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImEnabled)
        return acceptsTerminalInput();
    if (query == Qt::ImCursorRectangle && document_) {
        const QFontMetricsF metrics(cellFont());
        const auto& snapshot = document_->snapshot();
        if (!snapshot.cursor.in_viewport)
            return QRectF();
        const qreal cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
        const qreal row_height = metrics.height() + 3;
        const qreal scale = std::min(width() / (snapshot.size.columns * cell_width),
                                     height() / (snapshot.size.rows * row_height));
        return QRectF(snapshot.cursor.column * cell_width * scale,
                      snapshot.cursor.row * row_height * scale, 2 * scale,
                      metrics.height() * scale);
    }
    return QQuickItem::inputMethodQuery(query);
}

} // namespace lapis::desktop
