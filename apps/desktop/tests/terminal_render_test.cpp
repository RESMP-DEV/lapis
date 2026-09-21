#include "terminal_surface.hpp"

#include <lapis/session/terminal.hpp>

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QSGRendererInterface>
#include <QString>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using lapis::desktop::SessionPreview;
using lapis::desktop::TerminalSurface;
namespace session = lapis::session;

constexpr std::uint32_t page_background = 0x0d131dU;
constexpr std::uint32_t default_foreground = 0xd9dee8U;
constexpr std::uint32_t blank_background = 0x237d64U;
constexpr std::uint32_t red_underline = 0xff2a2aU;
constexpr std::uint32_t yellow_underline = 0xffd166U;
constexpr std::uint32_t blue_underline = 0x2ad4ffU;
constexpr std::uint32_t strike_foreground = 0xef476fU;

std::uint32_t rgb_color(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return (static_cast<std::uint32_t>(red) << 16U) | (static_cast<std::uint32_t>(green) << 8U) |
           static_cast<std::uint32_t>(blue);
}

void require(bool condition, const char* expression, int line) {
    if (!condition)
        throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(condition) require((condition), #condition, __LINE__)

void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

struct ColorArea {
    QRect bounds;
    int pixels{};
    QPoint center{};
};

QRgb qt_color(std::uint32_t rgb) {
    return qRgb(static_cast<int>((rgb >> 16U) & 0xffU), static_cast<int>((rgb >> 8U) & 0xffU),
                static_cast<int>(rgb & 0xffU));
}

ColorArea color_area(const QImage& image, std::uint32_t expected_rgb) {
    const QRgb expected = qt_color(expected_rgb);
    ColorArea area;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) != expected)
                continue;
            area.bounds = area.bounds.united(QRect(x, y, 1, 1));
            area.center += QPoint(x, y);
            ++area.pixels;
        }
    }
    if (area.pixels != 0)
        area.center /= area.pixels;
    return area;
}

std::string describe_area(const ColorArea& area) {
    return "(" + std::to_string(area.bounds.left()) + "," + std::to_string(area.bounds.top()) +
           " to " + std::to_string(area.bounds.right()) + "," +
           std::to_string(area.bounds.bottom()) + ", pixels=" + std::to_string(area.pixels) + ")";
}

void require_pixels(const ColorArea& area, std::string_view label, int minimum_pixels) {
    if (area.pixels < minimum_pixels)
        throw std::runtime_error(std::string(label) +
                                 " has too few pixels: " + describe_area(area));
}

QImage render(TerminalSurface& surface) {
    // Allow a noninteractive preview refresh (10 Hz) and its next presentation.
    pump(150);
    const QImage image = surface.window()->grabWindow();
    CHECK(!image.isNull());
    return image;
}

QFont terminal_font() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(16);
    font.setStyleHint(QFont::Monospace);
    return font;
}

struct CellRectangle {
    qreal left{};
    qreal right{};
    qreal top{};
    qreal bottom{};
};

class RenderGeometry {
  public:
    RenderGeometry(const session::TerminalSnapshot& snapshot, const QSizeF& surface_size,
                   int capture_width, int logical_width, const ColorArea& captured_page) {
        const QFontMetricsF metrics(terminal_font());
        const qreal font_cell_width = metrics.horizontalAdvance(QLatin1Char('M'));
        const qreal font_row_height = metrics.height() + 3;
        const qreal scale =
            std::min(surface_size.width() / (snapshot.size.columns * font_cell_width),
                     surface_size.height() / (snapshot.size.rows * font_row_height));
        const qreal capture_scale = static_cast<qreal>(capture_width) / logical_width;
        const int expected_width = static_cast<int>(
            std::lround(snapshot.size.columns * font_cell_width * scale * capture_scale));
        const int expected_height = static_cast<int>(
            std::lround(snapshot.size.rows * font_row_height * scale * capture_scale));
        const int edge_tolerance = std::max(2, static_cast<int>(std::ceil(2 * capture_scale)));
        if (std::abs(captured_page.bounds.width() - expected_width) > edge_tolerance ||
            std::abs(captured_page.bounds.height() - expected_height) > edge_tolerance)
            throw std::runtime_error("terminal page geometry disagrees with font-derived geometry");
        origin_ = captured_page.bounds.topLeft();
        cell_width_ = font_cell_width * scale * capture_scale;
        row_height_ = font_row_height * scale * capture_scale;
    }

