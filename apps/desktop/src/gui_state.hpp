#ifndef LAPIS_DESKTOP_GUI_STATE_HPP
#define LAPIS_DESKTOP_GUI_STATE_HPP

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <functional>
#include <memory>
#include <vector>

namespace lapis::desktop {

// What the window knows that changes often and lives only in its memory: the
// guessed next prompts and their bookkeeping, unseen marks, what was last seen
// of each agent, closed agents and the window's own choices. The session
// services keep the agents running across a window restart (an install swaps
// the app and opens the window again); this keeps the rest, so the window
// comes back as it was.
//
// One owner-only JSON file (`runtime/gui_state.json`), versioned, written
// whole and atomically, at most 1 MiB. Each owner registers a section: a name
// and a function that returns its state. A change asks for a write; writes are
// rate-limited publish-to-publish (the first change after a quiet interval is
// written at once, later ones at the interval's end, with the newest state),
// and run off the GUI thread. A missing, corrupt, oversized or other-version
// file is ignored with a warning; owners validate what they restore.
class GuiState final : public QObject {
    Q_OBJECT
  public:
    static constexpr int kVersion = 1;
    static constexpr qint64 kMaxBytes = qint64{1024} * 1024;
    static constexpr int kIntervalMs = 2000;
    // The window's own small choices (QML): at most this many keys, each
    // name and value bounded.
    static constexpr int kMaxWindowKeys = 32;
    static constexpr qsizetype kMaxWindowValueBytes = 4096;

    using Save = std::function<QJsonValue()>;

    explicit GuiState(QString path, QObject* parent = nullptr);
    // Writes what changed since the last write: every section's owner must
    // outlive this.
    ~GuiState() override;
    GuiState(const GuiState&) = delete;
    GuiState& operator=(const GuiState&) = delete;
    GuiState(GuiState&&) = delete;
    GuiState& operator=(GuiState&&) = delete;

    [[nodiscard]] const QString& path() const { return path_; }
    // Reads the file. False, with a warning, when it is missing, unreadable,
    // corrupt or of another version; every section then reads as empty.
    bool load();
    // A loaded section, or undefined.
    [[nodiscard]] QJsonValue section(const QString& name) const;
    // Adds an owner's section, written with every save. "window" is reserved
    // for the window's own choices.
    void addSection(const QString& name, Save save);
    // Something changed: write soon.
    Q_INVOKABLE void touch();
    // Writes now, waiting for any write in flight (at exit).
    void flush();

    // The window's own choices, for QML.
    Q_INVOKABLE [[nodiscard]] QVariant value(const QString& key) const;
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);

    // Tests.
    void setIntervalForTesting(int ms) { interval_ms_ = ms; }
    [[nodiscard]] int writesForTesting() const;
    // Waits for writes in flight.
    void waitForTesting() const;
    // True once no write is in flight, or after the bounded wait.
    [[nodiscard]] bool waitForWrites(int timeout_ms) const;

  private:
    struct Writer;
    [[nodiscard]] QByteArray encode() const;
    void write();
    QString path_;
    QJsonObject loaded_;
    std::vector<std::pair<QString, Save>> sections_;
    QJsonObject window_;
    QTimer timer_;
    QElapsedTimer last_write_;
    int interval_ms_{kIntervalMs};
    bool dirty_{};
    std::shared_ptr<Writer> writer_;
};

} // namespace lapis::desktop
#endif
