#include "../probe.hpp"

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtpty/MockPty.hpp>

#include <crispy/Environment.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

namespace lapis::probe {
namespace {

vtbackend::PageSize page_size(Size size) {
    if (size.columns == 0 || size.rows == 0) {
        throw std::invalid_argument("terminal dimensions must be nonzero");
    }
    return {vtbackend::LineCount{size.rows}, vtbackend::ColumnCount{size.columns}};
}

vtbackend::Settings settings(Size size) {
    auto value = vtbackend::Settings{};
    value.pageSize = page_size(size);
    value.historyLimits = vtbackend::HistoryLimits::plain(vtbackend::LineCount{1000});
    return value;
}

// The actual upstream terminal owns parsing/state. MockPty only captures encoded
// input; this headless experiment does not exercise process or PTY lifecycle.
class ContourEngine final : public Engine {
  public:
    explicit ContourEngine(Size size)
        : terminal_(events_, crispy::defaultEnvironment(),
                    std::make_unique<vtpty::MockPty>(page_size(size)), settings(size),
                    std::chrono::steady_clock::now()) {}

    void feed(std::string_view bytes) override { terminal_.writeToScreen(bytes); }

    void resize(Size size) override { terminal_.resizeScreen(page_size(size)); }

    Snapshot snapshot() override {
        const auto size = terminal_.pageSize();
        auto& screen = terminal_.currentScreen();
        const auto cursor = screen.cursor().position;
        Snapshot result;
        result.size = {static_cast<std::uint16_t>(unbox(size.columns)),
                       static_cast<std::uint16_t>(unbox(size.lines))};
        result.cursor_column = static_cast<std::uint16_t>(unbox(cursor.column));
        result.cursor_row = static_cast<std::uint16_t>(unbox(cursor.line));
        result.alternate_screen = terminal_.isAlternateScreen();
        result.bracketed_paste = terminal_.isModeEnabled(vtbackend::DECMode::BracketedPaste);
        result.cells.reserve(static_cast<std::size_t>(result.size.rows) * result.size.columns);
        for (std::uint16_t row = 0; row < result.size.rows; ++row) {
            for (std::uint16_t column = 0; column < result.size.columns; ++column) {
                const auto cell =
                    screen.at(vtbackend::LineOffset{row}, vtbackend::ColumnOffset{column});
                Cell copy;
                copy.text = cell.codepoints();
                copy.width = cell.isFlagEnabled(vtbackend::CellFlag::WideCharContinuation)
                                 ? 0
                                 : cell.width();
                copy.bold = cell.isFlagEnabled(vtbackend::CellFlag::Bold);
                copy.foreground_rgb =
                    vtbackend::apply(terminal_.colorPalette(), cell.foregroundColor(),
                                     vtbackend::ColorTarget::Foreground,
                                     vtbackend::ColorMode::Normal)
                        .value();
                copy.background_rgb =
                    vtbackend::apply(terminal_.colorPalette(), cell.backgroundColor(),
                                     vtbackend::ColorTarget::Background,
                                     vtbackend::ColorMode::Normal)
                        .value();
                if (cell.isFlagEnabled(vtbackend::CellFlag::Inverse)) {
                    std::swap(copy.foreground_rgb, copy.background_rgb);
                }
                result.cells.push_back(std::move(copy));
            }
        }
        return result;
    }

    std::string key_up() override {
        discard_input();
        terminal_.sendKeyEvent(vtbackend::Key::UpArrow, vtbackend::KeyboardModifiers{},
                               vtbackend::KeyboardEventType::Press,
                               std::chrono::steady_clock::now());
        return take_input();
    }

    std::string paste(std::string_view text) override {
        discard_input();
        terminal_.sendPaste(text);
        return take_input();
    }

  private:
    void discard_input() {
        terminal_.flushInput();
        static_cast<vtpty::MockPty&>(terminal_.device()).stdinBuffer().clear();
    }

    std::string take_input() {
        terminal_.flushInput();
        return std::exchange(static_cast<vtpty::MockPty&>(terminal_.device()).stdinBuffer(), {});
    }

    vtbackend::Terminal::NullEvents events_;
    vtbackend::Terminal terminal_;
};

} // namespace

std::unique_ptr<Engine> make_engine(Size size) { return std::make_unique<ContourEngine>(size); }

std::string_view engine_name() { return "contour"; }

} // namespace lapis::probe
