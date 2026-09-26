#ifndef LAPIS_SESSION_TERMINAL_HPP
#define LAPIS_SESSION_TERMINAL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lapis::session {

// In-process terminal contract v0. All values are owned; no upstream ABI leaks.
struct TerminalSize {
    std::uint16_t columns{80};
    std::uint16_t rows{24};
    bool operator==(const TerminalSize&) const = default;
};

struct TerminalLimits {
    std::size_t max_cells{std::size_t{512} * 256U};
    std::size_t max_input_bytes{std::size_t{1024} * 1024U};
    std::size_t max_grapheme_codepoints{std::size_t{1024} * 1024U};
    std::size_t max_reply_bytes{std::size_t{64} * 1024U};
    // Upstream page-granular history budget, not an allocation/RSS ceiling.
    std::size_t history_bytes{std::size_t{16} * 1024U * 1024U};
};

enum class ColorKind : std::uint8_t { default_color, indexed, rgb };
struct TerminalColor {
    ColorKind kind{ColorKind::default_color};
    std::uint32_t value{}; // Palette index or 0xRRGGBB; zero for default.
    bool operator==(const TerminalColor&) const = default;
};

enum class Underline : std::uint8_t { none, single, double_line, curly, dotted, dashed };
struct TerminalStyle {
    TerminalColor foreground;
    TerminalColor background;
    TerminalColor underline_color;
    Underline underline{Underline::none};
    bool bold{};
    bool italic{};
    bool faint{};
    bool blink{};
    bool inverse{};
    bool invisible{};
    bool strikethrough{};
    bool overline{};
    bool operator==(const TerminalStyle&) const = default;
};

enum class CellKind : std::uint8_t { narrow, wide, wide_tail, wrap_spacer };
struct TerminalCell {
    // Slice into the snapshot's contiguous grapheme storage. Avoid one heap
    // allocation per cell. Empty cells have an empty slice.
    std::uint32_t text_offset{};
    std::uint32_t text_length{};
    CellKind kind{CellKind::narrow};
    TerminalStyle style;
};

enum class CursorShape : std::uint8_t { block, bar, underline, hollow_block };
struct TerminalCursor {
    std::uint16_t column{};
    std::uint16_t row{};
    bool in_viewport{};
    bool visible{};
    bool blinking{};
    bool wide_tail{};
    CursorShape shape{CursorShape::block};
};

struct TerminalHistory {
    std::size_t total_rows{}; // Includes current screen; not an allocated-byte count.
    std::size_t viewport_offset{};
    std::size_t viewport_rows{};
    bool primary_available{};
};

struct TerminalSnapshot {
    std::uint64_t revision{};
    TerminalSize size;
    TerminalCursor cursor;
    bool alternate_screen{};
    // Set by the wire decoder, not the terminal: the service that sent this
    // screen takes wheel input on the alternate screen. Services from before
    // wheel input leave it false; their clients keep the older behavior.
    bool accepts_wheel{};
    bool bracketed_paste{};
    bool application_cursor_keys{};
    std::uint32_t foreground_rgb{};
    std::uint32_t background_rgb{};
    std::optional<std::uint32_t> cursor_rgb;
    std::array<std::uint32_t, 256> palette{};
    std::vector<TerminalCell> cells; // Row-major; exactly columns * rows.
    std::u32string graphemes;
    TerminalHistory history;

    [[nodiscard]] std::u32string_view text(std::size_t cell_index) const;
    // Resolve colors against this snapshot, then apply inverse. Bold alone does
    // not brighten indexed colors. Faint/invisible/blink remain rendering flags.
    [[nodiscard]] std::uint32_t cell_foreground(std::size_t cell_index) const;
    [[nodiscard]] std::uint32_t cell_background(std::size_t cell_index) const;
};

// Initial verified press-only navigation subset. Text/IME, the mouse beyond
// the wheel (encode_wheel), clipboard access, key release/repeat and full
// keyboard protocols need later contracts.
enum class TerminalKey : std::uint8_t {
    up,
    down,
    left,
    right,
    home,
    end,
    page_up,
    page_down,
    insert,
    delete_key,
    enter,
    tab,
    backspace,
    escape
};
struct WheelTurn {
    int steps{};            // notches; positive scrolls back (up)
    std::uint16_t column{}; // the viewport cell under the pointer
    std::uint16_t row{};
};
struct KeyModifiers {
    bool shift{};
    bool control{};
    bool alt{};
    bool super{};
};

// Single owner thread; callers must not access a Terminal concurrently. No I/O,
// threads, PTY, timers or callbacks into client code. Snapshots outlive Terminal.
// Invalid geometry/configuration throws invalid_argument; input/snapshot limits
// throw length_error. Reply overflow throws overflow_error and permanently faults
// the instance (subsequent operations fail); recreate and resynchronize it.
// Upstream errors throw runtime_error. No exceptions cross a C callback.
class Terminal {
  public:
    explicit Terminal(TerminalSize size = {}, TerminalLimits limits = {});
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal&&) = delete;

    void feed(std::string_view bytes);
    void resize(TerminalSize size);
    void clear_history();
    // Owned viewport extraction at an absolute history row. The live viewport,
    // including its screen and scroll position, is restored even on failure.
    // Offset is rejected unless it leaves a complete viewport in primary-screen
    // history. history_metadata performs no render extraction.
    [[nodiscard]] TerminalHistory history_metadata();
    [[nodiscard]] TerminalSnapshot history_snapshot(std::size_t offset);
    [[nodiscard]] TerminalSnapshot snapshot();
    [[nodiscard]] std::string encode_key(TerminalKey key, KeyModifiers modifiers = {});
    // Pure text encoding: sanitizes unsafe control bytes and uses current mode.
    // It neither accesses a clipboard nor grants an application's request.
    [[nodiscard]] std::string encode_paste(std::string_view text);
    // A turn of the wheel: mouse wheel events in the program's own format
    // when it asked for mouse reporting, else on the alternate screen three
    // arrow keys a notch, as terminals do, else nothing (the primary screen's
    // history scrolls in the view instead).
    [[nodiscard]] std::string encode_wheel(WheelTurn turn);
    // Drains ordered replies generated by feed/resize. Empty when no replies.
    [[nodiscard]] std::string take_replies();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lapis::session

#endif // LAPIS_SESSION_TERMINAL_HPP
