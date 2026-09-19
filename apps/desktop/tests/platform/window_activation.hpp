#ifndef LAPIS_TEST_WINDOW_ACTIVATION_HPP
#define LAPIS_TEST_WINDOW_ACTIVATION_HPP
#include <QWindow>
namespace lapis::desktop::test {
#ifdef Q_OS_MACOS
void activate_test_window(QWindow& window);
#else
inline void activate_test_window(QWindow& window) {
    window.show();
    window.requestActivate();
}
#endif
} // namespace lapis::desktop::test
#endif
