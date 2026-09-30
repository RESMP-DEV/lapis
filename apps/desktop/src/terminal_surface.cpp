#include "terminal_surface.hpp"
#include "cell_shapes.hpp"
#include "workspace.hpp"
#include <QScopeGuard>

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeySequence>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGSimpleRectNode>
#include <QSGTextNode>
#include <QStringConverter>
#include <QStringList>
#include <QSysInfo>
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
SessionPreview* TerminalSurface::document() const { return document_; }
namespace {

QColor color(std::uint32_t rgb) { return QColor::fromRgb(rgb | 0xff000000U); }

bool valid_file_url_path(const QUrl& url) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded =
        decoder(QByteArray::fromPercentEncoding(url.path(QUrl::FullyEncoded).toUtf8()));
    return !decoder.hasError() && !decoded.contains(u'\0');
}

struct SurfaceLayout {
    qreal scale{};
    qreal first_row{};
};
SurfaceLayout surface_layout(const session::TerminalSnapshot& snapshot, QSizeF viewport,
                             qreal minimum_scale, QSizeF cell_size) {
    const qreal cell_width = cell_size.width();
    const qreal row_height = cell_size.height();
    qreal scale = std::min(viewport.width() / (snapshot.size.columns * cell_width),
                           viewport.height() / (snapshot.size.rows * row_height));
    qreal first_row = 0;
    if (minimum_scale > 0 && scale < minimum_scale) {
        scale = minimum_scale;
        const auto rows = static_cast<qreal>(snapshot.size.rows);
        const qreal visible_rows = viewport.height() / (row_height * scale);
        const qreal last_row = snapshot.cursor.visible && snapshot.cursor.in_viewport
                                   ? std::min(rows, static_cast<qreal>(snapshot.cursor.row) + 3)
                                   : rows;
        first_row = std::max(0.0, std::floor(last_row - visible_rows));
    }
    return {scale, first_row};
}

bool modifier_key(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta ||
           key == Qt::Key_Alt || key == Qt::Key_CapsLock;
}

// An empty family keeps the platform's fixed-width system font. Callers pass
// only families already resolved as installed and fixed-pitch on the GUI thread.
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

#ifdef Q_OS_MACOS
constexpr auto kLinkModifier = Qt::MetaModifier; // Command
constexpr auto kLinkKey = Qt::Key_Meta;
#else
constexpr auto kLinkModifier = Qt::ControlModifier;
constexpr auto kLinkKey = Qt::Key_Control;
#endif
// A suggestion sent with Tab is submitted once its paste settled.
constexpr int kSubmitAfterPasteMs = 150;

void add_rectangle(QSGNode& node, const QRectF& bounds, const QColor& value) {
    auto rectangle = std::make_unique<QSGSimpleRectNode>(bounds, value);
    node.appendChildNode(rectangle.release());
}

// A line of `width` through the points, as a triangle strip.
void add_stroke(QSGNode& node, const std::vector<QPointF>& stroke, qreal width,
                const QColor& value) {
    std::vector<QPointF> points;
    for (const auto& point : stroke)
        if (points.empty() ||
            std::hypot(point.x() - points.back().x(), point.y() - points.back().y()) > 0.01)
            points.push_back(point);
    if (points.size() < 2)
        return;
    auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_Point2D(),
                                                  static_cast<int>(points.size() * 2));
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    auto* vertices = geometry->vertexDataAsPoint2D();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const QPointF direction =
            points[std::min(i + 1, points.size() - 1)] - points[i > 0 ? i - 1 : 0];
        const qreal length = std::hypot(direction.x(), direction.y());
        const QPointF normal =
            length > 0 ? QPointF(-direction.y(), direction.x()) * (width / 2 / length) : QPointF();
        vertices[2 * i].set(static_cast<float>(points[i].x() + normal.x()),
                            static_cast<float>(points[i].y() + normal.y()));
        vertices[(2 * i) + 1].set(static_cast<float>(points[i].x() - normal.x()),
                                  static_cast<float>(points[i].y() - normal.y()));
    }
    auto stroke_node = std::make_unique<QSGGeometryNode>();
    stroke_node->setGeometry(geometry.release());
    stroke_node->setFlag(QSGNode::OwnsGeometry);
    auto material = std::make_unique<QSGFlatColorMaterial>();
    material->setColor(value);
    stroke_node->setMaterial(material.release());
    stroke_node->setFlag(QSGNode::OwnsMaterial);
    node.appendChildNode(stroke_node.release());
}

void add_shapes(QSGNode& node, const CellShapes& shapes, const QColor& value) {
    for (const auto& fill : shapes.fills) {
        QColor shade = value;
        shade.setAlphaF(static_cast<float>(value.alphaF() * fill.alpha));
        add_rectangle(node, fill.rect, shade);
    }
    add_stroke(node, shapes.stroke, shapes.width, value);
}

