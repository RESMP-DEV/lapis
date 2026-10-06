#ifndef LAPIS_TERMINAL_HOOKS_HPP
#define LAPIS_TERMINAL_HOOKS_HPP
#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <lapis/session/attention.hpp>
#include <vector>

namespace lapis::session {
// An agent on another machine runs behind ssh, where lapis's local hook
// socket and app-server cannot reach. Its hooks (Claude Code) or notify
// program (Codex) instead print private sequences to the agent's own
// terminal, which ssh already carries here:
//
//   ESC ] 7717 ; lapis-init ; <cli> ; <nonce> BEL    once, by lapis's launch
//   ESC ] 7717 ; lapis-event ; <nonce> ; <base64 JSON> BEL    per hook
//
// The remote shell makes the 32-hex-digit nonce just before starting the CLI
// and keeps it only in that process tree's environment; it is on no command
// line and in no file. The first init sequence binds it; later inits, and
// events without it, are ignored, so text the agent displays cannot pose as
// a hook. Every lapis sequence is removed from the output before the
// terminal sees it. Frames carry bounded metadata only, never prompts.
struct TerminalHookEvent {
    QString cli; // "claude" or "codex", as bound by the init sequence
    QJsonObject source;
};

class TerminalHookChannel {
  public:
    static constexpr qsizetype max_sequence = qsizetype{24} * 1024;
    // The output without lapis sequences, in order. A possible sequence cut
    // by the end of this read is held until the next one; it cannot draw
    // anything by itself. Authenticated events are appended to `events`.
    [[nodiscard]] QByteArray filter(QByteArrayView output, std::vector<TerminalHookEvent>& events);
    [[nodiscard]] bool bound() const { return !nonce_.isEmpty(); }
    [[nodiscard]] const QString& cli() const { return cli_; }

  private:
    void accept(QByteArrayView body, std::vector<TerminalHookEvent>& events);
    QByteArray carry_;
    QByteArray nonce_;
    QString cli_;
};

// Turn boundaries of a CLI whose only real signal is a finished turn: Codex
// runs its notify program when a turn completes and has no matching start
// signal. A completed turn is observed; once the person submits input the
// activity is unknown again, and the desktop falls back to its output
// estimate until the next completed turn.
class NotifyTurns {
  public:
    explicit NotifyTurns(attention::State& state) : state_(state) {}
    // Whether the state changed.
    bool completed(const QJsonObject& source, attention::Tick now);
    bool submitted();
    [[nodiscard]] static QString diagnostic();

  private:
    attention::State& state_;
    std::uint64_t sequence_{};
};
} // namespace lapis::session
#endif
