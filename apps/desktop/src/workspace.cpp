#include "workspace.hpp"
#include "live_connection.hpp"
#include <QDir>
#include <QFile>

#include <stdexcept>
#include <utility>

namespace lapis::desktop {

SessionPreview::SessionPreview(QString title, QString directory, QString activity, QColor accent,
                               std::string_view content)
    : title_(std::move(title)), directory_(std::move(directory)), activity_(std::move(activity)),
      accent_(accent) {
    session::Terminal terminal({100, 30});
    terminal.feed("\x1b]10;rgb:d9/de/e8\x1b\\\x1b]11;rgb:0d/13/1d\x1b\\"
                  "\x1b]4;2;rgb:87/cb/ac\x1b\\\x1b]4;4;rgb:9c/b4/ee\x1b\\"
                  "\x1b]4;3;rgb:df/bb/7b\x1b\\\x1b]4;5;rgb:ba/a4/e8\x1b\\"
                  "\x1b]4;8;rgb:75/83/98\x1b\\");
    terminal.feed(content);
    snapshot_ = terminal.snapshot();
}

Workspace::Workspace() {
    const auto add = [this](const char* title, const char* directory, const char* activity,
                            const char* accent, std::string_view content) {
        sessions_.push_back(std::make_unique<SessionPreview>(
            QString::fromUtf8(title), QString::fromUtf8(directory), QString::fromUtf8(activity),
            QColor(QString::fromLatin1(accent)), content));
    };
    const QString directory = QStringLiteral(LAPIS_PROJECT_ROOT);
    add("Shell", directory.toUtf8().constData(), "Connecting", "#87cbac", "");
    const QString runtime = directory + QStringLiteral("/runtime");
    if (!QDir().mkpath(runtime))
        throw std::runtime_error("Cannot create session runtime directory");
    QFile::setPermissions(runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    sessions_.front()->startLive(runtime + QStringLiteral("/desktop-v1.sock"), directory);
    add("Renderer", "lapis/apps/desktop", "In progress", "#9cb4ee",
        "\x1b[35mlapis\x1b[0m  \x1b[90mapps/desktop\x1b[0m\r\n\r\n"
        "\x1b[1mTerminal surface\x1b[0m\r\n\r\n"
        "  \x1b[34m01\x1b[0m  Keep text crisp at native display scale\r\n"
        "  \x1b[34m02\x1b[0m  Draw from retained terminal snapshots\r\n"
        "  \x1b[34m03\x1b[0m  Let unchanged scenes sleep\r\n\r\n"
        "\x1b[90m// Preview content, not an active agent transcript.\x1b[0m\r\n\r\n"
        "\x1b[35mvoid\x1b[0m TerminalSurface::update() {\r\n"
        "    \x1b[90m// Present the selected session.\x1b[0m\r\n"
        "    draw(snapshot);\r\n"
        "}\r\n\r\n\x1b[34m>\x1b[0m ");
    add("Agent bridge", "lapis/adapters", "Needs input", "#dfbb7b",
        "\x1b[35mlapis\x1b[0m  \x1b[90madapters/codex\x1b[0m\r\n\r\n"
        "\x1b[1mAttention without interruption\x1b[0m\r\n\r\n"
        "  A session can request attention.\r\n"
        "  The workspace keeps keyboard ownership explicit.\r\n\r\n"
        "\x1b[33m  SAMPLE REQUEST\x1b[0m\r\n"
        "  Review the adapter contract before continuing.\r\n\r\n"
        "\x1b[90m  Focusing a session does not approve its request.\x1b[0m\r\n"
        "\x1b[90m  This card illustrates the attention treatment.\x1b[0m\r\n");
    add("Session service", "lapis/services", "Queued", "#92a0b5",
        "\x1b[35mlapis\x1b[0m  \x1b[90mservices/session\x1b[0m\r\n\r\n"
        "\x1b[1mNext implementation slice\x1b[0m\r\n\r\n"
        "  [ ] Launch a real child under a POSIX PTY\r\n"
        "  [ ] Route input, output and terminal resize\r\n"
        "  [ ] Handle exit and descriptor cleanup\r\n"
        "  [ ] Preserve sessions across GUI attachment\r\n\r\n"
        "\x1b[90m  Preview content, not a running agent.\x1b[0m\r\n");
    add("Checks", "lapis", "Passed", "#87cbac",
        "\x1b[35mlapis\x1b[0m  \x1b[90mterminal adapter\x1b[0m\r\n\r\n"
        "\x1b[34m>\x1b[0m just asan\r\n\r\n"
        "  \x1b[32mPASS\x1b[0m  snapshot lifetime\r\n"
        "  \x1b[32mPASS\x1b[0m  bounded reply overflow\r\n"
        "  \x1b[32mPASS\x1b[0m  history eviction and clearing\r\n"
        "  \x1b[32mPASS\x1b[0m  input rejection and recovery\r\n\r\n"
        "\x1b[90m  Saved adapter results; not a live test runner.\x1b[0m\r\n");
    add("Workspace notes", "lapis", "Idle", "#92a0b5",
        "\x1b[35mlapis\x1b[0m  \x1b[90mworkspace notes\x1b[0m\r\n\r\n"
        "\x1b[1mResponsiveness. Ergonomics. Visuals.\x1b[0m\r\n\r\n"
        "  Keep the focused session easy to read.\r\n"
        "  Keep neighboring sessions recognizable.\r\n"
        "  Use color for attention, not decoration.\r\n"
        "  Leave enough quiet space to think.\r\n\r\n"
        "\x1b[90m  Preview cards show the planned layout.\x1b[0m\r\n");
}

QVariantList Workspace::sessions() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(sessions_.size()));
    for (const auto& session : sessions_)
        result.push_back(QVariant::fromValue(session.get()));
    return result;
}

SessionPreview* Workspace::focusedSession() const {
    return sessions_.at(static_cast<std::size_t>(focused_index_)).get();
}

void Workspace::setFocusedIndex(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= sessions_.size() || index == focused_index_)
        return;
    focused_index_ = index;
    emit focusChanged();
}

} // namespace lapis::desktop