// The shapes for a cell's character when it is a block element or box
// drawing; empty for anything drawn from the font.
CellShapes shapes_for(const session::TerminalSnapshot& snapshot, std::size_t index,
                      const QRectF& bounds, qreal ratio) {
    const auto grapheme = snapshot.text(index);
    if (grapheme.size() != 1 || snapshot.cells[index].kind != session::CellKind::narrow)
        return {};
    return cell_shapes(grapheme.front(), bounds, ratio);
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

// Block and box characters are drawn as shapes filling their cells, over the
// row's backgrounds, so they join across rows; everything else is text.
void add_row(QSGNode& backgrounds, QSGTextNode& glyphs, const session::TerminalSnapshot& snapshot,
             std::size_t row, const QFont& font, const RowGeometry& geometry, qreal ratio) {
    const qreal cell_width = geometry.cell_width;
    const qreal row_height = geometry.row_height;
    std::vector<std::pair<CellShapes, QColor>> shapes;
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
        if (auto drawn = shapes_for(
                snapshot, index,
                QRectF(static_cast<qreal>(column) * cell_width, top, cell_width, row_height),
                ratio);
            !drawn.empty()) {
            flush_run();
            shapes.emplace_back(std::move(drawn),
                                text_format(snapshot, index).foreground().color());
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
    for (const auto& [drawn, ink] : shapes)
        add_shapes(backgrounds, drawn, ink);
}

QColor blend(const QColor& from, const QColor& to, qreal share) {
    return QColor::fromRgbF(static_cast<float>(from.redF() * share + to.redF() * (1 - share)),
                            static_cast<float>(from.greenF() * share + to.greenF() * (1 - share)),
                            static_cast<float>(from.blueF() * share + to.blueF() * (1 - share)));
}

} // namespace

SuggestionLayout lay_out_suggestion(const session::TerminalSnapshot& snapshot,
                                    const QFontMetricsF& metrics, const QString& suggestion) {
    SuggestionLayout layout;
    const qreal cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
    const int first = snapshot.cursor.column + 1;
    const int last = static_cast<int>(snapshot.size.columns) - 2;
    if (!snapshot.cursor.in_viewport || snapshot.cursor.row >= snapshot.size.rows ||
        last - first < 4)
        return layout;
    const qreal room = (last - first) * cell_width;
    const QString text = suggestion.trimmed();
    const QString line = text.section(QLatin1Char('\n'), 0, 0);
    layout.keys = QStringLiteral("  ⇥");
    const qreal keys_width = metrics.horizontalAdvance(layout.keys);
    layout.whole =
        line.size() == text.size() && metrics.horizontalAdvance(line) <= room - keys_width;
    if (!layout.whole)
        layout.keys = QStringLiteral("  ⇥ types it");
    if (room < metrics.horizontalAdvance(layout.keys) + 8 * cell_width)
        layout.keys.clear();
    const qreal keys_room = metrics.horizontalAdvance(layout.keys);
    layout.shown = metrics.elidedText(layout.whole ? line : line + QStringLiteral(" …"),
                                      Qt::ElideRight, room - keys_room);
    layout.column = first;
    layout.width = metrics.horizontalAdvance(layout.shown) + keys_room;
    return layout;
}

namespace {
// An offered next prompt, dim just after the cursor, then the keys that take
// it: Tab sends it when it shows whole, else only types it. The row is covered
// only under what is drawn.
void add_suggestion(QSGNode& overlays, QQuickWindow& window,
                    const session::TerminalSnapshot& snapshot, const QFont& font,
                    const QString& suggestion, qreal cell_width, qreal row_height) {
    const QFontMetricsF metrics(font);
    const auto layout = lay_out_suggestion(snapshot, metrics, suggestion);
    if (layout.shown.isEmpty())
        return;
    const QPointF origin(layout.column * cell_width, snapshot.cursor.row * row_height);
    const QColor background = color(snapshot.background_rgb);
    const QColor foreground = color(snapshot.foreground_rgb);
    add_rectangle(overlays, QRectF(origin, QSizeF(layout.width, row_height)), background);
    const auto write = [&](const QString& words, QPointF at, qreal share) {
        auto node = std::unique_ptr<QSGTextNode>(window.createTextNode());
        node->setColor(blend(foreground, background, share));
        QTextLayout text(words, font);
        text.beginLayout();
        auto line = text.createLine();
        if (line.isValid())
            line.setLineWidth(10000);
        text.endLayout();
        node->addTextLayout(at, &text);
        overlays.appendChildNode(node.release());
    };
    write(layout.shown, origin, 0.5);
    if (!layout.keys.isEmpty())
        write(layout.keys, origin + QPointF(metrics.horizontalAdvance(layout.shown), 0), 0.3);
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
    if (const auto drawn =
            shapes_for(snapshot, index, rectangle, window.effectiveDevicePixelRatio());
        !drawn.empty()) {
        add_shapes(overlays, drawn, color(snapshot.cell_background(index)));
        return;
    }
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

// The link under the pointer while Command is held: a faint wash and an
// underline in the text's color, over every row it wraps across.
void add_link(QSGNode& overlays, const session::TerminalSnapshot& snapshot,
              const std::vector<TerminalMatch>& cells, qreal cell_width, qreal row_height) {
    const QColor text = color(snapshot.foreground_rgb);
    QColor wash = text;
    wash.setAlpha(36);
    const qreal thickness = std::max<qreal>(1, row_height / 14);
    for (const auto& span : cells) {
        if (span.row < 0 || span.row >= snapshot.size.rows)
            continue;
        const QRectF bounds(span.first_column * cell_width, span.row * row_height,
                            (span.last_column - span.first_column + 1) * cell_width, row_height);
        add_rectangle(overlays, bounds, wash);
        add_rectangle(
            overlays,
            QRectF(bounds.left(), bounds.bottom() - thickness - 1, bounds.width(), thickness),
            text);
    }
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
                add_row(*backgrounds, *text, next, row, font, RowGeometry{cell_width, row_height},
                        window.effectiveDevicePixelRatio());
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
    std::vector<TerminalMatch> link;
    QString suggestion;
};

void TerminalSurface::publishFrame(bool snapshot_changed) {
    // Only the GUI thread touches document_, preedit_, or item geometry. The
    // render thread gets owned immutable values through an explicit C++ handoff.
    auto frame = std::make_shared<RenderState>();
    frame->preedit = preedit_;
    frame->suggestion = document_ && document_->attentionPending() ? QString() : suggestion_;
    frame->viewport = size();
    frame->font_family = use_system_font_ ? QString() : resolved_font_family_;
    frame->font_pixel_size = font_pixel_size_;
    frame->minimum_scale = minimum_scale_;
    if (selection_anchor_ && selection_head_)
        frame->selection = ordered(*selection_anchor_, *selection_head_);
    if (hovered_link_)
        frame->link = hovered_link_->cells;
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
    submit_timer_.setSingleShot(true);
    connect(&submit_timer_, &QTimer::timeout, this, [this] {
        // Still never into a request that arrived meanwhile.
        if (submit_owner_ && submit_owner_->live() && submit_owner_->inputReady() &&
            !submit_owner_->attentionPending())
            submit_owner_->sendKey(session::TerminalKey::enter, {});
        submit_owner_.clear();
    });
    setFlag(ItemHasContents);
    setClip(true);
    setAcceptHoverEvents(true);
    window_changed_connection_ =
        connect(this, &QQuickItem::windowChanged, this, &TerminalSurface::bindWindow);
    throttle_.setSingleShot(true);
    connect(&throttle_, &QTimer::timeout, this, [this] {
        since_frame_.start();
        publishFrame(true);
    });
    bindWindow(window());
    publishFrame(true);
}

void TerminalSurface::bindWindow(QQuickWindow* current) {
    if (warm_connection_) {
        disconnect(warm_connection_);
        warm_connection_ = {};
    }
    if (window_active_connection_)
        disconnect(window_active_connection_);
    if (window_visible_connection_)
        disconnect(window_visible_connection_);
    if (current)
        window_visible_connection_ =
            connect(current, &QWindow::visibleChanged, this, &TerminalSurface::updateViewing);
    updateViewing();
    if (current)
        window_active_connection_ = connect(current, &QWindow::activeChanged, this, [this] {
            if (!window() || !window()->isActive()) {
                ++ime_epoch_;
                resetInputContext();
            } else {
                updateInputContext(Qt::ImEnabled | Qt::ImCursorRectangle);
                claimSize();
                reportSeen();
            }
        });
}

TerminalSurface::~TerminalSurface() {
    disconnect(window_changed_connection_);
    disconnect(window_active_connection_);
    disconnect(window_visible_connection_);
    disconnect(warm_connection_);
    if (viewed_)
        viewed_->removeViewer(viewed_interval_);
    const std::lock_guard lock(render_mutex_);
    render_state_.reset();
}

// A new screen: redraw now, or within the view's frame interval.
void TerminalSurface::screenChanged() {
    // A selection names what was on screen; once that text moves or
    // changes, the highlight would point at something else.
    if (selection_anchor_ && selection_head_ &&
        textBetween(*selection_anchor_, *selection_head_) != selection_text_)
        clearSelection();
    // The underlined link follows what is under the pointer now.
    if (hovered_link_)
        updateLink(hover_position_, QGuiApplication::keyboardModifiers());
    if (frame_interval_ > 0) {
        // A change after a quiet interval draws at once; changes
        // within one share the frame at its end.
        if (!throttle_.isActive()) {
            const auto waited = since_frame_.isValid() ? since_frame_.elapsed() : frame_interval_;
            if (waited >= frame_interval_) {
                since_frame_.start();
                publishFrame(true);
            } else {
                throttle_.start(static_cast<int>(frame_interval_ - waited));
            }
        }
        updateInputContext(Qt::ImCursorRectangle);
        return;
    }
    publishFrame(true);
    updateInputContext(Qt::ImCursorRectangle);
}

void TerminalSurface::setDocument(SessionPreview* document) {
    if (document_ == document)
        return;
    if (document_)
        disconnect(document_, nullptr, this, nullptr);
    document_ = document;
    typed_since_arrival_ = false;
    wheel_remainder_ = 0;
    pixel_remainder_ = 0;
    selecting_ = false;
    clearSelection();
    clearLink();
    if (document_) {
        connect(document_, &SessionPreview::snapshotChanged, this, &TerminalSurface::screenChanged);
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
            selecting_ = false;
            clearSelection();
            ++ime_epoch_;
            resetInputContext();
            publishFrame(true);
            emit documentChanged();
        });
    }
    updateViewing();
    ++ime_epoch_;
    resetInputContext();
    requestResize();
    claimSize();
    publishFrame(true);
    emit documentChanged();
}

void TerminalSurface::keepFramesComing() {
    constexpr qint64 warm_ms = 600;
    auto* shown = window();
    if (shown == nullptr)
        return;
    since_typed_.start();
    if (!warm_connection_)
        // Each frame asks for the next until typing has paused for warm_ms.
        warm_connection_ = connect(
            shown, &QQuickWindow::frameSwapped, this,
            [this] {
                if (window() != nullptr && since_typed_.isValid() &&
                    since_typed_.elapsed() < warm_ms) {
                    window()->update();
                    return;
                }
                disconnect(warm_connection_);
                warm_connection_ = {};
            },
            Qt::QueuedConnection);
    shown->update();
}

// Shown while this view and its window are visible: a hidden window, a
// hidden strip or a closed tile decodes nothing for its agent.
void TerminalSurface::updateViewing() {
    SessionPreview* showing =
        document_ && isVisible() && window() != nullptr && window()->isVisible() ? document_.data()
                                                                                 : nullptr;
    if (showing == viewed_ && (showing == nullptr || viewed_interval_ == frame_interval_))
        return;
    if (viewed_)
        viewed_->removeViewer(viewed_interval_);
    viewed_ = showing;
    viewed_interval_ = frame_interval_;
    if (viewed_)
        viewed_->addViewer(viewed_interval_);
}

void TerminalSurface::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged)
        updateViewing();
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
    if (!frame->suggestion.isEmpty() && frame->preedit.isEmpty())
        add_suggestion(*root->overlays, *window(), snapshot, font, frame->suggestion, cell_width,
                       row_height);
    add_cursor(*root->overlays, *window(), snapshot, font, cell_width, row_height);
    if (frame->selection)
        add_selection(*root->overlays, snapshot, frame->selection->first, frame->selection->second,
                      cell_width, row_height);
    if (!frame->link.empty())
        add_link(*root->overlays, snapshot, frame->link, cell_width, row_height);
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
    const auto layout = surface_layout(snapshot, frame->viewport, frame->minimum_scale,
                                       QSizeF(cell_width, row_height));
    QMatrix4x4 matrix;
    matrix.translate(0, static_cast<float>(-layout.first_row * row_height * layout.scale));
    matrix.scale(static_cast<float>(layout.scale));
    root->setMatrix(matrix);
    return root;
}

