#include "../probe.hpp"

#include <ghostty/vt.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lapis::probe {
namespace {

void require_success(GhosttyResult result) {
    if (result != GHOSTTY_SUCCESS) {
        throw std::runtime_error("Ghostty C API failed: " + std::to_string(result));
    }
}

template <auto Free> struct HandleDeleter {
    template <typename Handle> void operator()(Handle* handle) const noexcept { Free(handle); }
};

template <typename Handle, auto Free>
using OwnedHandle = std::unique_ptr<std::remove_pointer_t<Handle>, HandleDeleter<Free>>;

template <typename Handle, auto Free, typename Create, typename... Args>
auto create_handle(Create create, Args... args) {
    Handle handle = nullptr;
    const GhosttyResult result = create(nullptr, &handle, args...);
    OwnedHandle<Handle, Free> owned(handle);
    require_success(result);
    return owned;
}

[[nodiscard]] std::uint32_t rgb(GhosttyColorRgb value) {
    return (static_cast<std::uint32_t>(value.r) << 16U) |
           (static_cast<std::uint32_t>(value.g) << 8U) | value.b;
}

[[nodiscard]] std::uint8_t cell_width(GhosttyCellWide value) {
    switch (value) {
    case GHOSTTY_CELL_WIDE_NARROW:
        return 1;
    case GHOSTTY_CELL_WIDE_WIDE:
        return 2;
    case GHOSTTY_CELL_WIDE_SPACER_TAIL:
    case GHOSTTY_CELL_WIDE_SPACER_HEAD:
        return 0;
    default:
        throw std::runtime_error("Unknown Ghostty cell width");
    }
}

[[nodiscard]] GhosttyColorRgb cell_color(GhosttyRenderStateRowCells cells,
                                         GhosttyRenderStateRowCellsData kind,
                                         GhosttyColorRgb fallback) {
    GhosttyColorRgb value{};
    const GhosttyResult result = ghostty_render_state_row_cells_get(cells, kind, &value);
    if (result == GHOSTTY_INVALID_VALUE) {
        return fallback;
    }
    require_success(result);
    return value;
}

struct DefaultColors {
    GhosttyColorRgb foreground{};
    GhosttyColorRgb background{};
};

[[nodiscard]] Cell copy_cell(GhosttyRenderStateRowCells cells, DefaultColors colors) {
    Cell cell{};
    std::uint32_t length = 0;
    require_success(ghostty_render_state_row_cells_get(
        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &length));
    std::vector<std::uint32_t> codepoints(length);
    if (length != 0) {
        require_success(ghostty_render_state_row_cells_get(
            cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, codepoints.data()));
        for (const std::uint32_t codepoint : codepoints) {
            cell.text.push_back(static_cast<char32_t>(codepoint));
        }
    }
    GhosttyCell raw{};
    require_success(
        ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw));
    GhosttyCellWide width{};
    require_success(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &width));
    cell.width = cell_width(width);
    GhosttyStyle style{};
    style.size = sizeof(style);
    require_success(ghostty_render_state_row_cells_get(
        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style));
    cell.bold = style.bold;
    cell.foreground_rgb =
        rgb(cell_color(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, colors.foreground));
    cell.background_rgb =
        rgb(cell_color(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, colors.background));
    if (style.inverse) {
        std::swap(cell.foreground_rgb, cell.background_rgb);
    }
    return cell;
}

class GhosttyEngine final : public Engine {
  public:
    explicit GhosttyEngine(Size size)
        : terminal_(create_handle<GhosttyTerminal, ghostty_terminal_free>(ghostty_terminal_new,
                                                                          size.columns, size.rows)),
          render_(create_handle<GhosttyRenderState, ghostty_render_state_free>(
              ghostty_render_state_new)),
          rows_(
              create_handle<GhosttyRenderStateRowIterator, ghostty_render_state_row_iterator_free>(
                  ghostty_render_state_row_iterator_new)),
          cells_(create_handle<GhosttyRenderStateRowCells, ghostty_render_state_row_cells_free>(
              ghostty_render_state_row_cells_new)),
          encoder_(
              create_handle<GhosttyKeyEncoder, ghostty_key_encoder_free>(ghostty_key_encoder_new)),
          key_(create_handle<GhosttyKeyEvent, ghostty_key_event_free>(ghostty_key_event_new)) {
        // Explicitly bound upstream history in this experiment; not a product default.
        const std::size_t history_bytes = std::size_t{1024} * 1024U;
        require_success(ghostty_terminal_set(
            terminal_.get(), GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &history_bytes));
        std::size_t observed_history_limit = 0;
        terminal_get(GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_BYTES, observed_history_limit);
        if (observed_history_limit != history_bytes) {
            throw std::runtime_error("Ghostty did not retain the requested history budget");
        }
    }

