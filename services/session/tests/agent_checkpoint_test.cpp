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
    require(!scanner.scan("plain output without any checkpoint"), "plain output has none");
}

void scanner_rejects_unusable_checkpoints() {
    CheckpointScanner scanner;
    require(!scanner.scan(sequence({{"agent", "claude"}, {"session_id", "--dangerous"}})),
            "an identity that looks like an option is refused");
    require(!scanner.scan(sequence({{"agent", "vim"}, {"session_id", "abc"}})),
            "an unknown agent is refused");
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

// Records are private files; unsafe existing files are never read or replaced.
void records_are_private() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const auto endpoint = QDir(directory.path()).filePath(QStringLiteral("agent.sock"));
    require(!lapis::session::read_resume_record(endpoint), "no record yet");
    lapis::session::write_resume_record(endpoint,
                                        {QStringLiteral("claude"), QStringLiteral("s-1")});
    const auto record = lapis::session::read_resume_record(endpoint);
    require(record && record->agent == QStringLiteral("claude") &&
                record->session_id == QStringLiteral("s-1"),
            "a written record reads back");
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
    const auto link_endpoint = QDir(directory.path()).filePath(QStringLiteral("link.sock"));
    require(QFile::link(endpoint + QStringLiteral(".resume"),
                        link_endpoint + QStringLiteral(".resume")),
            "create a symlinked record");
    require(!lapis::session::read_resume_record(link_endpoint), "a symlinked record is ignored");
}
} // namespace

int main() {
    try {
        scanner_finds_checkpoints();
        scanner_rejects_unusable_checkpoints();
        records_are_private();
        std::cout << "Agent checkpoints across reads, identity and host checks, and private "
                     "resume records passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
