#include <lapis/session/terminal.hpp>

#include <ghostty/vt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace lapis::session {
namespace {

void require_success(GhosttyResult result);

template <auto Free> struct HandleDeleter {
    template <typename Handle> void operator()(Handle handle) const noexcept { Free(handle); }
};

template <typename Handle, auto Free>
using OwnedHandle = std::unique_ptr<std::remove_pointer_t<Handle>, HandleDeleter<Free>>;

template <typename Handle, auto Free, typename Create, typename... Args>
OwnedHandle<Handle, Free> create_handle(Create create, Args... args) {
    Handle handle = nullptr;
    const GhosttyResult result = create(nullptr, &handle, args...);
    OwnedHandle<Handle, Free> owned(handle);
    require_success(result);
    return owned;
}

[[noreturn]] void fail(GhosttyResult result) {
    throw std::runtime_error("Ghostty C API failed: " + std::to_string(result));
}

void require_success(GhosttyResult result) {
    if (result != GHOSTTY_SUCCESS)
        fail(result);
}

[[nodiscard]] std::uint32_t rgb_from(GhosttyColorRgb color) {
    return (std::uint32_t{color.r} << 16U) | (std::uint32_t{color.g} << 8U) |
           std::uint32_t{color.b};
}

template <typename Container>
void copy_palette(const Container& source, std::array<std::uint32_t, 256>& destination) {
    std::transform(source, source + destination.size(), destination.begin(), rgb_from);
}

[[nodiscard]] TerminalLimits normalize_limits(const TerminalLimits& limits) {
    const std::size_t minimum_cells = 1U;
    if (limits.max_cells < minimum_cells || limits.max_input_bytes == 0U ||
        limits.max_grapheme_codepoints == 0U || limits.max_reply_bytes == 0U ||
        limits.max_grapheme_codepoints > std::numeric_limits<std::uint32_t>::max() ||
        limits.max_input_bytes > std::string{}.max_size() - 12U ||
        limits.max_reply_bytes > std::string{}.max_size()) {
        throw std::invalid_argument("Terminal limits must be nonzero and permit one cell");
    }
    return limits;
}

void validate_size(TerminalSize size, std::size_t max_cells) {
    const std::size_t cells = std::size_t{size.columns} * std::size_t{size.rows};
    if (size.columns == 0U || size.rows == 0U || cells > max_cells) {
        throw std::invalid_argument("Terminal dimensions exceed cell limit");
    }
}

} // namespace

std::u32string_view TerminalSnapshot::text(std::size_t cell_index) const {
    if (cell_index >= cells.size())
        throw std::out_of_range("Terminal cell index is invalid");
    const TerminalCell& cell = cells[cell_index];
    return std::u32string_view{graphemes}.substr(cell.text_offset, cell.text_length);
}

namespace {

[[nodiscard]] TerminalColor style_color(const GhosttyStyleColor& color) {
    switch (color.tag) {
    case GHOSTTY_STYLE_COLOR_NONE:
        return {};
    case GHOSTTY_STYLE_COLOR_PALETTE:
        return {ColorKind::indexed, color.value.palette};
    case GHOSTTY_STYLE_COLOR_RGB:
        return {ColorKind::rgb, rgb_from(color.value.rgb)};
    default:
        throw std::runtime_error("Unknown Ghostty style color tag");
    }
}

[[nodiscard]] Underline style_underline(int value) {
    switch (value) {
    case GHOSTTY_SGR_UNDERLINE_NONE:
        return Underline::none;
    case GHOSTTY_SGR_UNDERLINE_SINGLE:
        return Underline::single;
    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
        return Underline::double_line;
    case GHOSTTY_SGR_UNDERLINE_CURLY:
        return Underline::curly;
    case GHOSTTY_SGR_UNDERLINE_DOTTED:
        return Underline::dotted;
    case GHOSTTY_SGR_UNDERLINE_DASHED:
        return Underline::dashed;
    default:
        throw std::runtime_error("Unknown Ghostty underline style");
    }
}

[[nodiscard]] CellKind cell_kind(GhosttyCellWide value) {
    switch (value) {
    case GHOSTTY_CELL_WIDE_NARROW:
        return CellKind::narrow;
    case GHOSTTY_CELL_WIDE_WIDE:
        return CellKind::wide;
    case GHOSTTY_CELL_WIDE_SPACER_TAIL:
        return CellKind::wide_tail;
    case GHOSTTY_CELL_WIDE_SPACER_HEAD:
        return CellKind::wrap_spacer;
    default:
        throw std::runtime_error("Unknown Ghostty cell width");
    }
}

[[nodiscard]] CursorShape cursor_shape(GhosttyRenderStateCursorVisualStyle value) {
    switch (value) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK:
        return CursorShape::block;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
        return CursorShape::bar;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
        return CursorShape::underline;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
        return CursorShape::hollow_block;
    default:
        throw std::runtime_error("Unknown Ghostty cursor style");
    }
}