    void feed(std::string_view bytes) override {
        ghostty_terminal_vt_write(
            terminal_.get(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }

    void resize(Size size) override {
        require_success(ghostty_terminal_resize(terminal_.get(), size.columns, size.rows, 8, 16));
    }

    [[nodiscard]] Snapshot snapshot() override {
        require_success(ghostty_render_state_update(render_.get(), terminal_.get()));
        Snapshot result{};
        terminal_get(GHOSTTY_TERMINAL_DATA_COLS, result.size.columns);
        terminal_get(GHOSTTY_TERMINAL_DATA_ROWS, result.size.rows);
        terminal_get(GHOSTTY_TERMINAL_DATA_CURSOR_X, result.cursor_column);
        terminal_get(GHOSTTY_TERMINAL_DATA_CURSOR_Y, result.cursor_row);
        GhosttyTerminalScreen screen{};
        terminal_get(GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, screen);
        result.alternate_screen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
        result.bracketed_paste = bracketed_paste();
        const std::size_t cell_count =
            static_cast<std::size_t>(result.size.columns) * result.size.rows;
        result.cells.reserve(cell_count);
        DefaultColors colors{};
        require_success(ghostty_render_state_get(
            render_.get(), GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND, &colors.foreground));
        require_success(ghostty_render_state_get(
            render_.get(), GHOSTTY_RENDER_STATE_DATA_COLOR_BACKGROUND, &colors.background));
        auto row_handle = rows_.get();
        require_success(ghostty_render_state_get(render_.get(),
                                                 GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                                 static_cast<void*>(&row_handle)));
        while (ghostty_render_state_row_iterator_next(rows_.get())) {
            auto cell_handle = cells_.get();
            require_success(ghostty_render_state_row_get(rows_.get(),
                                                         GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                                         static_cast<void*>(&cell_handle)));
            while (ghostty_render_state_row_cells_next(cells_.get())) {
                result.cells.push_back(copy_cell(cells_.get(), colors));
            }
        }
        if (result.cells.size() != cell_count) {
            throw std::runtime_error("Ghostty render state dimensions disagree with cells");
        }
        require_success(ghostty_render_state_clean(render_.get()));
        return result;
    }

    [[nodiscard]] std::string key_up() override {
        ghostty_key_encoder_setopt_from_terminal(encoder_.get(), terminal_.get());
        ghostty_key_event_set_action(key_.get(), GHOSTTY_KEY_ACTION_PRESS);
        ghostty_key_event_set_key(key_.get(), GHOSTTY_KEY_ARROW_UP);
        std::array<char, 64> buffer{};
        std::size_t written = 0;
        require_success(ghostty_key_encoder_encode(encoder_.get(), key_.get(), buffer.data(),
                                                   buffer.size(), &written));
        return {buffer.data(), written};
    }

    [[nodiscard]] std::string paste(std::string_view text) override {
        // Probe only the text encoder. Clipboard permissions and protocol callbacks
        // remain the future service's responsibility; no clipboard is accessed here.
        std::string mutable_text(text);
        std::string output(text.size() + 12U, '\0');
        std::size_t written = 0;
        require_success(ghostty_paste_encode(mutable_text.data(), mutable_text.size(),
                                             bracketed_paste(), output.data(), output.size(),
                                             &written));
        output.resize(written);
        return output;
    }

  private:
    template <typename Value> void terminal_get(GhosttyTerminalData kind, Value& value) const {
        require_success(ghostty_terminal_get(terminal_.get(), kind, &value));
    }

    [[nodiscard]] bool bracketed_paste() const {
        GhosttyTerminalModeConfig mode{GHOSTTY_MODE_BRACKETED_PASTE, false};
        terminal_get(GHOSTTY_TERMINAL_DATA_MODE, mode);
        return mode.value;
    }

    OwnedHandle<GhosttyTerminal, ghostty_terminal_free> terminal_;
    OwnedHandle<GhosttyRenderState, ghostty_render_state_free> render_;
    OwnedHandle<GhosttyRenderStateRowIterator, ghostty_render_state_row_iterator_free> rows_;
    OwnedHandle<GhosttyRenderStateRowCells, ghostty_render_state_row_cells_free> cells_;
    OwnedHandle<GhosttyKeyEncoder, ghostty_key_encoder_free> encoder_;
    OwnedHandle<GhosttyKeyEvent, ghostty_key_event_free> key_;
};

} // namespace

std::unique_ptr<Engine> make_engine(Size size) { return std::make_unique<GhosttyEngine>(size); }

std::string_view engine_name() { return "ghostty"; }

} // namespace lapis::probe
