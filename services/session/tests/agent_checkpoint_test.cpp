#include "agent_checkpoint.hpp"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using lapis::session::CheckpointScanner;
using lapis::session::ResumeRecord;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray sequence(const QJsonObject& checkpoint, const char* terminator = "\x07") {
    return QByteArrayLiteral("\x1b]1337;SetUserVar=agent_checkpoint=") +
           QJsonDocument(checkpoint).toJson(QJsonDocument::Compact).toBase64() + terminator;
}

// The hook sequence is found inside ordinary output, across read boundaries,
// with either terminator; the newest checkpoint wins.
void scanner_finds_checkpoints() {
    CheckpointScanner scanner;
    const auto first = sequence({{"agent", "claude"}, {"session_id", "abc-123"}});
    const auto whole = QByteArrayLiteral("prompt> ") + first + QByteArrayLiteral("more output");
    for (qsizetype split = 1; split < whole.size(); split += 7) {
        CheckpointScanner fresh;
        const auto head = fresh.scan(QByteArrayView(whole).first(split));
        const auto tail = fresh.scan(QByteArrayView(whole).sliced(split));
        const auto found = head ? head : tail;
        require(found && found->agent == QStringLiteral("claude") &&
                    found->session_id == QStringLiteral("abc-123"),
                "a checkpoint split across reads is found");
    }
    const auto found =
        scanner.scan(sequence({{"agent", "Codex"}, {"session_id", "first"}}) +
                     sequence({{"agent", "codex"}, {"session_id", "second"}}, "\x1b\\"));
    require(found && found->agent == QStringLiteral("codex") &&
                found->session_id == QStringLiteral("second"),
            "the newest checkpoint in a read wins, with either terminator");
    // A lost first terminator must not glue its value to the next checkpoint.
    const auto terminated = sequence({{"agent", "kimi"}, {"session_id", "recover"}});
    CheckpointScanner recovered;
    const auto recovered_record = recovered.scan(terminated.chopped(1) + terminated);
    require(recovered_record && recovered_record->agent == QStringLiteral("kimi") &&
                recovered_record->session_id == QStringLiteral("recover"),
            "a marker without a terminator does not hide the next checkpoint");
    require(!scanner.scan("plain output without any checkpoint"), "plain output has none");
}

void scanner_rejects_unusable_checkpoints() {
    CheckpointScanner scanner;
    require(!scanner.scan(sequence({{"agent", "claude"}, {"session_id", "--dangerous"}})),
            "an identity that looks like an option is refused");
    require(!scanner.scan(sequence({{"agent", "vim"}, {"session_id", "abc"}})),
            "an unknown agent is refused");
    require(!scanner.scan(sequence({{"agent", "pi"}, {"session_id", "abc"}})),
            "an agent without a lapis resume route is refused");
    require(!scanner.scan(sequence({{"agent", "claude"}, {"session_id", "abc"}, {"version", 2}})),
            "an unknown checkpoint schema version is refused");
    const auto forged = scanner.scan(sequence(
        {{"agent", "claude"}, {"session_id", "abc"}, {"version", 1}, {"source", "observer"}}));
    require(forged && forged->source == lapis::session::ResumeSource::terminal,
            "terminal data cannot attest to observer provenance");
    require(!scanner.scan(sequence({{"agent", "claude"}, {"session_id", "has space"}})),
            "an identity with whitespace is refused");
    require(!scanner.scan(sequence(
                {{"agent", "claude"}, {"session_id", "abc"}, {"host", "someone-elses-server"}})),
            "a checkpoint from a remote host is refused");
    require(scanner
                .scan(sequence({{"agent", "claude"},
                                {"session_id", "abc"},
                                {"host", QSysInfo::machineHostName()}}))
                .has_value(),
            "a checkpoint from this host is accepted");
    require(!scanner.scan(QByteArrayLiteral("\x1b]1337;SetUserVar=agent_checkpoint=%%%\x07")),
            "malformed base64 is refused");
    // An unterminated sequence is bounded and does not hide later checkpoints.
    CheckpointScanner bounded;
    require(!bounded.scan(QByteArrayLiteral("\x1b]1337;SetUserVar=agent_checkpoint=") +
                          QByteArray(20000, 'A')),
            "an unterminated sequence yields nothing");
    require(bounded.scan(sequence({{"agent", "kimi"}, {"session_id", "k1"}})).has_value(),
            "a later checkpoint is still found after an oversized one");
}

// A valid OSC payload can still be forged. The service must accept only the
// agent identity consistent with the CLI it actually launched.
void checkpoints_are_bound_to_the_launched_agent() {
    using lapis::session::AgentMode;
    using lapis::session::checkpoint_agent_for_launch;
    using lapis::session::LaunchSpec;
    require(
        checkpoint_agent_for_launch({QStringLiteral("/x/codex"), {}, {}, {}, AgentMode::codex}) ==
            QStringLiteral("codex"),
        "managed Codex checkpoints are named codex");
    require(
        checkpoint_agent_for_launch({QStringLiteral("/x/claude"), {}, {}, {}, AgentMode::claude}) ==
            QStringLiteral("claude"),
        "managed Claude checkpoints are named claude");
    require(checkpoint_agent_for_launch(
                {QStringLiteral("/opt/tools/Kimi"), {}, {}, {}, AgentMode::terminal}) ==
                QStringLiteral("kimi"),
            "terminal checkpoints use the launched executable name");
    require(checkpoint_agent_for_launch(
                {QStringLiteral("/opt/tools/other"), {}, {}, {}, AgentMode::terminal}) ==
                QStringLiteral("other"),
            "terminal checkpoints do not inherit another harness name");
}