void TerminalSurface::setHoldResize(bool hold) {
    if (hold_resize_ == hold)
        return;
    hold_resize_ = hold;
    emit holdResizeChanged();
    if (!hold)
        requestResize();
}

void TerminalSurface::setInteractive(bool enabled) {
    if (interactive_ == enabled)
        return;
    interactive_ = enabled;
    if (!enabled) {
        selecting_ = false;
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
    updateViewing();
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
    updateInputContext(Qt::ImCursorRectangle);
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
    clearLink();
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
    if (!interactive_ || hold_resize_ || !document_ || !document_->live() || grid.isEmpty())
        return;
    document_->resizeTerminal(
        {static_cast<std::uint16_t>(grid.width()), static_cast<std::uint16_t>(grid.height())});
}
// Coming back to this window after using the agent on another device (a joined
// phone) takes the terminal size back: activating the window, showing the
// agent on the stage, or moving the pointer over it.
void TerminalSurface::claimSize() {
    if (interactive_ && document_ && document_->live())
        document_->claimTerminalSize();
}
void TerminalSurface::hoverMoveEvent(QHoverEvent* event) {
    // Qt Quick repeats hover events while the scene changes under a resting
    // pointer; only a pointer that moved means someone is at this Mac.
    if (event->globalPosition() != last_hover_) {
        last_hover_ = event->globalPosition();
        claimSize();
    }
    updateLink(event->position(), event->modifiers());
    event->ignore();
}
void TerminalSurface::hoverLeaveEvent(QHoverEvent* event) {
    clearLink();
    event->ignore();
}
void TerminalSurface::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == kLinkKey)
        clearLink();
    QQuickItem::keyReleaseEvent(event);
}
// While Command is held, the link or existing path under the pointer is
// underlined and the pointer becomes a hand; Command-click opens it.
void TerminalSurface::updateLink(QPointF position, Qt::KeyboardModifiers modifiers) {
    hover_position_ = position;
    if (!interactive_ || !document_ || !modifiers.testFlag(kLinkModifier) || !contains(position)) {
        clearLink();
        return;
    }
    const auto cell = cellAt(position);
    auto link = terminal_link_at(document_->snapshot(), cell.x(), cell.y());
    const auto target = link ? linkTarget(*link) : QString();
    if (!link || target.isEmpty()) {
        clearLink();
        return;
    }
    if (hovered_link_ && hovered_link_->cells == link->cells && hovered_target_ == target)
        return;
    hovered_link_ = std::move(link);
    hovered_target_ = target;
    setCursor(Qt::PointingHandCursor);
    publishFrame(false);
    emit hoveredLinkChanged();
}
void TerminalSurface::clearLink() {
    if (!hovered_link_)
        return;
    hovered_link_.reset();
    hovered_target_.clear();
    unsetCursor();
    publishFrame(false);
    emit hoveredLinkChanged();
}
// A web link, or a written path as an existing file or folder here; an
// agent over ssh prints paths on another machine, so only its links open.
QString TerminalSurface::linkTarget(const TerminalLink& link) const {
    if (link.kind == TerminalLink::Kind::url) {
        const QUrl url(link.text, QUrl::StrictMode);
        return url.isValid() && !url.host().isEmpty() &&
                       (url.scheme() == QLatin1String("https") ||
                        url.scheme() == QLatin1String("http"))
                   ? link.text
                   : QString();
    }
    const auto folder = document_ ? document_->linkFolder() : QString();
    if (folder.isEmpty())
        return {}; // Paths printed by a remote agent do not identify local files.
    if (link.text.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        const QUrl uri(link.text, QUrl::StrictMode);
        const auto host = uri.host();
        if (!uri.isValid() || !uri.isLocalFile() || !valid_file_url_path(uri) ||
            (!host.isEmpty() &&
             host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0 &&
             host.compare(QSysInfo::machineHostName(), Qt::CaseInsensitive) != 0))
            return {};
        return resolve_terminal_path(uri.path(QUrl::FullyDecoded), folder);
    }
    return resolve_terminal_path(link.text, folder);
}
void TerminalSurface::mousePressEvent(QMouseEvent* event) {
    if (!interactive_) {
        event->ignore();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::LeftButton && event->modifiers() == kLinkModifier && document_) {
        // Command-click (Control-click elsewhere) opens what it is on as a
        // double-click in the Finder would: a web link in the browser, an
        // image in Preview, a folder in the Finder. An app or a program is
        // shown in its folder rather than run.
        const auto cell = cellAt(event->position());
        const auto link = terminal_link_at(document_->snapshot(), cell.x(), cell.y());
        const auto target = link ? linkTarget(*link) : QString();
        if (link && !target.isEmpty()) {
            QUrl url(target);
            if (link->kind == TerminalLink::Kind::path) {
                const QFileInfo info(target);
                const bool runs = info.isBundle() || (info.isFile() && info.isExecutable());
                url = QUrl::fromLocalFile(runs ? info.absolutePath() : target);
            }
            if (!opens_links_ || QDesktopServices::openUrl(url))
                emit linkOpened(target);
            else
                qWarning() << "Could not open terminal link:" << url;
        }
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
    if (!interactive_ || !selecting_) {
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
    event->accept();
    const bool program = document_->snapshot().alternate_screen && !document_->historyActive();
    if (program != wheel_program_) {
        wheel_remainder_ = 0;
        pixel_remainder_ = 0;
        wheel_program_ = program;
    }
    if (program) {
        wheel_remainder_ += event->angleDelta().y();
        const int steps = wheel_remainder_ / 120;
        wheel_remainder_ -= steps * 120;
        if (steps != 0)
            scrollProgram(steps, cellAt(event->position()));
        return;
    }
    // Kept history scrolls by rows, as in other terminals: a trackpad's
    // pixels a row at a time, a wheel's notch three rows. Positive is back.
    int rows = 0;
    const auto grid = cellGrid();
    if (!event->pixelDelta().isNull() && grid && grid->height > 0) {
        pixel_remainder_ += event->pixelDelta().y();
        rows = static_cast<int>(pixel_remainder_ / grid->height);
        pixel_remainder_ -= rows * grid->height;
    } else {
        wheel_remainder_ += event->angleDelta().y();
        rows = wheel_remainder_ / 40;
        wheel_remainder_ -= rows * 40;
    }
    if (rows != 0)
        document_->scrollHistory(rows);
}
// Full-screen programs scroll themselves: the service sends the wheel as the
// program asked (mouse wheel events when it reports the mouse, as Claude
// Code's full-screen mode does, else arrow keys). A service from before wheel
// input gets arrow keys from here. Positive steps scroll back.
void TerminalSurface::scrollProgram(int steps, QPoint cell) {
    if (!acceptsTerminalInput())
        return;
    if (document_->snapshot().accepts_wheel) {
        document_->sendWheel(steps, cell.x(), cell.y());
        return;
    }
    const auto key = steps > 0 ? session::TerminalKey::up : session::TerminalKey::down;
    for (int line = 0; line < std::abs(steps) * 3; ++line)
        document_->sendKey(key, {});
}
std::optional<TerminalSurface::CellGrid> TerminalSurface::cellGrid() const {
    if (!document_)
        return std::nullopt;
    const auto& snapshot = document_->snapshot();
    const auto size = snapshot.size;
    if (size.columns == 0 || size.rows == 0 || width() <= 0 || height() <= 0)
        return std::nullopt;
    const QFontMetricsF metrics(cellFont());
    const qreal cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
    const qreal row_height = metrics.height() + 3;
    const auto layout = surface_layout(snapshot, QSizeF(width(), height()), minimum_scale_,
                                       QSizeF(cell_width, row_height));
    return CellGrid{cell_width * layout.scale, row_height * layout.scale, snapshot.size.columns,
                    snapshot.size.rows,        layout.first_row,          layout.scale};
}
QPoint TerminalSurface::cellAt(QPointF position) const {
    const auto grid = cellGrid();
    if (!grid)
        return {};
    return {std::clamp(static_cast<int>(position.x() / grid->width), 0, grid->columns - 1),
            std::clamp(static_cast<int>(position.y() / grid->height + grid->first_row), 0,
                       grid->rows - 1)};
}
QRectF TerminalSurface::cellRect(int column, int row) const {
    const auto grid = cellGrid();
    if (!grid)
        return {};
    return {column * grid->width, (row - grid->first_row) * grid->height, grid->width,
            grid->height};
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
            text.empty() || snapshot.cells[index].style.invisible
                ? QStringLiteral(" ")
                : QString::fromUcs4(text.data(), static_cast<qsizetype>(text.size()));
        for (qsizetype unit = 0; unit < value.size(); ++unit)
            result.columns.append(column);
        result.text += value;
    }
    return result;
}

// Every case-insensitive occurrence of `needle` in one row, as columns.
std::vector<TerminalMatch> row_matches(const session::TerminalSnapshot& snapshot, int row,
                                       const QString& needle) {
    std::vector<TerminalMatch> matches;
    const auto line = row_text(snapshot, row);
    for (auto at = line.text.indexOf(needle, 0, Qt::CaseInsensitive); at >= 0;
         at = line.text.indexOf(needle, at + 1, Qt::CaseInsensitive))
        matches.push_back({row, line.columns[at], line.columns[at + needle.size() - 1]});
    return matches;
}

bool printable_url_character(char32_t value) {
    return QChar::isPrint(value) && QChar::category(value) != QChar::Other_Format;
}
} // namespace