[[nodiscard]] std::uint32_t effective_rgb(TerminalColor color, const TerminalSnapshot& snapshot,
                                          bool background) {
    switch (color.kind) {
    case ColorKind::default_color:
        return background ? snapshot.background_rgb : snapshot.foreground_rgb;
    case ColorKind::indexed: {
        if (color.value >= snapshot.palette.size()) {
            throw std::runtime_error("Ghostty returned an invalid palette index");
        }
        return snapshot.palette[color.value];
    }
    case ColorKind::rgb:
        return color.value;
    default:
        throw std::runtime_error("Unknown terminal color kind");
    }
}

} // namespace

std::uint32_t TerminalSnapshot::cell_foreground(std::size_t cell_index) const {
    const TerminalStyle& style = cells.at(cell_index).style;
    return style.inverse ? effective_rgb(style.background, *this, true)
                         : effective_rgb(style.foreground, *this, false);
}

std::uint32_t TerminalSnapshot::cell_background(std::size_t cell_index) const {
    const TerminalStyle& style = cells.at(cell_index).style;
    return style.inverse ? effective_rgb(style.foreground, *this, false)
                         : effective_rgb(style.background, *this, true);
}

struct Terminal::Impl {
    TerminalLimits limits;
    std::uint64_t revision{1U};
    bool poisoned{false};
    std::vector<char> replies;
    std::size_t reply_size{};
    OwnedHandle<GhosttyTerminal, ghostty_terminal_free> terminal;
    OwnedHandle<GhosttyRenderState, ghostty_render_state_free> render;
    OwnedHandle<GhosttyRenderStateRowIterator, ghostty_render_state_row_iterator_free> row_iterator;
    OwnedHandle<GhosttyRenderStateRowCells, ghostty_render_state_row_cells_free> row_cells;
    OwnedHandle<GhosttyKeyEncoder, ghostty_key_encoder_free> encoder;
    OwnedHandle<GhosttyKeyEvent, ghostty_key_event_free> key;
    std::vector<std::uint32_t> scratch;

    Impl(TerminalSize size, const TerminalLimits& input_limits)
        : limits(normalize_limits(input_limits)), replies(limits.max_reply_bytes) {
        validate_size(size, limits.max_cells);
        terminal = create_handle<GhosttyTerminal, ghostty_terminal_free>(ghostty_terminal_new,
                                                                         size.columns, size.rows);
        setup(size);
    }

    void setup(TerminalSize size) {
        render =
            create_handle<GhosttyRenderState, ghostty_render_state_free>(ghostty_render_state_new);
        row_iterator =
            create_handle<GhosttyRenderStateRowIterator, ghostty_render_state_row_iterator_free>(
                ghostty_render_state_row_iterator_new);
        row_cells = create_handle<GhosttyRenderStateRowCells, ghostty_render_state_row_cells_free>(
            ghostty_render_state_row_cells_new);
        encoder =
            create_handle<GhosttyKeyEncoder, ghostty_key_encoder_free>(ghostty_key_encoder_new);
        key = create_handle<GhosttyKeyEvent, ghostty_key_event_free>(ghostty_key_event_new);
        require_success(ghostty_terminal_set(terminal.get(), GHOSTTY_TERMINAL_OPT_USERDATA, this));
        constexpr GhosttyTerminalWritePtyFn write = &Impl::write_pty;
        require_success(ghostty_terminal_set(terminal.get(), GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                                             reinterpret_cast<const void*>(write)));
        require_success(ghostty_terminal_set(
            terminal.get(), GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &limits.history_bytes));
        std::size_t observed = 0U;
        terminal_get(GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_BYTES, observed);
        if (observed != limits.history_bytes) {
            throw std::runtime_error("Ghostty did not retain the configured history budget");
        }
        // This contract presents text only and never serves file-backed images.
        require_success(ghostty_terminal_set(
            terminal.get(), GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, nullptr));
        const bool disabled = false;
        require_success(ghostty_terminal_set(
            terminal.get(), GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &disabled));
        require_success(ghostty_terminal_set(
            terminal.get(), GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &disabled));
        require_success(ghostty_terminal_set(
            terminal.get(), GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, nullptr));
        resize_terminal(size);
    }

