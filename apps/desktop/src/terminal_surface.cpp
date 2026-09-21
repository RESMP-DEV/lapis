#include "terminal_surface.hpp"

#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeySequence>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QTextCharFormat>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <memory>

#include <QUuid>

namespace lapis::desktop {
namespace {

QColor color(std::uint32_t rgb) { return QColor::fromRgb(rgb | 0xff000000U); }

QFont terminal_font() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(16);
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

class TerminalNode final : public QSGTransformNode {
  public:
    std::shared_ptr<const session::TerminalSnapshot> snapshot;
    // Scene graph ownership stays with each parent; these are observers only.
    QSGSimpleRectNode* background{};
    QSGNode* rows{};
    QSGNode* overlays{};

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
};

void TerminalSurface::publishFrame(bool snapshot_changed) {
    // Only the GUI thread touches document_, preedit_, or item geometry. The
    // render thread gets owned immutable values through an explicit C++ handoff.
    auto frame = std::make_shared<RenderState>();
    frame->preedit = preedit_;
    frame->viewport = size();
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

void TerminalSurface::updateInteractionBlock() {
    if (!focus_workspace_)
        return;
    focus_workspace_->setInteractionBlocked(
        interaction_reason_, composition_state_ == CompositionState::active || paste_in_progress_);
}

void TerminalSurface::resetInputContext() {
    if (resetting_input_)
        return;
    resetting_input_ = true;
    if (composition_state_ == CompositionState::active)
        composition_state_ = CompositionState::stale;
    preedit_.clear();
    updateInteractionBlock();
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

TerminalSurface::TerminalSurface(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setClip(true);
    preview_update_.setSingleShot(true);
    preview_update_.setInterval(100); // Visible noninteractive previews are capped at 10 Hz.
    connect(&preview_update_, &QTimer::timeout, this, [this] {
        if (isVisible())
            publishFrame(true);
    });
    connect(this, &QQuickItem::visibleChanged, this, [this] {
        preview_update_.stop();
        if (isVisible())
            publishFrame(true);
    });
    window_changed_connection_ =
        connect(this, &QQuickItem::windowChanged, this, &TerminalSurface::bindWindow);
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
    setFocusWorkspace(nullptr);
    const std::lock_guard lock(render_mutex_);
    render_state_.reset();
}

void TerminalSurface::setDocument(SessionPreview* document) {
    if (document_ == document)
        return;
    if (document_)
        disconnect(document_, nullptr, this, nullptr);
    document_ = document;
    if (document_) {
        connect(document_, &SessionPreview::snapshotChanged, this, [this] {
            if (isVisible()) {
                if (interactive_)
                    publishFrame(true);
                else if (!preview_update_.isActive())
                    preview_update_.start();
            }
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
    const QFont font = terminal_font();
    const QFontMetricsF metrics(font);
    const qreal cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
    const qreal row_height = metrics.height() + 3;
    auto* root = static_cast<TerminalNode*>(old_node);
    if (!root)
        root = new TerminalNode(); // Qt takes ownership of the returned root.
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
    const qreal scale = std::min(frame->viewport.width() / (snapshot.size.columns * cell_width),
                                 frame->viewport.height() / (snapshot.size.rows * row_height));
    QMatrix4x4 matrix;
    matrix.scale(static_cast<float>(scale));
    root->setMatrix(matrix);
    return root;
}

void TerminalSurface::setInteractive(bool enabled) {
    if (interactive_ == enabled)
        return;
    interactive_ = enabled;
    preview_update_.stop();
    publishFrame(true);
    if (!enabled) {
        ++ime_epoch_;
        resetInputContext();
        publishFrame(false);
    }
    setFlag(ItemAcceptsInputMethod, enabled);
    setAcceptedMouseButtons(enabled ? Qt::LeftButton : Qt::NoButton);
    setActiveFocusOnTab(enabled);
    requestResize();
    emit interactiveChanged();
}

void TerminalSurface::setFocusWorkspace(Workspace* workspace) {
    if (focus_workspace_ == workspace)
        return;
    if (focus_workspace_)
        focus_workspace_->setInteractionBlocked(interaction_reason_, false);
    focus_workspace_ = workspace;
    interaction_reason_ = QStringLiteral("terminal-") + QUuid::createUuid().toString(QUuid::Id128);
    if (focus_workspace_)
        updateInteractionBlock();
    emit focusWorkspaceChanged();
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
    ++ime_epoch_;
    resetInputContext();
}
void TerminalSurface::requestResize() {
    if (!interactive_ || !document_ || !document_->live() || width() <= 0 || height() <= 0)
        return;
    const QFontMetricsF metrics(terminal_font());
    const auto columns = static_cast<std::uint16_t>(
        std::clamp(width() / metrics.horizontalAdvance(QLatin1Char('M')), 2.0, 300.0));
    const auto rows =
        static_cast<std::uint16_t>(std::clamp(height() / (metrics.height() + 3), 2.0, 100.0));
    document_->resizeTerminal({columns, rows});
}
void TerminalSurface::mousePressEvent(QMouseEvent* event) {
    if (interactive_) {
        forceActiveFocus(Qt::MouseFocusReason);
        event->accept();
    } else
        event->ignore();
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
    if (!acceptsTerminalInput()) {
        event->ignore();
        return;
    }
    if (composition_state_ == CompositionState::stale)
        composition_state_ = CompositionState::idle;
    if (event->matches(QKeySequence::Paste)) {
        const auto owner = document_;
        const QString text = QGuiApplication::clipboard()->text();
        paste_in_progress_ = true;
        updateInteractionBlock();
        ++ime_epoch_;
        resetInputContext();
        if (document_ == owner && acceptsTerminalInput()) {
            document_->sendText(text.toUtf8(), true);
        }
        paste_in_progress_ = false;
        updateInteractionBlock();
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
        // macOS editing convention: Command-Left/Right go to line start/end.
        // The shells people actually run (zsh with emacs bindings, bash, fish)
        // implement that as Ctrl-A and Ctrl-E, so translate rather than
        // reimplement line editing inside the terminal.
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            const bool line_start = event->key() == Qt::Key_Left;
            document_->sendKey(line_start ? session::TerminalKey::home : session::TerminalKey::end,
                               {false, true, false, false});
            event->accept();
            return;
        }
        // Every other Command combination stays available to the window for
        // navigation and menu shortcuts.
        event->ignore();
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
    updateInteractionBlock();
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
    if (!preedit_.isEmpty())
        composition_state_ = CompositionState::active;
    else if (!event->commitString().isEmpty())
        composition_state_ = CompositionState::idle;
    else if (composition_state_ == CompositionState::active)
        composition_state_ = CompositionState::stale;
    updateInteractionBlock();
    publishFrame(false);
    updateInputContext(Qt::ImCursorRectangle);
    event->accept();
}
QVariant TerminalSurface::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImEnabled)
        return acceptsTerminalInput();
    if (query == Qt::ImCursorRectangle && document_) {
        const QFontMetricsF metrics(terminal_font());
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