std::optional<TerminalMatch> terminal_find(const session::TerminalSnapshot& snapshot,
                                           const QString& needle, QPoint from, bool backwards) {
    const int rows = snapshot.size.rows;
    if (needle.isEmpty() || rows <= 0 || snapshot.size.columns <= 0)
        return std::nullopt;
    const int step = backwards ? -1 : 1;
    for (int row = std::clamp(from.y(), 0, rows - 1); row >= 0 && row < rows; row += step) {
        auto matches = row_matches(snapshot, row, needle);
        if (backwards)
            std::reverse(matches.begin(), matches.end());
        for (const auto& match : matches) {
            const bool after = row > from.y() || match.first_column > from.x();
            const bool before = row < from.y() || match.first_column < from.x();
            if (backwards ? before : after)
                return match;
        }
    }
    return std::nullopt;
}

void TerminalSurface::setSuggestion(const QString& suggestion) {
    if (suggestion_ == suggestion)
        return;
    suggestion_ = suggestion;
    typed_while_offered_ = 0;
    emit suggestionChanged();
    publishFrame(false);
    reportSeen();
}

void TerminalSurface::setSuggestionKey(const QString& key) {
    if (suggestion_key_ == key)
        return;
    suggestion_key_ = key;
    emit suggestionChanged();
    reportSeen();
}