    static void write_pty(GhosttyTerminal, void* userdata, const std::uint8_t* data,
                          std::size_t length) noexcept {
        auto& instance = *static_cast<Impl*>(userdata);
        if (instance.poisoned || length == 0U)
            return;
        if (length > instance.replies.size() - instance.reply_size) {
            instance.poisoned = true;
            return;
        }
        // Storage is fully sized before callback registration; no allocation,
        // exceptions or engine reentry may occur here.
        std::memcpy(instance.replies.data() + instance.reply_size, data, length);
        instance.reply_size += length;
    }

    template <typename Value> void terminal_get(GhosttyTerminalData kind, Value& value) const {
        require_success(ghostty_terminal_get(terminal.get(), kind, &value));
    }

    template <typename Value> void render_get(GhosttyRenderStateData kind, Value& value) const {
        require_success(ghostty_render_state_get(render.get(), kind, &value));
    }

    void row_get(GhosttyRenderStateRowData kind, void* value) const {
        require_success(ghostty_render_state_row_get(row_iterator.get(), kind, value));
    }

    void cells_get(GhosttyRenderStateRowCellsData kind, void* value) const {
        require_success(ghostty_render_state_row_cells_get(row_cells.get(), kind, value));
    }

    [[nodiscard]] bool mode(GhosttyMode requested) const {
        GhosttyTerminalModeConfig config{requested, false};
        terminal_get(GHOSTTY_TERMINAL_DATA_MODE, config);
        return config.value;
    }

    [[nodiscard]] TerminalHistory scrollbar_history() const {
        GhosttyTerminalScrollbar scrollbar{};
        terminal_get(GHOSTTY_TERMINAL_DATA_SCROLLBAR, scrollbar);
        GhosttyTerminalScreen screen{};
        terminal_get(GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, screen);
        return {scrollbar.total, scrollbar.offset, scrollbar.len,
                screen == GHOSTTY_TERMINAL_SCREEN_PRIMARY};
    }

    void scroll_to_row(std::size_t offset) const {
        const GhosttyTerminalScrollViewport behavior{
            GHOSTTY_SCROLL_VIEWPORT_ROW, GhosttyTerminalScrollViewportValue{.row = offset}};
        ghostty_terminal_scroll_viewport(terminal.get(), behavior);
    }

    void require_restored_history(const TerminalHistory& expected) const {
        const TerminalHistory observed = scrollbar_history();
        if (observed.primary_available != expected.primary_available ||
            observed.total_rows != expected.total_rows ||
            observed.viewport_offset != expected.viewport_offset ||
            observed.viewport_rows != expected.viewport_rows) {
            throw std::runtime_error("Ghostty did not restore terminal viewport: expected " +
                                     std::to_string(expected.total_rows) + "/" +
                                     std::to_string(expected.viewport_offset) + "/" +
                                     std::to_string(expected.viewport_rows) + ", observed " +
                                     std::to_string(observed.total_rows) + "/" +
                                     std::to_string(observed.viewport_offset) + "/" +
                                     std::to_string(observed.viewport_rows));
        }
    }

    void require_healthy() const {
        if (poisoned)
            throw std::overflow_error("Terminal reply queue overflowed");
    }

