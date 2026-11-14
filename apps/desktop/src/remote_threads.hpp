#ifndef LAPIS_DESKTOP_REMOTE_THREADS_HPP
#define LAPIS_DESKTOP_REMOTE_THREADS_HPP

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QProcess;

namespace lapis::desktop {

// The visual view for a Claude Code agent on another machine. The agent's
// conversation file lives there, so a helper (remote_thread.py, sent to that
// machine's python3 on ssh's standard input, nothing installed or written
// there) streams it into a private cache on this Mac, which the view reads
// as it reads a local transcript:
//
//   <runtime>/remote/<machine>/<conversation>.jsonl       the mirror
//   <runtime>/remote/<machine>/<conversation>.tail.jsonl  its end, shown first
//   <runtime>/remote/<machine>/<conversation>.meta.json   identity to resume
//
// Folders are 0700 and files 0600. One helper runs per conversation the view
// shows (or asked for in the last few seconds), stopped shortly after it is
// no longer shown; nothing reads ahead. A helper that ends is restarted with
// the agents' reconnect waits (2, 5, 10, 20, 30, 60 s), resuming at the
// cache's size; the helper checks the file is the same one (inode, the
// first 4 KiB, its size) and otherwise resets the copy.
//
// Attachments the composer sends to such an agent are copied first into
// ~/.cache/lapis/attachments/ on that machine (upload), the only thing lapis
// writes there; the message names that copy, never a path on this Mac.
//
// Owned by Workspace, GUI thread only. The processes are children of this
// object and end with it.
class RemoteThreads final : public QObject {
    Q_OBJECT
  public:
    // What a remote agent's view needs: its ssh host, its folder there as
    // its command names it (~ is that machine's home), the conversation id
    // (a UUID) and the ssh program its own command uses.
    struct Target {
        QString machine;
        QString folder;
        QString conversation;
        QString ssh;
        friend bool operator==(const Target&, const Target&) = default;
    };
    // The current target of an agent, or nullopt when it is not a Claude
    // Code agent on another machine (or has no conversation id).
    using Resolve = std::function<std::optional<Target>(const QString& agent)>;
    // `root` is the runtime folder (beside the workspace registry); the cache
    // is root/remote. An empty root disables mirroring.
    RemoteThreads(QString root, Resolve resolve, QObject* parent = nullptr);
    ~RemoteThreads() override;
    RemoteThreads(const RemoteThreads&) = delete;
    RemoteThreads& operator=(const RemoteThreads&) = delete;

    // The file the view should read for `agent`: the mirror once it holds a
    // complete copy, its end while the rest arrives, or empty before any
    // data. Asking counts as wanting the thread: its helper starts (or keeps
    // running), and moves to another conversation when the agent's changed.
    [[nodiscard]] QString file(const QString& agent);
    // The agent whose thread shows now ("" for none): its helper keeps
    // running; the others stop once not asked for a while.
    void setShown(const QString& agent);
    // Whether the view can show the thread: false only after a failure with
    // nothing cached to show.
    [[nodiscard]] bool supported(const QString& agent) const;
    // Why the thread cannot follow now (no python3, ssh needs a password,
    // no such conversation there, lost connection), or empty.
    [[nodiscard]] QString unavailable(const QString& agent) const;
    // For the view: {machine, phase (connecting, waiting, following,
    // retrying, failed, idle), reason, status (the empty thread's line),
    // cached, behind (a failure while a cached copy shows)}. Empty for an
    // agent that is not mirrored.
    [[nodiscard]] QVariantMap state(const QString& agent) const;
    // The cache's size limit (visual.remoteCacheMB). Least recently shown
    // conversations go first; a followed one never does.
    void setCacheLimit(qint64 bytes);
    [[nodiscard]] qint64 cacheLimit() const { return cache_limit_; }
    // Removes conversations until the cache fits the limit.
    void trimCache();
    // Forgets an agent that closed: its helper stops (its cache stays).
    void forget(const QString& agent);

    // Copies `local` to the agent's machine (see the class comment); returns
    // a token naming the upload, or empty with `error` set. uploadFinished
    // reports the path there, or why it failed.
    QString upload(const QString& agent, const QString& local, QString* error = nullptr);
    static constexpr qint64 upload_limit = qint64{20} * 1024 * 1024;

    // The cache files, for tests and tools.
    [[nodiscard]] QString cacheFolder(const QString& machine) const;
    [[nodiscard]] bool running(const QString& agent) const;
    // Shorter waits for tests: how long a thread no longer shown keeps its
    // helper, each reconnect wait, how often conversations are checked, and
    // the tail sizes the helper is asked for.
    struct Timing {
        std::chrono::milliseconds keep{15000};
        std::vector<std::chrono::milliseconds> waits{
            std::chrono::seconds(2),  std::chrono::seconds(5),  std::chrono::seconds(10),
            std::chrono::seconds(20), std::chrono::seconds(30), std::chrono::seconds(60)};
        std::chrono::milliseconds check{1000};
        qint64 tail_bytes{qint64{2} * 1024 * 1024};
    };
    void setTimingForTesting(Timing timing);
    // Ends a helper as a dropped connection would (tests).
    void killForTesting(const QString& agent);

  signals:
    // The agent's file, support or state changed: the view asks again.
    void changed(const QString& agent);
    void uploadProgress(const QString& token, double fraction);
    // `remote` is the copy's absolute path on that machine; empty with
    // `error` when the upload failed.
    void uploadFinished(const QString& token, const QString& remote, const QString& error);

  private:
    struct Mirror;
    QString root_;
    Resolve resolve_;
    Timing timing_;
    qint64 cache_limit_{qint64{2048} * 1024 * 1024};
    QString shown_;
    QTimer check_;
    std::vector<std::unique_ptr<Mirror>> mirrors_;
    QHash<QString, QPointer<QProcess>> uploads_;
    quint64 upload_serial_{};
    [[nodiscard]] Mirror* mirror(const QString& agent) const;
    Mirror& ensure(const QString& agent, const Target& target);
    void open(Mirror& mirror);
    void start(Mirror& mirror);
    void stop(Mirror& mirror);
    void retry(Mirror& mirror);
    void receive(Mirror& mirror);
    bool handle(Mirror& mirror, char type, const QByteArray& payload);
    void finished(Mirror& mirror, int code, bool crashed);
    void saveMeta(const Mirror& mirror) const;
    void reset(Mirror& mirror);
    void check();
    void notify(const QString& agent);
    [[nodiscard]] QString path(const Mirror& mirror) const;
    [[nodiscard]] QStringList helperArguments(const Mirror& mirror) const;
};

// The ssh options a helper uses (BatchMode: it never asks for a password).
[[nodiscard]] QStringList remote_helper_options();
// One POSIX shell word for a remote command line ssh passes to that
// machine's shell; every helper argument goes through it.
[[nodiscard]] QString remote_shell_word(const QString& text);

} // namespace lapis::desktop
#endif