void TerminalSurface::setTabFlow(bool enabled) {
    if (tab_flow_ == enabled)
        return;
    tab_flow_ = enabled;
    emit tabFlowChanged();
}

void TerminalSurface::setTabAway(const QJSValue& move) {
    tab_away_ = move;
    emit tabFlowChanged();
}

// A suggestion counts as seen once it is on screen in the active window, once
// per offer: the same words offered again after another turn are a new offer.
void TerminalSurface::reportSeen() {
    const auto& key = suggestion_key_.isEmpty() ? suggestion_ : suggestion_key_;
    if (suggestion_.isEmpty() || seen_ == key || !isVisible() || !window() || !window()->isActive())
        return;
    seen_ = key;
    emit suggestionSeen();
}

// Input the person sent the agent: it counts against a pending Tab's Return,
// which then waits for them instead.
void TerminalSurface::noteTyped() {
    typed_since_arrival_ = true;
    if (!suggestion_.isEmpty())
        ++typed_while_offered_;
    if (submit_owner_ && submit_owner_ == document_)
        submit_timer_.stop();
}

bool TerminalSurface::suggestionWhole() const {
    if (!document_ || suggestion_.isEmpty())
        return false;
    const QFontMetricsF metrics(
        terminal_font(use_system_font_ ? QString() : resolved_font_family_, font_pixel_size_));
    return lay_out_suggestion(document_->snapshot(), metrics, suggestion_).whole;
}