    [[nodiscard]] CellRectangle cell(QPoint position, int column_span = 1) const {
        const qreal left = origin_.x() + static_cast<qreal>(position.x()) * cell_width_;
        const qreal top = origin_.y() + static_cast<qreal>(position.y()) * row_height_;
        return {left, left + static_cast<qreal>(column_span) * cell_width_, top, top + row_height_};
    }

  private:
    QPointF origin_;
    qreal cell_width_{};
    qreal row_height_{};
};

void require_horizontal_edges(const ColorArea& area, const CellRectangle& rectangle,
                              std::string_view label) {
    const int left = area.bounds.left();
    const int right = area.bounds.right();
    const int expected_left = static_cast<int>(std::lround(rectangle.left));
    const int expected_right = static_cast<int>(std::lround(rectangle.right)) - 1;
    if (left < expected_left - 2 || left > expected_left + 2 || right < expected_right - 2 ||
        right > expected_right + 2)
        throw std::runtime_error(
            std::string(label) + " horizontal edges are misaligned: expected [" +
            std::to_string(expected_left) + "," + std::to_string(expected_right) + "], actual [" +
            std::to_string(left) + "," + std::to_string(right) + "]");
}

std::uint32_t cell_background(const session::TerminalSnapshot& snapshot, int column, int row) {
    return snapshot.cell_background(static_cast<std::size_t>(row) * snapshot.size.columns +
                                    static_cast<std::size_t>(column));
}

void check_background_edges(const QImage& image, const session::TerminalSnapshot& snapshot,
                            const RenderGeometry& geometry, int row) {
    int column = 0;
    while (column < snapshot.size.columns) {
        const std::uint32_t background = cell_background(snapshot, column, row);
        int end = column + 1;
        while (end < snapshot.size.columns && cell_background(snapshot, end, row) == background)
            ++end;
        if (background != page_background) {
            const ColorArea area = color_area(image, background);
            require_pixels(area, "styled background marker", 8);
            require_horizontal_edges(area, geometry.cell({column, row}, end - column),
                                     "styled background marker");
            const CellRectangle rectangle = geometry.cell({column, row}, end - column);
            CHECK(area.bounds.top() >= static_cast<int>(std::floor(rectangle.top)) - 2);
            CHECK(area.bounds.bottom() <= static_cast<int>(std::ceil(rectangle.bottom)) + 2);
        }
        column = end;
    }
}

bool visually_distinct(QRgb left, QRgb right) {
    return std::abs(qRed(left) - qRed(right)) > 24 || std::abs(qGreen(left) - qGreen(right)) > 24 ||
           std::abs(qBlue(left) - qBlue(right)) > 24;
}

ColorArea glyph_area(const QImage& image, const CellRectangle& rectangle,
                     std::uint32_t cell_background_rgb,
                     const std::vector<std::uint32_t>& known_backgrounds) {
    const QRgb background = qt_color(cell_background_rgb);
    const qreal inset = (rectangle.right - rectangle.left) * 0.1;
    const int left = std::max(0, static_cast<int>(std::floor(rectangle.left + inset)));
    const int right =
        std::min(image.width() - 1, static_cast<int>(std::ceil(rectangle.right - inset)) - 1);
    const int top = std::max(0, static_cast<int>(std::floor(rectangle.top)));
    const int bottom =
        std::min(image.height() - 1, static_cast<int>(std::ceil(rectangle.bottom)) - 1);
    ColorArea area;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const QRgb color = image.pixel(x, y);
            if (!visually_distinct(color, background))
                continue;
            if (std::any_of(known_backgrounds.begin(), known_backgrounds.end(),
                            [color](std::uint32_t known) { return color == qt_color(known); }))
                continue;
            area.bounds = area.bounds.united(QRect(x, y, 1, 1));
            area.center += QPoint(x, y);
            ++area.pixels;
        }
    }
    if (area.pixels != 0)
        area.center /= area.pixels;
    return area;
}

