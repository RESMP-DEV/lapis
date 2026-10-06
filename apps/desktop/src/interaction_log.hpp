#ifndef LAPIS_DESKTOP_INTERACTION_LOG_HPP
#define LAPIS_DESKTOP_INTERACTION_LOG_HPP

#include <QJsonObject>
#include <QJsonValue>
#include <QPointF>
#include <QString>
#include <QtGlobal>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

// The local interaction log: what the person does in lapis's own windows,
// one JSON line per record in <data>/runtime/interaction.jsonl, for modeling
// how they work with their agents. This file holds the pieces that need no
// window: the setting, the secret-prompt rule, pointer sampling and the
// background writer. InteractionRecorder (interaction_recorder.hpp) turns
// events into records.
namespace lapis::desktop {

// {"interactionLog": {"enabled": true, "pointerSampleMs": 50, "maxFileMiB": 64,
// "maxFiles": 8}}. Off unless the config turns it on.
struct InteractionLogSettings {
    static constexpr int kDefaultPointerSampleMs = 50;
    static constexpr int kMaximumPointerSampleMs = 10'000;
    static constexpr int kDefaultMaxFileMiB = 64;
    static constexpr int kMaximumMaxFileMiB = 1024;
    static constexpr int kDefaultMaxFiles = 8;
    static constexpr int kMaximumMaxFiles = 64;
    bool enabled{};
    // At most one pointer-movement record per this many milliseconds while
    // the pointer moves; 0 records no movement at all.
    int pointerSampleMs{kDefaultPointerSampleMs};
    // Size of one log file before it rotates, in MiB (1 to 1024).
    int maxFileMiB{kDefaultMaxFileMiB};
    // Files kept, the current one included (1 to 64).
    int maxFiles{kDefaultMaxFiles};
    bool operator==(const InteractionLogSettings&) const = default;
};
// Missing or invalid values keep their defaults; out-of-range numbers are
// clamped to their limits.
[[nodiscard]] InteractionLogSettings parse_interaction_log(const QJsonValue& value);

// True when the text on the cursor's row looks like a prompt for a secret:
// it names a password, passphrase, passcode, PIN or OTP (whole words, any
// case, also one-time and verification codes), or it ends in a colon right
// after "secret", "token" or "key" ("Enter API key:"). Keys typed there are
// recorded without their text.
[[nodiscard]] bool likely_secret_prompt(const QString& cursor_row);

// The class of a key whose identity is withheld: "character", "space",
// "enter", "backspace", "tab", "escape", "navigation", "function" or
// "modifier".
[[nodiscard]] QString key_class(int qt_key, const QString& text);

// Pointer movement: the first move after a quiet interval is recorded at
// once, later ones within `interval_ms` are folded into one trailing sample
// that carries the latest position and how many moves it stands for.
class PointerSampler {
  public:
    struct Sample {
        qint64 at_ms{};
        QPointF position;
        int folded{}; // moves this sample stands for, itself included
    };
    explicit PointerSampler(int interval_ms) : interval_ms_(interval_ms) {}
    // The sample to record now, if any; otherwise the move waits.
    [[nodiscard]] std::optional<Sample> move(qint64 now_ms, QPointF position);
    // The waiting sample once its interval has passed (or always, with
    // `force`); recorded before any other record so the order holds.
    [[nodiscard]] std::optional<Sample> flush(qint64 now_ms, bool force);
    // When the waiting sample becomes due, or -1 when none waits.
    [[nodiscard]] qint64 due_ms() const;
    [[nodiscard]] int interval() const { return interval_ms_; }
    void reset() { pending_.reset(); }

  private:
    int interval_ms_{};
    qint64 last_emit_ms_{-1};
    std::optional<Sample> pending_;
};

// Appends records on its own thread so typing never waits on the disk.
// Records are serialized and written there in batches; when the queue is
// full new records are dropped and counted, and a `dropped` record notes
// the gap once there is room. Failure to write never reaches input.
class InteractionWriter {
  public:
    static constexpr std::size_t kQueueLimit = 16'384;
    struct Limits {
        qint64 maxFileBytes{};
        int maxFiles{};
    };
    // `path` is the current file; predecessors are path.1 ... path.(files-1).
    InteractionWriter(QString path, Limits limits);
    ~InteractionWriter();
    InteractionWriter(const InteractionWriter&) = delete;
    InteractionWriter& operator=(const InteractionWriter&) = delete;
    InteractionWriter(InteractionWriter&&) = delete;
    InteractionWriter& operator=(InteractionWriter&&) = delete;
    // Queues one record; `wall_ms` is the wall clock in milliseconds since
    // the epoch, formatted on the writer thread. False when it was dropped.
    bool append(QJsonObject record, qint64 wall_ms);
    // Waits until everything queued so far is on disk (tests and shutdown).
    void drain();
    [[nodiscard]] quint64 dropped() const;
    [[nodiscard]] quint64 failures() const;
    [[nodiscard]] const QString& path() const { return path_; }

  private:
    struct Item {
        QJsonObject record;
        qint64 wall_ms{};
    };
    void run();
    bool writeBatch(std::deque<Item>& batch);
    bool openFile();
    bool rotate();
    QString path_;
    qint64 max_file_bytes_{};
    int max_files_{};
    int fd_{-1};
    qint64 size_{};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable drained_;
    std::deque<Item> queue_;
    quint64 dropped_{};
    quint64 unreported_drops_{};
    quint64 failures_{};
    quint64 queued_total_{};
    quint64 written_total_{};
    bool stopping_{};
    bool warned_{};
    std::thread thread_;
};

} // namespace lapis::desktop
#endif // LAPIS_DESKTOP_INTERACTION_LOG_HPP
