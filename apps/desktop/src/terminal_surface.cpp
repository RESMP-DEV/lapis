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

} // namespace

TerminalSurface::TerminalSurface(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setClip(true);
}

void TerminalSurface::setDocument(SessionPreview* document) {
    if (document_ == document)
        return;
    if (document_)
        disconnect(document_, nullptr, this, nullptr);
    document_ = document;
    if (document_)
        connect(document_, &SessionPreview::snapshotChanged, this, [this] {
            content_dirty_ = true;
            update();
        });
    requestResize();
    content_dirty_ = true;
    emit documentChanged();
    update();
}

void TerminalSurface::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size()) {
        requestResize();
        update();
    }
}

QSGNode* TerminalSurface::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    // Qt owns the returned scene graph. updatePaintNode runs during scene-graph
    // synchronization while the GUI thread is blocked; snapshots are immutable.
    if (!document_ || width() <= 0 || height() <= 0) {
        delete old_node;
        return nullptr;
    }
    const auto& snapshot = document_->snapshot();
    const QFont font = terminal_font();
    const QFontMetricsF metrics(font);
    const qreal cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
    const qreal row_height = metrics.height() + 3;
    auto* root = static_cast<QSGTransformNode*>(old_node);
    if (content_dirty_ || root == nullptr) {
        delete root;
        auto owned = std::make_unique<QSGTransformNode>();
        auto background = std::make_unique<QSGSimpleRectNode>(
            QRectF(0, 0, snapshot.size.columns * cell_width, snapshot.size.rows * row_height),
            color(snapshot.background_rgb));
        owned->appendChildNode(background.release());
        auto text = std::unique_ptr<QSGTextNode>(window()->createTextNode());
        text->setColor(color(snapshot.foreground_rgb));
        text->setRenderType(QSGTextNode::QtRendering);
        for (std::size_t row = 0; row < snapshot.size.rows; ++row)
            add_row(*text, snapshot, row, font, row_height);
        owned->appendChildNode(text.release());
        if (snapshot.cursor.visible && snapshot.cursor.in_viewport) {
            auto cursor = std::make_unique<QSGSimpleRectNode>(
                QRectF(snapshot.cursor.column * cell_width, snapshot.cursor.row * row_height, 2,
                       metrics.height()),
                color(snapshot.foreground_rgb));
            owned->appendChildNode(cursor.release());
        }
        if (!preedit_.isEmpty()) {
            auto composition = std::unique_ptr<QSGTextNode>(window()->createTextNode());
            composition->setColor(color(snapshot.foreground_rgb));
            QTextLayout layout(preedit_, font);
            layout.beginLayout();
            auto line = layout.createLine();
            if (line.isValid())
                line.setLineWidth(10000);
            layout.endLayout();
            composition->addTextLayout(
                QPointF(snapshot.cursor.column * cell_width, snapshot.cursor.row * row_height),
                &layout);
            owned->appendChildNode(composition.release());
        }
        root = owned.release();
        content_dirty_ = false;
    }
    const qreal scale = std::min(width() / (snapshot.size.columns * cell_width),
                                 height() / (snapshot.size.rows * row_height));
    QMatrix4x4 matrix;
    matrix.scale(static_cast<float>(scale));
    root->setMatrix(matrix);
    return root;
}

void TerminalSurface::setInteractive(bool enabled) {
    if (interactive_ == enabled)
        return;
    interactive_ = enabled;
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
    } else if (event->modifiers().testFlag(Qt::ControlModifier) && event->key() >= Qt::Key_A &&
               event->key() <= Qt::Key_Z) {
        document_->sendText(QByteArray(1, static_cast<char>(event->key() - Qt::Key_A + 1)));
    } else if (!event->text().isEmpty())
        document_->sendText(event->text().toUtf8());
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
    content_dirty_ = true;
    update();
    event->accept();
}
QVariant TerminalSurface::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImEnabled)
        return interactive_ && document_ && document_->live();
    if (query == Qt::ImCursorRectangle && document_) {
        const QFontMetricsF metrics(terminal_font());
        const auto& cursor = document_->snapshot().cursor;
        return QRectF(cursor.column * metrics.horizontalAdvance(QLatin1Char('M')),
                      cursor.row * (metrics.height() + 3), 2, metrics.height());
    }
    return QQuickItem::inputMethodQuery(query);
}

} // namespace lapis::desktop