std::vector<std::uint32_t> snapshot_backgrounds(const session::TerminalSnapshot& snapshot) {
    std::vector<std::uint32_t> backgrounds{page_background};
    for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
        const std::uint32_t background = snapshot.cell_background(index);
        if (std::find(backgrounds.begin(), backgrounds.end(), background) == backgrounds.end())
            backgrounds.push_back(background);
    }
    return backgrounds;
}

void check_glyph_anchor(const QImage& image, const session::TerminalSnapshot& snapshot,
                        const RenderGeometry& geometry, int column, int row,
                        std::string_view label) {
    const CellRectangle rectangle = geometry.cell({column, row});
    const ColorArea area = glyph_area(image, rectangle, cell_background(snapshot, column, row),
                                      snapshot_backgrounds(snapshot));
    require_pixels(area, label, 6);
    const qreal center_x = static_cast<qreal>(area.center.x());
    const qreal center_y = static_cast<qreal>(area.center.y());
    CHECK(center_x >= rectangle.left - 2);
    CHECK(center_x <= rectangle.right + 2);
    CHECK(center_y >= rectangle.top - 2);
    CHECK(center_y <= rectangle.bottom + 2);
}

ColorArea restricted_color_area(const QImage& image, const CellRectangle& rectangle,
                                std::uint32_t expected_rgb) {
    const int x = std::max(0, static_cast<int>(std::floor(rectangle.left)));
    const int y = std::max(0, static_cast<int>(std::floor(rectangle.top)));
    const int width =
        std::min(image.width() - x, static_cast<int>(std::ceil(rectangle.right - rectangle.left)));
    const int height =
        std::min(image.height() - y, static_cast<int>(std::ceil(rectangle.bottom - rectangle.top)));
    if (width <= 0 || height <= 0)
        return {};
    ColorArea area = color_area(image.copy(x, y, width, height), expected_rgb);
    area.bounds.translate(x, y);
    area.center += QPoint(x, y);
    return area;
}

std::vector<QRect> horizontal_color_runs(const QImage& image, const CellRectangle& rectangle,
                                         std::uint32_t expected_rgb) {
    std::vector<QRect> runs;
    for (int y = std::max(0, static_cast<int>(std::ceil(rectangle.top) + 1));
         y <= std::min(image.height() - 1, static_cast<int>(std::floor(rectangle.bottom) - 2));
         ++y) {
        QRect run;
        for (int x = std::max(0, static_cast<int>(std::ceil(rectangle.left) + 1));
             x <= std::min(image.width() - 1, static_cast<int>(std::floor(rectangle.right) - 2));
             ++x) {
            if (image.pixel(x, y) == qt_color(expected_rgb))
                run = run.united(QRect(x, y, 1, 1));
        }
        if (!run.isNull()) {
            if (!runs.empty() && runs.back().bottom() == y - 1)
                runs.back() = runs.back().united(run);
            else
                runs.push_back(run);
        }
    }
    return runs;
}

class GhosttyScenario {
  public:
    GhosttyScenario() : terminal_({14, 4}) {
        terminal_.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\");
        terminal_.feed("\x1b[48;2;229;20;3m米\x1b[48;2;14;173;91mA"
                       "\x1b[48;2;31;94;223mm\xcc\x81\x1b[0m"
                       "\x1b[48;2;35;125;100m  \x1b[1;48;2;255;105;180mB"
                       "\x1b[48;2;153;102;255mI\x1b[0m\r\n");
        terminal_.feed("\x1b[48;2;255;159;064m😀\x1b[48;2;26;188;156m!"
                       "\x1b[48;2;52;152;219mש\x1b[48;2;155;089;182mע"
                       "\x1b[48;2;241;196;050mר\x1b[0m  "
                       "\x1b[48;2;189;195;199mا\x1b[48;2;149;165;166mب\x1b[0m\r\n");
        terminal_.feed("\x1b[48;2;88;101;242mש\x1b[48;2;255;184;108mל"
                       "\x1b[48;2;139;195;74mו\x1b[48;2;255;121;198mם\x1b[0m "
                       "\x1b[3;48;2;171;71;188mا\x1b[0m"
                       "\x1b[4;48;2;0;184;169mب\x1b[0m\r\n");
    }

