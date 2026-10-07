#include "terminal_hooks.hpp"

#include <QJsonDocument>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace lapis::session {
class NotifyTurnsTestAccess final {
  public:
    [[nodiscard]] static std::uint64_t sequence(const NotifyTurns& turns) {
        return turns.sequence_;
    }
};
} // namespace lapis::session

namespace {
using lapis::session::NotifyTurns;
using lapis::session::NotifyTurnsTestAccess;
using lapis::session::TerminalHookChannel;
using lapis::session::TerminalHookEvent;
namespace attention = lapis::session::attention;

void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("terminal-hooks check failed at line " +
                                 std::to_string(where.line()));
}

constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
QByteArray init(const QByteArray& cli = "claude", const QByteArray& value = nonce) {
    return "\x1b]7717;lapis-init;" + cli + ";" + value + "\x07";
}
QByteArray event(const QJsonObject& source, const QByteArray& value = nonce,
                 const char* terminator = "\x07") {
    return "\x1b]7717;lapis-event;" + value + ";" +
           QJsonDocument(source).toJson(QJsonDocument::Compact).toBase64() + terminator;
}

// Sequences vanish from the output at any read boundary, with either
// terminator, and an authenticated event arrives once.
void removes_sequences_across_reads() {
    const QJsonObject stop{{"hook_event_name", "Stop"}, {"session_id", "s"}};
    const QByteArray whole =
        "login\r\n" + init() + "\x1b[1mbold\x1b[0m" + event(stop, nonce, "\x1b\\") + "after\x1b";
    const QByteArray visible = "login\r\n\x1b[1mbold\x1b[0mafter\x1b";
    for (qsizetype split = 1; split < whole.size(); ++split) {
        TerminalHookChannel channel;
        std::vector<TerminalHookEvent> events;
        auto shown = channel.filter(QByteArrayView(whole).first(split), events);
        shown += channel.filter(QByteArrayView(whole).sliced(split), events);
        shown += channel.filter("x", events);
        require(shown == visible + "x");
        require(events.size() == 1 && events.front().cli == QStringLiteral("claude") &&
                events.front().source == stop);
    }
}

// Only the first init binds; events need its nonce; other OSC text and
// broken sequences reach the terminal untouched.
void authenticates_events() {
    TerminalHookChannel channel;
    std::vector<TerminalHookEvent> events;
    const QJsonObject done{{"type", "agent-turn-complete"}};
    require(channel.filter(event(done), events).isEmpty() && events.empty());
    require(channel.filter(init("other"), events).isEmpty() && !channel.bound());
    require(channel.filter(init("codex", "short"), events).isEmpty() && !channel.bound());
    require(channel.filter(init("codex", "0123456789ABCDEF0123456789ABCDEF"), events).isEmpty() &&
            !channel.bound());
    require(channel.filter(init("codex"), events).isEmpty() && channel.bound());
    const QByteArray forged("ffffffffffffffffffffffffffffffff");
    require(channel.filter(init("claude", forged), events).isEmpty());
    require(channel.cli() == QStringLiteral("codex"));
    require(channel.filter(event(done, forged), events).isEmpty() && events.empty());
    require(channel.filter(QByteArray("\x1b]7717;lapis-event;") + nonce + ";!!\x07", events)
                .isEmpty() &&
            events.empty());
    require(channel.filter(event(done), events).isEmpty() && events.size() == 1);
    const QByteArray title = "\x1b]0;title\x07";
    require(channel.filter(title, events) == title);
    const QByteArray broken = "\x1b]7717;lapis-event;x\x1b[31m";
    require(channel.filter(broken, events) == broken);
    QByteArray endless = "\x1b]7717;lapis-event;";
    endless += QByteArray(TerminalHookChannel::max_sequence + 1, 'a');
    require(channel.filter(endless, events).isEmpty() && events.size() == 1);
    // The oversized candidate is quarantined through its terminator; a later
    // authenticated event still passes instead of being bypassed as raw output.
    require(channel.filter(event(done), events).isEmpty() && events.size() == 1);
    require(channel.filter(event(done), events).isEmpty() && events.size() == 2);
    const QByteArray oversized_done = init("codex") + endless + "\x07";
    for (qsizetype split = 1; split < oversized_done.size(); ++split) {
        TerminalHookChannel split_channel;
        std::vector<TerminalHookEvent> split_events;
        auto shown =
            split_channel.filter(QByteArrayView(oversized_done).first(split), split_events);
        shown += split_channel.filter(QByteArrayView(oversized_done).sliced(split), split_events);
        shown += split_channel.filter(event(done), split_events);
        require(shown.isEmpty() && split_events.size() == 1);
    }
}

// Codex reports only finished turns: a finished turn is observed, submitted
// input returns the activity to unknown, and a repeat changes nothing.
void notify_turns() {
    attention::State state("session", "codex-notify");
    NotifyTurns turns(state);
    require(!turns.submitted());
    require(!turns.completed({{"type", "other"}}, 1));
    require(turns.completed({{"type", "agent-turn-complete"}}, 1));
    require(state.ready() && state.activity() == attention::Activity::turn_completed);
    require(!turns.completed({{"type", "agent-turn-complete"}}, 2));
    require(turns.submitted() && state.activity() == attention::Activity::unknown);
    require(!turns.submitted());
    require(turns.completed({{"type", "agent-turn-complete"}}, 3));
    require(state.activity() == attention::Activity::turn_completed);
    // A failed transition must not consume an adapter sequence.
    state.overflow();
    require(!turns.completed({{"type", "agent-turn-complete"}}, 4));
    require(NotifyTurnsTestAccess::sequence(turns) == 4);
}
} // namespace

int main() {
    try {
        removes_sequences_across_reads();
        authenticates_events();
        notify_turns();
        std::cout << "terminal-hooks: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
