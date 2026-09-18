#include "terminal_surface.hpp"

#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QTextCharFormat>
#include <QTextLayout>

#include <algorithm>
#include <memory>

namespace lapis::desktop {
namespace {

QColor color(std::uint32_t rgb) { return QColor::fromRgb(rgb | 0xff000000U); }

QFont terminal_font() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(16);
    font.setStyleHint(QFont::Monospace);
    return font;
}

void add_row(QSGTextNode& node, const session::TerminalSnapshot& snapshot, std::size_t row,
             const QFont& font, qreal row_height) {
    QString text;
    QList<QTextLayout::FormatRange> formats;
    session::TerminalStyle previous{};
    for (std::size_t column = 0; column < snapshot.size.columns; ++column) {
        const std::size_t index = row * snapshot.size.columns + column;
        const auto& cell = snapshot.cells[index];
        if (cell.kind == session::CellKind::wide_tail)
            continue;
        const auto grapheme = snapshot.text(index);
        const QString value =
            grapheme.empty()
                ? QStringLiteral(" ")
                : QString::fromUcs4(grapheme.data(), static_cast<qsizetype>(grapheme.size()));
        const int start = static_cast<int>(text.size());
        text += value;
        if (!formats.empty() && previous == cell.style) {
            formats.back().length += static_cast<int>(value.size());
            continue;
        }
        QTextCharFormat format;
        QColor foreground = color(snapshot.cell_foreground(index));
        if (cell.style.faint)
            foreground.setAlphaF(0.6F);
        if (cell.style.invisible)
            foreground.setAlpha(0);
        format.setForeground(foreground);
        format.setBackground(color(snapshot.cell_background(index)));
        format.setFontWeight(cell.style.bold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(cell.style.italic);
        format.setFontUnderline(cell.style.underline != session::Underline::none);
        format.setFontStrikeOut(cell.style.strikethrough);
        formats.push_back({start, static_cast<int>(value.size()), format});
        previous = cell.style;
    }
    QTextLayout layout(text, font);
    layout.setFormats(formats);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid())
        line.setLineWidth(100000);
    layout.endLayout();
    node.addTextLayout(QPointF(0, static_cast<qreal>(row) * row_height), &layout);
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
    glyph->addTextLayout(rectangle.topLeft(), &layout);
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
                    qreal row_height) {
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
                auto text = std::unique_ptr<QSGTextNode>(window.createTextNode());
                text->setColor(color(next.foreground_rgb));
                text->setRenderType(QSGTextNode::QtRendering);
                add_row(*text, next, row, font, row_height);
                if (previous) {
                    rows->insertChildNodeBefore(text.release(), previous);
                    rows->removeChildNode(previous);
                    delete previous;
                } else
                    rows->appendChildNode(text.release());
            }
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

TerminalSurface::TerminalSurface(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setClip(true);
    publishFrame(true);
}

TerminalSurface::~TerminalSurface() {
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
        connect(document_, &SessionPreview::snapshotChanged, this, [this] { publishFrame(true); });
        connect(document_, &QObject::destroyed, this, [this] {
            document_ = nullptr;
            preedit_.clear();
            publishFrame(true);
            emit documentChanged();
        });
    }
    preedit_.clear();
    requestResize();
    publishFrame(true);
    emit documentChanged();
}

void TerminalSurface::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        requestResize();
        publishFrame(false);
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
        root->updateRows(*window(), snapshot, font, row_height);
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
    if (!enabled) {
        preedit_.clear();
        publishFrame(false);
    }
    setFlag(ItemAcceptsInputMethod, enabled);
    setAcceptedMouseButtons(enabled ? Qt::LeftButton : Qt::NoButton);
    setActiveFocusOnTab(enabled);
    requestResize();
    emit interactiveChanged();
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
    if (!interactive_ || !document_ || !document_->live()) {
        event->ignore();
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        document_->sendText(QGuiApplication::clipboard()->text().toUtf8(), true);
        event->accept();
        return;
    }
    if (event->modifiers().testFlag(Qt::MetaModifier)) {
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
    if (!interactive_ || !document_ || !document_->live()) {
        event->ignore();
        return;
    }
    if (!event->commitString().isEmpty())
        document_->sendText(event->commitString().toUtf8());
    preedit_ = event->preeditString();
    publishFrame(false);
    event->accept();
}
QVariant TerminalSurface::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImEnabled)
        return interactive_ && document_ && document_->live();
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
