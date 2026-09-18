#ifndef LAPIS_TERMINAL_PROBE_HPP
#define LAPIS_TERMINAL_PROBE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Experiment contract v0. These are owned observations, not the service IPC API.
namespace lapis::probe {

struct Size {
    std::uint16_t columns{0};
    std::uint16_t rows{0};
};

struct Cell {
    std::u32string text;
    std::uint8_t width{1}; // Zero denotes the continuation of a wide cell.
    bool bold{false};
    std::uint32_t foreground_rgb{0}; // Resolved 0xRRGGBB; palette identity is not retained.
    std::uint32_t background_rgb{0};
};

struct Snapshot {
    Size size{};
    std::vector<Cell> cells; // Row-major, including blank and continuation cells.
    std::uint16_t cursor_column{0};
    std::uint16_t cursor_row{0};
    bool alternate_screen{false};
    bool bracketed_paste{false};
};

class Engine {
  public:
    Engine() = default;
    virtual ~Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    virtual void feed(std::string_view bytes) = 0;
    virtual void resize(Size size) = 0;
    [[nodiscard]] virtual Snapshot snapshot() = 0;
    [[nodiscard]] virtual std::string key_up() = 0;
    [[nodiscard]] virtual std::string paste(std::string_view text) = 0;
};

[[nodiscard]] std::unique_ptr<Engine> make_engine(Size size);
[[nodiscard]] std::string_view engine_name();

} // namespace lapis::probe

#endif // LAPIS_TERMINAL_PROBE_HPP