    [[nodiscard]] session::TerminalSnapshot snapshot() { return terminal_.snapshot(); }
    void resize(session::TerminalSize size) { terminal_.resize(size); }

  private:
    session::Terminal terminal_;
};

session::TerminalSnapshot decoration_snapshot() {
    session::Terminal terminal({14, 3});
    terminal.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\");
    terminal.feed("\r\n");
    terminal.feed("\x1b[4:2;58;2;255;42;42m \x1b[4:0;58;2;255;209;102m "
                  "\x1b[4;58;2;42;212;255m \x1b[0m");
    terminal.feed("\x1b[38;2;239;71;111;9m \x1b[0m");
    terminal.feed("\x1b[38;2;23;249;84;8;9;4m \x1b[0m");
    terminal.feed("\x1b[7;4m \x1b[0m");
    terminal.feed("\x1b[4m \x1b[0m");
    terminal.feed("\x1b[38;2;163;42;219;53m \x1b[0m");
    return terminal.snapshot();
}

session::TerminalSnapshot blank_snapshot() {
    session::Terminal terminal({12, 3});
    terminal.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\");
    terminal.feed("\x1b[0m\r\n\r\n\r\n");
    return terminal.snapshot();
}

void check_snapshot_contract(const session::TerminalSnapshot& snapshot) {
    CHECK((snapshot.size == session::TerminalSize{14, 4}));
    CHECK(snapshot.cells.size() == 56);
    CHECK(cell_background(snapshot, 0, 0) == rgb_color(0xe5, 0x14, 0x03));
    CHECK(snapshot.cells[0].kind == session::CellKind::wide);
    CHECK(snapshot.cells[1].kind == session::CellKind::wide_tail);
    CHECK(cell_background(snapshot, 2, 0) == rgb_color(0x0e, 0xad, 0x5b));
    CHECK(cell_background(snapshot, 3, 0) == rgb_color(0x1f, 0x5e, 0xdf));
    CHECK(snapshot.cells[3].text_length >= 2);
    CHECK(cell_background(snapshot, 4, 0) == blank_background);
    CHECK(cell_background(snapshot, 5, 0) == blank_background);
    CHECK(cell_background(snapshot, 6, 0) == rgb_color(0xff, 0x69, 0xb4));
    CHECK(snapshot.cells[6].style.bold);
    CHECK(cell_background(snapshot, 7, 0) == rgb_color(0x99, 0x66, 0xff));
    CHECK(cell_background(snapshot, 8, 0) == page_background);
    CHECK(cell_background(snapshot, 0, 1) == rgb_color(0xff, 0x9f, 0x40));
    CHECK(snapshot.cells[14].kind == session::CellKind::wide);
    CHECK(cell_background(snapshot, 2, 1) == rgb_color(0x1a, 0xbc, 0x9c));
    CHECK(cell_background(snapshot, 3, 1) == rgb_color(0x34, 0x98, 0xdb));
    CHECK(cell_background(snapshot, 4, 1) == rgb_color(0x9b, 0x59, 0xb6));
    CHECK(cell_background(snapshot, 5, 1) == rgb_color(0xf1, 0xc4, 0x32));
    CHECK(cell_background(snapshot, 8, 1) == rgb_color(0xbd, 0xc3, 0xc7));
    CHECK(cell_background(snapshot, 9, 1) == rgb_color(0x95, 0xa5, 0xa6));
    CHECK(cell_background(snapshot, 0, 2) == rgb_color(0x58, 0x65, 0xf2));
    CHECK(cell_background(snapshot, 1, 2) == rgb_color(0xff, 0xb8, 0x6c));
    CHECK(cell_background(snapshot, 2, 2) == rgb_color(0x8b, 0xc3, 0x4a));
    CHECK(cell_background(snapshot, 3, 2) == rgb_color(0xff, 0x79, 0xc6));
    CHECK(cell_background(snapshot, 5, 2) == rgb_color(0xab, 0x47, 0xbc));
    CHECK(snapshot.cells[33].style.italic);
    CHECK(cell_background(snapshot, 6, 2) == rgb_color(0x00, 0xb8, 0xa9));
    CHECK(snapshot.cells[34].style.underline == session::Underline::single);
    CHECK(cell_background(snapshot, 7, 2) == page_background);
}

