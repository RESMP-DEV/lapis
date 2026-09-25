#include "app_paths.hpp"
#include "desktop_actions.hpp"
#include "shell_environment.hpp"

#include <QDir>
#include <QFileInfo>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::parse_environment;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void login_environment_is_read_after_the_marker() {
    // A startup file printed a banner, and one value spans lines.
    constexpr char printed[] = "Welcome back\nPATH=/wrong\n\nlapis-login-environment\n"
                               "PATH=/opt/tools/bin:/usr/bin\0SHLVL=2\0NOTE=two\nlines\0"
                               "_=/usr/bin/env\0PWD=/\0TERM=dumb\0KEY=a=b\0";
    const QByteArray output(printed, sizeof(printed) - 1);
    const auto variables = parse_environment(output);
    require(variables.size() == 3, "the shell's bookkeeping and TERM are left out");
    require(variables[0] == std::pair{QByteArray("PATH"), QByteArray("/opt/tools/bin:/usr/bin")},
            "the banner's look-alike line before the marker is ignored");
    require(variables[1].second == QByteArray("two\nlines"), "a value keeps its newlines");
    require(variables[2].second == QByteArray("a=b"), "only the first = separates the name");
    require(parse_environment(QByteArrayLiteral("PATH=/usr/bin")).isEmpty(),
            "without the marker, nothing is taken");
}

void lapis_home_moves_config_and_runtime_state() {
    const QString developer = lapis::desktop::data_directory();
    require(QFileInfo(QDir(developer).filePath(QStringLiteral("CMakeLists.txt"))).exists(),
            "a developer build keeps its data in the checkout");
    qputenv("LAPIS_HOME", "/tmp/lapis-home-check");
    require(lapis::desktop::data_directory() == QStringLiteral("/tmp/lapis-home-check"),
            "LAPIS_HOME moves lapis.json and runtime/");
    qunsetenv("LAPIS_HOME");
}
void editors_follow_the_config_then_preference() {
    using lapis::desktop::DesktopActions;
    require(DesktopActions::chooseEditor(QStringLiteral(" Nova "), {QStringLiteral("Cursor")}) ==
                QStringLiteral("Nova"),
            "the configured editor wins");
    require(DesktopActions::chooseEditor({}, {QStringLiteral("Zed"), QStringLiteral("Cursor")}) ==
                QStringLiteral("Zed"),
            "otherwise the first installed, in preference order");
    require(DesktopActions::chooseEditor({}, {}).isEmpty(), "none without an editor");
}
} // namespace

int main() {
    try {
        login_environment_is_read_after_the_marker();
        lapis_home_moves_config_and_runtime_state();
        editors_follow_the_config_then_preference();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "app environment tests passed\n";
    return 0;
}