    void feed(std::string_view bytes) {
        require_healthy();
        if (bytes.size() > limits.max_input_bytes) {
            throw std::length_error("Terminal input exceeds configured byte limit");
        }
        if (bytes.empty())
            return;
        ghostty_terminal_vt_write(
            terminal.get(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        require_healthy();
        ++revision;
    }

    void resize_terminal(TerminalSize size) {
        validate_size(size, limits.max_cells);
        require_success(ghostty_terminal_resize(terminal.get(), size.columns, size.rows, 8U, 16U));
        std::uint16_t columns = 0U;
        std::uint16_t rows = 0U;
        terminal_get(GHOSTTY_TERMINAL_DATA_COLS, columns);
        terminal_get(GHOSTTY_TERMINAL_DATA_ROWS, rows);
        if (columns != size.columns || rows != size.rows) {
            throw std::runtime_error("Ghostty did not retain the requested terminal dimensions");
        }
    }

    void resize(TerminalSize size) {
        require_healthy();
        resize_terminal(size);
        require_healthy();
        ++revision;
    }

    void clear_history() {
        require_healthy();
        constexpr std::size_t zero = 0U;
        require_success(
            ghostty_terminal_set(terminal.get(), GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &zero));
        require_success(ghostty_terminal_set(
            terminal.get(), GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &limits.history_bytes));
        std::size_t observed = 0U;
        terminal_get(GHOSTTY_TERMINAL_DATA_SCROLLBACK_MAX_BYTES, observed);
        if (observed != limits.history_bytes) {
            throw std::runtime_error("Ghostty did not restore the configured history budget");
        }
        ++revision;
    }

    [[nodiscard]] TerminalHistory history_metadata() {
        require_healthy();
        return scrollbar_history();
    }

    [[nodiscard]] TerminalSnapshot make_snapshot();
    [[nodiscard]] TerminalCell extract_cell(TerminalSnapshot& snapshot);
    [[nodiscard]] std::string encode_key(TerminalKey key_value, KeyModifiers modifiers);
    [[nodiscard]] std::string encode_paste(std::string_view text);
};

Terminal::Terminal(TerminalSize size, TerminalLimits limits)
    : impl_(std::make_unique<Impl>(size, limits)) {}
Terminal::~Terminal() = default;

TerminalHistory Terminal::history_metadata() { return impl_->history_metadata(); }

TerminalSnapshot Terminal::history_snapshot(std::size_t offset) {
    const TerminalHistory before = impl_->history_metadata();
    if (!before.primary_available)
        throw std::runtime_error("Terminal primary history is unavailable");
    if (before.viewport_rows == 0U || before.viewport_rows > before.total_rows ||
        offset > before.total_rows || before.total_rows - offset < before.viewport_rows) {
        throw std::invalid_argument("Terminal history offset does not contain a full viewport");
    }

    impl_->scroll_to_row(offset);
    try {
        TerminalSnapshot snapshot = impl_->make_snapshot();
        snapshot.history = impl_->history_metadata();
        snapshot.history.primary_available = true;
        impl_->scroll_to_row(before.viewport_offset);
        impl_->require_restored_history(before);
        return snapshot;
    } catch (...) {
        try {
            impl_->require_restored_history(before);
        } catch (const std::exception&) {
            impl_->scroll_to_row(before.viewport_offset);
        }
        impl_->require_restored_history(before);
        throw;
    }
}

void Terminal::feed(std::string_view bytes) { impl_->feed(bytes); }
void Terminal::resize(TerminalSize size) { impl_->resize(size); }
void Terminal::clear_history() { impl_->clear_history(); }

std::string Terminal::encode_key(TerminalKey key, KeyModifiers modifiers) {
    return impl_->encode_key(key, modifiers);
}

std::string Terminal::encode_paste(std::string_view text) { return impl_->encode_paste(text); }

std::string Terminal::take_replies() {
    impl_->require_healthy();
    std::string result(impl_->replies.data(), impl_->reply_size);
    impl_->reply_size = 0;
    return result;
}

struct KeyMapping {
    TerminalKey key{};
    GhosttyKey value{};
};

constexpr std::size_t key_mapping_count = 14U;
constexpr std::array<KeyMapping, key_mapping_count> key_mappings{
    KeyMapping{TerminalKey::up, GHOSTTY_KEY_ARROW_UP},
    {TerminalKey::down, GHOSTTY_KEY_ARROW_DOWN},
    {TerminalKey::left, GHOSTTY_KEY_ARROW_LEFT},
    {TerminalKey::right, GHOSTTY_KEY_ARROW_RIGHT},
    {TerminalKey::home, GHOSTTY_KEY_HOME},
    {TerminalKey::end, GHOSTTY_KEY_END},
    {TerminalKey::page_up, GHOSTTY_KEY_PAGE_UP},
    {TerminalKey::page_down, GHOSTTY_KEY_PAGE_DOWN},
    {TerminalKey::insert, GHOSTTY_KEY_INSERT},
    {TerminalKey::delete_key, GHOSTTY_KEY_DELETE},
    {TerminalKey::enter, GHOSTTY_KEY_ENTER},
    {TerminalKey::tab, GHOSTTY_KEY_TAB},
    {TerminalKey::backspace, GHOSTTY_KEY_BACKSPACE},
    {TerminalKey::escape, GHOSTTY_KEY_ESCAPE},
};

std::string Terminal::Impl::encode_key(TerminalKey key_value, KeyModifiers modifiers) {
    require_healthy();
    const auto mapping = std::find_if(
        key_mappings.begin(), key_mappings.end(),
        [key_value](const KeyMapping& candidate) { return candidate.key == key_value; });
    if (mapping == key_mappings.end()) {
        throw std::invalid_argument("Unknown terminal key");
    }
    ghostty_key_encoder_setopt_from_terminal(encoder.get(), terminal.get());
    ghostty_key_event_set_action(key.get(), GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(key.get(), mapping->value);
    std::uint16_t mods = 0U;
    if (modifiers.shift)
        mods |= GHOSTTY_MODS_SHIFT;
    if (modifiers.control)
        mods |= GHOSTTY_MODS_CTRL;
    if (modifiers.alt)
        mods |= GHOSTTY_MODS_ALT;
    if (modifiers.super)
        mods |= GHOSTTY_MODS_SUPER;
    ghostty_key_event_set_mods(key.get(), mods);
    ghostty_key_event_set_consumed_mods(key.get(), 0U);
    ghostty_key_event_set_composing(key.get(), false);
    ghostty_key_event_set_utf8(key.get(), nullptr, 0U);
    ghostty_key_event_set_unshifted_codepoint(key.get(), 0U);

    std::array<char, 128> output{};
    std::size_t written = 0U;
    require_success(ghostty_key_encoder_encode(encoder.get(), key.get(), output.data(),
                                               output.size(), &written));
    if (written > output.size())
        throw std::runtime_error("Ghostty key encoding exceeded buffer");
    return {output.data(), written};
}

std::string Terminal::Impl::encode_paste(std::string_view text) {
    require_healthy();
    if (text.size() > limits.max_input_bytes) {
        throw std::length_error("Terminal paste exceeds configured byte limit");
    }
    std::string mutable_text(text);
    std::string output(text.size() + 12U, '\0');
    std::size_t written{};
    require_success(ghostty_paste_encode(mutable_text.data(), mutable_text.size(),
                                         mode(GHOSTTY_MODE_BRACKETED_PASTE), output.data(),
                                         output.size(), &written));
    if (written > output.size())
        throw std::runtime_error("Ghostty paste exceeded buffer");
    output.resize(written);
    return output;
}

TerminalSnapshot Terminal::snapshot() { return impl_->make_snapshot(); }

TerminalSnapshot Terminal::Impl::make_snapshot() {
    require_healthy();
    require_success(ghostty_render_state_update(render.get(), terminal.get()));
    TerminalSnapshot snapshot;
    snapshot.revision = revision;
    std::uint16_t columns = 0U;
    std::uint16_t rows = 0U;
    terminal_get(GHOSTTY_TERMINAL_DATA_COLS, columns);
    terminal_get(GHOSTTY_TERMINAL_DATA_ROWS, rows);
    snapshot.size = {columns, rows};
    const std::size_t expected_cells = std::size_t{columns} * rows;
    if (expected_cells == 0U || expected_cells > limits.max_cells ||
        expected_cells > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("Terminal snapshot dimensions exceed extraction limits");
    }
    snapshot.cells.reserve(expected_cells);
    snapshot.graphemes.reserve(std::min(limits.max_grapheme_codepoints, expected_cells * 2U));

    GhosttyRenderStateColors colors{};
    colors.size = sizeof(colors);
    render_get(GHOSTTY_RENDER_STATE_DATA_COLORS, colors);
    snapshot.foreground_rgb = rgb_from(colors.foreground);
    snapshot.background_rgb = rgb_from(colors.background);
    snapshot.cursor_rgb =
        colors.cursor_has_value ? std::optional{rgb_from(colors.cursor)} : std::nullopt;
    copy_palette(colors.palette, snapshot.palette);

    GhosttyRenderStateCursor cursor{};
    cursor.size = sizeof(cursor);
    render_get(GHOSTTY_RENDER_STATE_DATA_CURSOR, cursor);
    snapshot.cursor.in_viewport = cursor.viewport_has_value;
    snapshot.cursor.visible = cursor.visible;
    snapshot.cursor.blinking = cursor.blinking;
    snapshot.cursor.shape = cursor_shape(cursor.visual_style);
    if (cursor.viewport_has_value) {
        snapshot.cursor.column = cursor.viewport_x;
        snapshot.cursor.row = cursor.viewport_y;
        snapshot.cursor.wide_tail = cursor.wide_tail;
    }

    GhosttyTerminalScreen screen{};
    terminal_get(GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, screen);
    snapshot.alternate_screen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    snapshot.bracketed_paste = mode(GHOSTTY_MODE_BRACKETED_PASTE);
    snapshot.application_cursor_keys = mode(GHOSTTY_MODE_DECCKM);
    GhosttyTerminalScrollbar scrollbar{};
    terminal_get(GHOSTTY_TERMINAL_DATA_SCROLLBAR, scrollbar);
    snapshot.history = {scrollbar.total, scrollbar.offset, scrollbar.len,
                        !snapshot.alternate_screen};

    auto row_handle = row_iterator.get();
    require_success(ghostty_render_state_get(render.get(), GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                             static_cast<void*>(&row_handle)));
    while (ghostty_render_state_row_iterator_next(row_iterator.get())) {
        auto cell_handle = row_cells.get();
        row_get(GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, static_cast<void*>(&cell_handle));
        while (ghostty_render_state_row_cells_next(row_cells.get())) {
            if (snapshot.cells.size() == expected_cells) {
                throw std::runtime_error("Ghostty returned more cells than viewport dimensions");
            }
            snapshot.cells.push_back(extract_cell(snapshot));
        }
    }
    if (snapshot.cells.size() != expected_cells) {
        throw std::runtime_error("Ghostty render state dimensions disagree with cells");
    }
    require_success(ghostty_render_state_clean(render.get()));
    return snapshot;
}

TerminalCell Terminal::Impl::extract_cell(TerminalSnapshot& snapshot) {
    TerminalCell cell;
    std::uint32_t graphemes_length = 0U;
    cells_get(GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &graphemes_length);
    if (graphemes_length != 0U) {
        if (graphemes_length > limits.max_grapheme_codepoints ||
            snapshot.graphemes.size() > limits.max_grapheme_codepoints - graphemes_length) {
            throw std::length_error("Terminal snapshot exceeds grapheme codepoint limit");
        }
        scratch.resize(graphemes_length);
        cells_get(GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, scratch.data());
        cell.text_offset = static_cast<std::uint32_t>(snapshot.graphemes.size());
        snapshot.graphemes.append(scratch.begin(), scratch.begin() + graphemes_length);
        cell.text_length = graphemes_length;
    }
    GhosttyCell raw{};
    cells_get(GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
    GhosttyCellWide width{};
    require_success(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &width));
    cell.kind = cell_kind(width);
    GhosttyStyle style{};
    style.size = sizeof(style);
    cells_get(GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
    cell.style = {style_color(style.fg_color),
                  style_color(style.bg_color),
                  style_color(style.underline_color),
                  style_underline(style.underline),
                  style.bold,
                  style.italic,
                  style.faint,
                  style.blink,
                  style.inverse,
                  style.invisible,
                  style.strikethrough,
                  style.overline};
    // Erased cells can store their background directly in the cell instead of
    // in its style. Reading only STYLE loses TUI panel fills on blank cells.
    // Preserve indexed colors so subsequent palette updates still apply.
    GhosttyCellContentTag content{};
    require_success(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_CONTENT_TAG, &content));
    if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE) {
        GhosttyColorPaletteIndex index{};
        require_success(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_PALETTE, &index));
        cell.style.background = {ColorKind::indexed, index};
    } else if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_RGB) {
        GhosttyColorRgb background{};
        require_success(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_COLOR_RGB, &background));
        cell.style.background = {ColorKind::rgb, rgb_from(background)};
    }
    return cell;
}

} // namespace lapis::session