// With the Tab flow on (an agent lapis guesses for), Tab sends the offered
// suggestion when it shows whole: typed as a paste, then Return once it
// settled, as the phone does, to that agent even if Tab moved on meanwhile,
// unless the person typed to it first. Otherwise, and with Option-Tab, Tab
// only types it. With nothing offered and nothing typed since arriving, Tab
// moves to the next agent that needs you, and is the program's own when none
// does. Typing keeps the suggestion; what was typed first is counted.
bool TerminalSurface::takeSuggestion(const QKeyEvent& event) {
    if (!tab_flow_ || !document_ || modifier_key(event.key()))
        return false;
    const auto modifiers = event.modifiers() & ~Qt::KeypadModifier;
    const bool tab = event.key() == Qt::Key_Tab && modifiers == Qt::NoModifier;
    const bool fill = event.key() == Qt::Key_Tab && modifiers == Qt::AltModifier;
    // Never over a request: Return in a permission dialog would answer it.
    const bool offered = !suggestion_.isEmpty() && !document_->attentionPending();
    if (offered && (tab || fill)) {
        const bool send = tab && suggestionWhole();
        const int typed_first = typed_while_offered_;
        if (!pasteText(suggestion_))
            return true;
        setSuggestion({});
        typed_since_arrival_ = !send;
        if (send) {
            submit_owner_ = document_;
            submit_timer_.start(kSubmitAfterPasteMs);
        }
        emit suggestionUsed(send, typed_first);
        return true;
    }
    if (tab && !typed_since_arrival_ && tab_away_.isCallable() && tab_away_.call().toBool())
        return true;
    // Keys that reach the agent, including Command-Delete and friends.
    if (!modifiers.testFlag(Qt::MetaModifier) || event.key() == Qt::Key_Backspace ||
        event.key() == Qt::Key_Delete || event.key() == Qt::Key_Left ||
        event.key() == Qt::Key_Right)
        noteTyped();
    return false;
}

bool TerminalSurface::pasteText(const QString& text) {
    if (!document_ || text.isEmpty() || !interactive_ || !document_->live() || pasting_)
        return false;
    if (document_->historyActive())
        document_->returnToLive();
    if (!acceptsTerminalInput())
        return false;
    const auto owner = document_;
    pasting_ = true;
    emit inputOwnershipChanged();
    const auto release_paste = qScopeGuard([this] {
        pasting_ = false;
        emit inputOwnershipChanged();
    });
    ++ime_epoch_;
    resetInputContext();
    // Resetting the input context can re-enter input handling. Keep the paste
    // bound to its captured document and send only when that destination still
    // owns the keyboard after the reset.
    if (document_ != owner || !acceptsTerminalInput())
        return false;
    clearSelection();
    document_->sendText(text.toUtf8(), true);
    return true;
}

