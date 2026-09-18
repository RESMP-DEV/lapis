#ifndef LAPIS_TEST_NATIVE_INPUT_DRIVER_HPP
#define LAPIS_TEST_NATIVE_INPUT_DRIVER_HPP
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
class QWindow;
namespace lapis::desktop::test {
enum class NativeModifiers : std::uint32_t {
    none = 0,
    control = std::uint32_t{1} << 18U,
    option = std::uint32_t{1} << 19U,
    command = std::uint32_t{1} << 20U
};
class NativeInputDriver {
  public:
    NativeInputDriver();
    ~NativeInputDriver();
    NativeInputDriver(const NativeInputDriver&) = delete;
    NativeInputDriver& operator=(const NativeInputDriver&) = delete;
    void selectUS();
    void selectJapanese();
    [[nodiscard]] static QString selectedSource();
    [[nodiscard]] static QStringList enabledSources();
    void activate(QWindow& window);
    void key(std::uint16_t code, NativeModifiers flags = NativeModifiers::none);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lapis::desktop::test
#endif