// Records are private files; unsafe existing files are never read or replaced.
void records_are_private() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const auto endpoint = QDir(directory.path()).filePath(QStringLiteral("agent.sock"));
    require(!lapis::session::read_resume_record(endpoint), "no record yet");
    lapis::session::write_resume_record(endpoint, {QStringLiteral("claude"), QStringLiteral("s-1"),
                                                   lapis::session::ResumeSource::observer});
    const auto record = lapis::session::read_resume_record(endpoint);
    require(record && record->agent == QStringLiteral("claude") &&
                record->session_id == QStringLiteral("s-1") &&
                record->source == lapis::session::ResumeSource::observer,
            "a written record preserves observer provenance");
    const auto record_path = endpoint + QStringLiteral(".resume");
    QFile foreign_case(record_path);
    require(foreign_case.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "open the record for a foreign spelling");
    const QJsonObject uppercase{{"version", 1}, {"agent", "Claude"}, {"session_id", "s-1"}};
    require(foreign_case.write(QJsonDocument(uppercase).toJson(QJsonDocument::Compact)) > 0 &&
                foreign_case.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "write a normalized-agent test record");
    foreign_case.close();
    const auto normalized = lapis::session::read_resume_record(endpoint);
    require(normalized && normalized->agent == QStringLiteral("claude") &&
                normalized->session_id == QStringLiteral("s-1") &&
                normalized->source == lapis::session::ResumeSource::terminal,
            "record agents use the same lowercase spelling as checkpoints");
    QFile wrong_version(record_path);
    require(wrong_version.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "open the record for a version test");
    const QJsonObject unsupported_version{
        {"version", 3}, {"agent", "claude"}, {"session_id", "s-1"}};
    require(wrong_version.write(QJsonDocument(unsupported_version).toJson(QJsonDocument::Compact)) >
                    0 &&
                wrong_version.setPermissions(QFile::ReadOwner | QFile::WriteOwner),
            "write a wrong-version test record");
    wrong_version.close();
    require(!lapis::session::read_resume_record(endpoint),
            "a record from another durable schema version is not trusted");
    lapis::session::write_resume_record(endpoint,
                                        {QStringLiteral("claude"), QStringLiteral("s-1")});
    struct stat info{};
    require(::stat(QFile::encodeName(endpoint + QStringLiteral(".resume")).constData(), &info) ==
                    0 &&
                (info.st_mode & 07777U) == 0600U,
            "records are owner-only");
    require(::chmod(QFile::encodeName(endpoint + QStringLiteral(".resume")).constData(), 0644) == 0,
            "loosen the record");
    require(!lapis::session::read_resume_record(endpoint), "a shared record is not trusted");
    bool refused = false;
    try {
        lapis::session::write_resume_record(endpoint,
                                            {QStringLiteral("claude"), QStringLiteral("s-2")});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused, "a shared record is not replaced");
    require(::chmod(QFile::encodeName(record_path).constData(), 0600) == 0 &&
                lapis::session::read_resume_record(endpoint).has_value(),
            "restore a valid private target before testing symlink rejection");
    const auto link_endpoint = QDir(directory.path()).filePath(QStringLiteral("link.sock"));
    require(QFile::link(endpoint + QStringLiteral(".resume"),
                        link_endpoint + QStringLiteral(".resume")),
            "create a symlinked record");
    require(!lapis::session::read_resume_record(link_endpoint), "a symlinked record is ignored");
    const auto fifo_endpoint = QDir(directory.path()).filePath(QStringLiteral("pipe.sock"));
    require(::mkfifo(QFile::encodeName(fifo_endpoint + QStringLiteral(".resume")).constData(),
                     0600) == 0,
            "create a non-regular record");
    require(!lapis::session::read_resume_record(fifo_endpoint),
            "opening an untrusted FIFO does not block waiting for a writer");
}
// An app-server's open files name its thread's rollout; subagent rollouts
// opened later do not replace the conversation.
void codex_threads_from_open_files() {
    const QString listing =
        QStringLiteral("p58543\nfcwd\nn/Users/me/project\nf12\n"
                       "n/Users/me/.codex/sessions/2026/09/23/"
                       "rollout-2026-09-23T16-20-11-01a0d055-9999-7581-be09-2abee03315ea.jsonl\n"
                       "f13\nn/Users/me/.codex/sessions/2026/09/23/"
                       "rollout-2026-09-23T16-13-58-01a0d055-2f25-7581-be09-2abee03315ea.jsonl\n"
                       "f14\nn/Users/me/.codex/log/codex-tui.log\n");
    require(lapis::session::codex_thread_from_open_files(listing) ==
                QStringLiteral("01a0d055-2f25-7581-be09-2abee03315ea"),
            "the earliest open rollout names the conversation");
    require(!lapis::session::codex_thread_from_open_files(
                 QStringLiteral("p1\nn/tmp/rollout-notes.jsonl\nn/Users/me/.codex/log/x\n"))
                 .has_value(),
            "files outside Codex sessions are not rollouts");
}
} // namespace

int main() {
    try {
        scanner_finds_checkpoints();
        scanner_rejects_unusable_checkpoints();
        checkpoints_are_bound_to_the_launched_agent();
        records_are_private();
        codex_threads_from_open_files();
        std::cout << "Agent checkpoints across reads, identity and host checks, and private "
                     "resume records passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