QString TerminalSurface::localFilePath(const QString& url) const {
    if (url.contains(u'\0'))
        return {};
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.scheme() != QLatin1String("file") ||
        !parsed.path().startsWith(u'/') || parsed.port() != -1 || !parsed.userName().isEmpty() ||
        !parsed.password().isEmpty() || parsed.hasQuery() || parsed.hasFragment())
        return {};
    if (!valid_file_url_path(parsed))
        return {};
    const QString path = parsed.toLocalFile();
    return path.isEmpty() || path.contains(u'\0') ? QString() : path;
}

bool TerminalSurface::findText(const QString& text, bool backwards) {
    if (!document_)
        return false;
    const auto& snapshot = document_->snapshot();
    const QPoint start =
        selection_anchor_
            ? *selection_anchor_
            : (backwards ? QPoint(snapshot.size.columns, snapshot.size.rows - 1) : QPoint(-1, 0));
    const auto match = terminal_find(snapshot, text, start, backwards);
    if (!match)
        return false;
    setSelection(QPoint(match->first_column, match->row), QPoint(match->last_column, match->row));
    return true;
}

int TerminalSurface::countMatches(const QString& text) const {
    if (!document_ || text.isEmpty())
        return 0;
    const auto& snapshot = document_->snapshot();
    int count = 0;
    for (int row = 0; row < snapshot.size.rows; ++row)
        count += static_cast<int>(row_matches(snapshot, row, text).size());
    return count;
}