void check_decoration_contract(const session::TerminalSnapshot& snapshot) {
    CHECK((snapshot.size == session::TerminalSize{14, 3}));
    CHECK(snapshot.cells.size() == 42);
    CHECK(snapshot.cells[14].style.underline == session::Underline::double_line);
    CHECK(snapshot.cells[14].style.underline_color.value == red_underline);
    CHECK(snapshot.cells[14].text_length == 1);
    CHECK(snapshot.cells[15].style.underline == session::Underline::none);
    CHECK(snapshot.cells[15].style.underline_color.value == yellow_underline);
    CHECK(snapshot.cells[15].text_length == 1);
    CHECK(snapshot.cells[16].style.underline == session::Underline::single);
    CHECK(snapshot.cells[16].style.underline_color.value == blue_underline);
    CHECK(snapshot.cells[16].text_length == 1);
    CHECK(snapshot.cells[17].style.strikethrough);
    CHECK(snapshot.cells[17].text_length == 1);
    CHECK(snapshot.cells[18].style.invisible);
    CHECK(snapshot.cells[19].style.inverse);
    CHECK(snapshot.cell_background(19) == snapshot.foreground_rgb);
    CHECK(snapshot.cell_foreground(19) == snapshot.background_rgb);
    CHECK(snapshot.cells[20].style.underline == session::Underline::single);
    CHECK(!snapshot.cells[20].style.inverse);
    CHECK(snapshot.cells[21].style.overline);
}

struct RenderFixture {
    QQuickWindow window;
    SessionPreview document;
    TerminalSurface surface;

    explicit RenderFixture(const session::TerminalSnapshot& snapshot)
        : window(), document(QStringLiteral("renderer"), QStringLiteral("."),
                             QStringLiteral("idle"), QColor(Qt::black), std::string_view{}),
          surface() {
        window.setGeometry(80, 120, 620, 220);
        window.show();
        window.requestActivate();
        pump(100);
        surface.setParentItem(window.contentItem());
        surface.setSize(QSizeF(560, 180));
        surface.setDocument(&document);
        apply(snapshot, snapshot.revision + 1);
        pump(200);
    }

    void apply(const session::TerminalSnapshot& snapshot, std::uint64_t revision) {
        session::TerminalSnapshot updated = snapshot;
        updated.revision = revision;
        updated.cursor.visible = false;
        document.applySnapshot(std::move(updated));
    }
};