namespace {
// The rows a cell's line runs over, joined: a row that runs to the last column
// is treated as wrapping onto the next one, which is how agents print long
// links in a narrow terminal. Each unit of the text keeps its cell.
struct JoinedRows {
    QString text;
    std::vector<QPoint> cells; // (column, row) of each unit
    qsizetype target{-1};      // the unit of the asked-for cell
};
JoinedRows joined_rows(const session::TerminalSnapshot& snapshot, int column, int row) {
    JoinedRows joined;
    const auto fills = [](const RowText& line) { return !line.text.endsWith(QLatin1Char(' ')); };
    int first = row;
    while (first > 0 && fills(row_text(snapshot, first - 1)))
        --first;
    for (int current = first; current < snapshot.size.rows; ++current) {
        const auto line = row_text(snapshot, current);
        for (qsizetype unit = 0; unit < line.columns.size(); ++unit) {
            if (current == row && joined.target < 0 && line.columns[unit] == column)
                joined.target = joined.text.size() + unit;
            joined.cells.emplace_back(line.columns[unit], current);
        }
        joined.text += line.text;
        if (current >= row && !fills(line))
            break;
    }
    return joined;
}
// The cells a stretch of the joined text covers, row by row.
std::vector<TerminalMatch> covered(const JoinedRows& joined, qsizetype start, qsizetype length) {
    std::vector<TerminalMatch> cells;
    for (auto unit = start; unit < start + length; ++unit) {
        const auto cell = joined.cells[static_cast<std::size_t>(unit)];
        if (!cells.empty() && cells.back().row == cell.y())
            cells.back().last_column = std::max(cells.back().last_column, cell.x());
        else
            cells.push_back({cell.y(), cell.x(), cell.x()});
    }
    return cells;
}
// Where a written path ends: spaces, quotes, brackets and list punctuation, as
// in Claude Code's Update(docs/a.md) or a quoted '~/x'.
bool path_delimiter(QChar value) {
    return value.isSpace() || QStringLiteral("\"'`()[]{}<>|,;").contains(value);
}
bool printable(const QString& text) {
    const auto codepoints = text.toUcs4();
    return std::all_of(codepoints.cbegin(), codepoints.cend(), printable_url_character);
}
std::vector<TerminalMatch> hyperlink_cells(const session::TerminalSnapshot& snapshot,
                                           const session::TerminalHyperlink& span) {
    std::vector<TerminalMatch> cells;
    for (std::size_t at = span.first_cell; at < span.first_cell + span.cell_count; ++at) {
        if (snapshot.cells[at].style.invisible ||
            snapshot.cells[at].kind == session::CellKind::wrap_spacer)
            continue;
        const int y = static_cast<int>(at / snapshot.size.columns);
        const int x = static_cast<int>(at % snapshot.size.columns);
        if (!cells.empty() && cells.back().row == y && cells.back().last_column + 1 == x)
            cells.back().last_column = x;
        else
            cells.push_back({y, x, x});
    }
    return cells;
}
std::optional<TerminalLink> explicit_link(const session::TerminalSnapshot& snapshot,
                                          const session::TerminalHyperlink& span,
                                          std::size_t index) {
    if (span.first_cell >= snapshot.cells.size() ||
        span.cell_count > snapshot.cells.size() - span.first_cell ||
        snapshot.cells[index].style.invisible ||
        snapshot.cells[index].kind == session::CellKind::wrap_spacer)
        return std::nullopt;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString uri =
        decoder(QByteArrayView(span.uri.data(), static_cast<qsizetype>(span.uri.size())));
    const QUrl url(uri, QUrl::StrictMode);
    const bool web =
        url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http");
    if (decoder.hasError() || !printable(uri) || !url.isValid() || (!web && !url.isLocalFile()) ||
        (web && url.host().isEmpty()))
        return std::nullopt;
    return TerminalLink{web ? TerminalLink::Kind::url : TerminalLink::Kind::path, uri, 0,
                        hyperlink_cells(snapshot, span)};
}
std::optional<TerminalLink> visible_link(const JoinedRows& joined) {
    const auto target = joined.target;
    if (target < 0)
        return std::nullopt;
    static const QRegularExpression pattern(QStringLiteral(R"((?:https?://|www\.)[^\s<>"'`]+)"),
                                            QRegularExpression::CaseInsensitiveOption);
    for (auto matches = pattern.globalMatch(joined.text); matches.hasNext();) {
        const auto match = matches.next();
        auto url = match.captured();
        // Sentence punctuation and unbalanced closing brackets end the link.
        while (!url.isEmpty() && QStringLiteral(".,;:!?)]}").contains(url.back()) &&
               !(url.back() == QLatin1Char(')') &&
                 url.count(QLatin1Char('(')) > url.count(QLatin1Char(')')) - 1))
            url.chop(1);
        if (!printable(url))
            continue;
        if (target >= match.capturedStart() && target < match.capturedStart() + url.size())
            return TerminalLink{TerminalLink::Kind::url,
                                url.startsWith(QLatin1String("www."), Qt::CaseInsensitive)
                                    ? QStringLiteral("https://") + url
                                    : url,
                                0, covered(joined, match.capturedStart(), url.size())};
    }
    // Otherwise the word under the cell, which may name a file or folder.
    const auto& text = joined.text;
    if (path_delimiter(text[target]))
        return std::nullopt;
    auto start = target;
    auto end = target + 1;
    while (start > 0 && !path_delimiter(text[start - 1]))
        --start;
    while (end < text.size() && !path_delimiter(text[end]))
        ++end;
    // Sentence punctuation after a path is not part of it.
    while (end > start && QStringLiteral(".:!?").contains(text[end - 1]))
        --end;
    if (target >= end || end - start > 1024)
        return std::nullopt;
    auto word = text.mid(start, end - start);
    // file.cpp:12 or file.cpp:12:3 names a line of the file.
    int line = 0;
    static const QRegularExpression location(QStringLiteral(R"(^(.+?):(\d+)(?::\d+)?$)"));
    if (const auto found = location.match(word); found.hasMatch()) {
        word = found.captured(1);
        line = found.captured(2).toInt();
    }
    if (!printable(word))
        return std::nullopt;
    return TerminalLink{TerminalLink::Kind::path, word, line, covered(joined, start, end - start)};
}
} // namespace

std::optional<TerminalLink> terminal_link_at(const session::TerminalSnapshot& snapshot, int column,
                                             int row) {
    if (row < 0 || row >= snapshot.size.rows || column < 0 || column >= snapshot.size.columns)
        return std::nullopt;
    const auto index =
        static_cast<std::size_t>(row) * snapshot.size.columns + static_cast<std::size_t>(column);
    // An explicit but unsupported OSC 8 destination must not open its label.
    for (const auto& span : snapshot.hyperlinks)
        if (index >= span.first_cell && index - span.first_cell < span.cell_count)
            return explicit_link(snapshot, span, index);
    return visible_link(joined_rows(snapshot, column, row));
}

QString terminal_url_at(const session::TerminalSnapshot& snapshot, int column, int row) {
    const auto link = terminal_link_at(snapshot, column, row);
    return link && link->kind == TerminalLink::Kind::url ? link->text : QString();
}

QString resolve_terminal_path(const QString& written, const QString& folder) {
    if (written.isEmpty())
        return {};
    QString path = written;
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    else if (path.startsWith(QLatin1String("file://")))
        path = QUrl(path).toLocalFile();
    if (QDir::isRelativePath(path)) {
        if (folder.isEmpty())
            return {};
        path = QDir(folder).filePath(path);
    }
    const QFileInfo info(QDir::cleanPath(path));
    return info.exists() ? info.absoluteFilePath() : QString();
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
    if (event->key() == kLinkKey)
        updateLink(hover_position_, event->modifiers() | kLinkModifier);
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
    keepFramesComing();
    // New input replaces what was selected; a modifier alone does not.
    if (!modifier_key(event->key()))
        clearSelection();
    if (composition_state_ == CompositionState::stale)
        composition_state_ = CompositionState::idle;
    if (event->matches(QKeySequence::Paste)) {
        noteTyped();
        const QString text = QGuiApplication::clipboard()->text();
        static_cast<void>(pasteText(text));
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
    if (takeSuggestion(*event)) {
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
#ifdef Q_OS_MACOS
    // Command-Backspace deletes to the line start (Ctrl-U) and
    // Command-Delete to the line end (Ctrl-K), as in iTerm2's natural text
    // editing and the shells' emacs bindings.
    if (bare_command && (event.key() == Qt::Key_Backspace || event.key() == Qt::Key_Delete)) {
        document_->sendText(QByteArray(1, event.key() == Qt::Key_Backspace ? '\x15' : '\x0b'));
        event.accept();
        return;
    }
#endif
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
    keepFramesComing();
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
        noteTyped();
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
    // Keys go straight to a program: no predictions, completion or
    // corrections for the platform to type in on the user's behalf.
    if (query == Qt::ImHints)
        return static_cast<int>(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    if (query == Qt::ImCursorRectangle && document_) {
        const auto& snapshot = document_->snapshot();
        const auto grid = cellGrid();
        if (!snapshot.cursor.in_viewport || !grid)
            return QRectF();
        const QFontMetricsF metrics(cellFont());
        return QRectF(snapshot.cursor.column * grid->width,
                      (snapshot.cursor.row - grid->first_row) * grid->height, 2 * grid->scale,
                      metrics.height() * grid->scale);
    }
    return QQuickItem::inputMethodQuery(query);
}

} // namespace lapis::desktop