int run_renderer_regression() {
    GhosttyScenario scenario;
    session::TerminalSnapshot snapshot = scenario.snapshot();
    check_snapshot_contract(snapshot);

    RenderFixture fixture(snapshot);
    const QImage initial = render(fixture.surface);
    const ColorArea initial_page = color_area(initial, page_background);
    require_pixels(initial_page, "initial Ghostty page", 200);
    const RenderGeometry initial_geometry(snapshot, fixture.surface.size(), initial.width(),
                                          static_cast<int>(fixture.window.width()), initial_page);

    for (int row = 0; row < 3; ++row)
        check_background_edges(initial, snapshot, initial_geometry, row);
    check_glyph_anchor(initial, snapshot, initial_geometry, 0, 0, "CJK wide glyph");
    check_glyph_anchor(initial, snapshot, initial_geometry, 2, 0, "post-wide Latin glyph");
    check_glyph_anchor(initial, snapshot, initial_geometry, 3, 0, "combining grapheme glyph");
    check_glyph_anchor(initial, snapshot, initial_geometry, 6, 0, "bold glyph");
    check_glyph_anchor(initial, snapshot, initial_geometry, 0, 1, "emoji fallback glyph");
    check_glyph_anchor(initial, snapshot, initial_geometry, 8, 1, "Arabic glyph");
    check_glyph_anchor(initial, snapshot, initial_geometry, 0, 2, "Hebrew glyph");
    check_glyph_anchor(initial, snapshot, initial_geometry, 5, 2, "italic Arabic glyph");

    session::TerminalSnapshot decorations = decoration_snapshot();
    check_decoration_contract(decorations);
    RenderFixture decoration_fixture(decorations);
    const QImage decoration_image = render(decoration_fixture.surface);
    const ColorArea decoration_page = color_area(decoration_image, page_background);
    require_pixels(decoration_page, "decoration page", 200);
    const RenderGeometry decoration_geometry(
        decorations, decoration_fixture.surface.size(), decoration_image.width(),
        static_cast<int>(decoration_fixture.window.width()), decoration_page);
    const auto decoration_runs = [&](int column, std::uint32_t color) {
        return horizontal_color_runs(decoration_image, decoration_geometry.cell({column, 1}),
                                     color);
    };
    const std::vector<QRect> double_runs = decoration_runs(0, red_underline);
    CHECK(double_runs.size() == 2);
    for (const auto& run : double_runs) {
        CHECK(run.left() >= static_cast<int>(decoration_geometry.cell({0, 1}).left) - 2);
        CHECK(run.right() <= static_cast<int>(decoration_geometry.cell({1, 1}).left) + 2);
    }
    CHECK(decoration_runs(1, yellow_underline).empty());
    const std::vector<QRect> single_runs = decoration_runs(2, blue_underline);
    CHECK(single_runs.size() == 1);
    CHECK(single_runs.front().left() >=
          static_cast<int>(decoration_geometry.cell({2, 1}).left) - 2);
    CHECK(single_runs.front().right() <=
          static_cast<int>(decoration_geometry.cell({3, 1}).left) + 2);

    const QFontMetricsF metrics(terminal_font());
    const CellRectangle strike_rectangle = decoration_geometry.cell({3, 1});
    const qreal physical_scale =
        (strike_rectangle.bottom - strike_rectangle.top) / (metrics.height() + 3);
    const qreal strike_y =
        strike_rectangle.top + (metrics.ascent() - metrics.strikeOutPos()) * physical_scale;
    const ColorArea strike =
        restricted_color_area(decoration_image, strike_rectangle, strike_foreground);
    require_pixels(strike, "strike foreground", 8);
    CHECK(std::abs(strike.bounds.top() - strike_y) <= 2);
    CHECK(std::abs(strike.bounds.bottom() + 1 - strike_y - physical_scale) <= 2);
    require_horizontal_edges(strike, strike_rectangle, "strike span");

    CHECK(color_area(decoration_image, 0x17f954U).pixels == 0);
    CHECK(color_area(decoration_image, yellow_underline).pixels == 0);
    // Sample the interior so the page outside this inverse cell cannot count
    // as an underline. Its default foreground resolves to the page background.
    CellRectangle inverse_rectangle = decoration_geometry.cell({5, 1});
    inverse_rectangle.left += 3;
    inverse_rectangle.right -= 3;
    const auto inverse_runs =
        horizontal_color_runs(decoration_image, inverse_rectangle, page_background);
    CHECK(inverse_runs.size() == 1);
    CHECK(std::abs(inverse_runs.front().top() -
                   (inverse_rectangle.top + (metrics.ascent() + 1) * physical_scale)) <= 2);
    const auto normal_runs = decoration_runs(6, default_foreground);
    CHECK(normal_runs.size() == 1);
    const ColorArea overline = color_area(decoration_image, 0xa32adbU);
    require_pixels(overline, "overline", 8);
    require_horizontal_edges(overline, decoration_geometry.cell({7, 1}), "overline span");
    CHECK(std::abs(overline.bounds.top() -
                   (decoration_geometry.cell({7, 1}).top + physical_scale)) <= 2);

    scenario.resize({12, 3});
    session::TerminalSnapshot resized = scenario.snapshot();
    CHECK((resized.size == session::TerminalSize{12, 3}));
    CHECK(resized.cells.size() == 36);
    fixture.apply(resized, resized.revision + 1);
    const QImage resized_image = render(fixture.surface);
    const ColorArea resized_page = color_area(resized_image, page_background);
    require_pixels(resized_page, "actual resized Ghostty page", 150);
    const RenderGeometry resized_geometry(resized, fixture.surface.size(), resized_image.width(),
                                          static_cast<int>(fixture.window.width()), resized_page);
    for (int row = 0; row < resized.size.rows; ++row)
        check_background_edges(resized_image, resized, resized_geometry, row);

    const session::TerminalSnapshot blank = blank_snapshot();
    CHECK((blank.size == session::TerminalSize{12, 3}));
    CHECK(blank.cells.size() == 36);
    fixture.apply(blank, blank.revision + 1);
    const QImage blank_image = render(fixture.surface);
    const ColorArea blank_page = color_area(blank_image, page_background);
    require_pixels(blank_page, "blank page", 200);
    const RenderGeometry blank_geometry(blank, fixture.surface.size(), blank_image.width(),
                                        static_cast<int>(fixture.window.width()), blank_page);
    const std::uint32_t old_colors[] = {rgb_color(0xe5, 0x14, 0x03), rgb_color(0x0e, 0xad, 0x5b),
                                        rgb_color(0x1f, 0x5e, 0xdf), blank_background,
                                        rgb_color(0xff, 0x69, 0xb4), rgb_color(0x00, 0xb8, 0xa9)};
    for (const auto color : old_colors) {
        require_pixels(color_area(initial, color), "pre-blank color", 8);
        CHECK(color_area(blank_image, color).pixels == 0);
    }

    session::TerminalSnapshot cursor_snapshot = blank;
    cursor_snapshot.cursor_rgb = 0xff0000U;
    cursor_snapshot.cursor = {.column = 2, .row = 1, .in_viewport = true, .visible = true};
    cursor_snapshot.revision = blank.revision + 2;
    fixture.document.applySnapshot(cursor_snapshot);
    const QImage cursor_image = render(fixture.surface);
    const ColorArea first_cursor = color_area(cursor_image, 0xff0000U);
    require_pixels(first_cursor, "block cursor", 30);
    const CellRectangle first_rectangle = blank_geometry.cell({2, 1});
    CHECK(first_cursor.bounds.left() >= static_cast<int>(first_rectangle.left) - 2);
    CHECK(first_cursor.bounds.right() < first_rectangle.right + 2);
    CHECK(first_cursor.bounds.top() >= static_cast<int>(first_rectangle.top) - 2);
    CHECK(first_cursor.bounds.bottom() < first_rectangle.bottom + 2);

    cursor_snapshot.cursor.column = 8;
    cursor_snapshot.revision = blank.revision + 3;
    fixture.document.applySnapshot(cursor_snapshot);
    const QImage moved_image = render(fixture.surface);
    const ColorArea moved_cursor = color_area(moved_image, 0xff0000U);
    require_pixels(moved_cursor, "repainted cursor", 30);
    const CellRectangle moved_rectangle = blank_geometry.cell({8, 1});
    CHECK(moved_cursor.bounds.left() >= static_cast<int>(moved_rectangle.left) - 2);
    CHECK(moved_cursor.bounds.right() < moved_rectangle.right + 2);
    CHECK(moved_cursor.center.y() == first_cursor.center.y());

    cursor_snapshot.cursor = {.column = 1, .row = 2, .in_viewport = true, .visible = true};
    cursor_snapshot.revision = blank.revision + 4;
    fixture.document.applySnapshot(cursor_snapshot);
    const QImage next_row_image = render(fixture.surface);
    const ColorArea next_row_cursor = color_area(next_row_image, 0xff0000U);
    require_pixels(next_row_cursor, "next-row cursor", 30);
    const CellRectangle next_row_rectangle = blank_geometry.cell({1, 2});
    CHECK(next_row_cursor.bounds.left() >= static_cast<int>(next_row_rectangle.left) - 2);
    CHECK(next_row_cursor.bounds.right() < next_row_rectangle.right + 2);
    CHECK(next_row_cursor.center.y() > moved_cursor.center.y());
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lapis-terminal-render-tests"));
    QCoreApplication::setOrganizationName(QStringLiteral("lapis"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    qmlRegisterUncreatableType<SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                               "Sessions are owned by tests");
    qmlRegisterType<TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    try {
        const int result = run_renderer_regression();
        if (result == EXIT_SUCCESS)
            std::cout << "terminal_render_test: PASS\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << "terminal_render_test: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
