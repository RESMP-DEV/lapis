#include "gui_state.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

namespace lapis::desktop {
namespace {
// A healthy write is a small local file; these bound shutdown and tests only.
constexpr int kExitWaitMs = 5000;
constexpr int kTestWaitMs = 30000;
} // namespace

// One write at a time, off the GUI thread; a newer state waiting replaces an
// older one that has not started.
struct GuiState::Writer {
    std::mutex mutex;
    std::condition_variable idle;
    std::optional<QByteArray> pending;
    bool busy{};
    int writes{};
    QString path;

    static void store(const QString& path, const QByteArray& bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath(),
                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning().noquote() << "Window state not saved:" << file.errorString();
            return;
        }
        if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
            qWarning().noquote() << "Window state not saved: cannot make it private";
            file.cancelWriting();
            return;
        }
        if (file.write(bytes) != bytes.size() || !file.commit())
            qWarning().noquote() << "Window state not saved:" << file.errorString();
    }
    // Runs on a pool thread until nothing waits.
    void drain() {
        std::unique_lock lock(mutex);
        while (pending) {
            const auto bytes = std::move(*pending);
            pending.reset();
            lock.unlock();
            store(path, bytes);
            lock.lock();
            ++writes;
        }
        busy = false;
        idle.notify_all();
    }
};

GuiState::GuiState(QString path, QObject* parent)
    : QObject(parent), path_(std::move(path)), writer_(std::make_shared<Writer>()) {
    writer_->path = path_;
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &GuiState::write);
}

GuiState::~GuiState() {
    try {
        // write() also runs on aboutToQuit; this destructor call is a last
        // chance for owners destroyed before that signal.
        write();
        // Shutdown waits only as long as a healthy write takes; a writer stuck
        // on a stuck filesystem must not hold the window open.
        if (!waitForWrites(kExitWaitMs))
            qWarning().noquote() << "Window state write still in flight at exit";
    } catch (const std::exception& error) {
        qWarning().noquote() << "Window state not saved at exit:" << error.what();
    }
}

bool GuiState::load() {
    loaded_ = {};
    const QFileInfo info(path_);
    if (!info.exists())
        return false;
    const auto ignored = [this](const char* why) {
        qWarning().noquote() << "Window state ignored:" << path_ << QLatin1String(why);
        return false;
    };
    if (info.isSymLink() || !info.isFile())
        return ignored("is not a regular file");
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return ignored("is unreadable");
    // Read the limit, not the stat result: another process can grow the file
    // between stat and read, and the owner-only contract still has to hold.
    const auto bytes = file.read(kMaxBytes + 1);
    if (bytes.size() > kMaxBytes || !file.atEnd())
        return ignored("is too large");
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return ignored("is not valid JSON");
    const auto root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != kVersion)
        return ignored("has another version");
    loaded_ = root.value(QStringLiteral("sections")).toObject();
    const auto window = loaded_.value(QStringLiteral("window")).toObject();
    for (auto entry = window.constBegin(); entry != window.constEnd(); ++entry)
        if (window_.size() < kMaxWindowKeys)
            setValue(entry.key(), entry.value().toVariant());
    dirty_ = false;
    return true;
}

QJsonValue GuiState::section(const QString& name) const {
    return loaded_.contains(name) ? loaded_.value(name) : QJsonValue(QJsonValue::Undefined);
}

void GuiState::addSection(const QString& name, Save save) {
    if (name == QLatin1String("window")) {
        qWarning().noquote() << "Window state section name is reserved:" << name;
        return;
    }
    sections_.emplace_back(name, std::move(save));
}

QVariant GuiState::value(const QString& key) const { return window_.value(key).toVariant(); }

void GuiState::setValue(const QString& key, const QVariant& value) {
    if (key.isEmpty() || key.size() > 64)
        return;
    const auto json = QJsonValue::fromVariant(value);
    if (json.isUndefined())
        return;
    const auto size = json.isObject()  ? QJsonDocument(json.toObject()).toJson().size()
                      : json.isArray() ? QJsonDocument(json.toArray()).toJson().size()
                                       : json.toVariant().toString().toUtf8().size();
    if (size > kMaxWindowValueBytes ||
        (!window_.contains(key) && window_.size() >= kMaxWindowKeys) || window_.value(key) == json)
        return;
    window_.insert(key, json);
    touch();
}

QByteArray GuiState::encode() const {
    QJsonObject sections{{QStringLiteral("window"), window_}};
    for (const auto& [name, save] : sections_)
        sections.insert(name, save());
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), kVersion},
                                     {QStringLiteral("sections"), sections}})
        .toJson(QJsonDocument::Compact);
}

void GuiState::touch() {
    dirty_ = true;
    if (timer_.isActive())
        return;
    // Publish-to-publish: due an interval after the last write, so the first
    // change after a quiet spell goes out on the next turn of the loop.
    const qint64 since = last_write_.isValid() ? last_write_.elapsed() : interval_ms_;
    timer_.start(static_cast<int>(std::max<qint64>(0, interval_ms_ - since)));
}

void GuiState::write() {
    if (!dirty_)
        return;
    dirty_ = false;
    last_write_.start();
    auto bytes = encode();
    if (bytes.size() > kMaxBytes) {
        qWarning().noquote() << "Window state not saved: over" << kMaxBytes << "bytes";
        return;
    }
    std::unique_lock lock(writer_->mutex);
    writer_->pending = std::move(bytes);
    if (writer_->busy)
        return;
    writer_->busy = true;
    lock.unlock();
    QThreadPool::globalInstance()->start([writer = writer_] { writer->drain(); });
}

void GuiState::flush() {
    timer_.stop();
    write();
    waitForTesting();
}

bool GuiState::waitForWrites(int timeout_ms) const {
    std::unique_lock lock(writer_->mutex);
    return writer_->idle.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [this] { return !writer_->busy; });
}

void GuiState::waitForTesting() const { static_cast<void>(waitForWrites(kTestWaitMs)); }

int GuiState::writesForTesting() const {
    const std::scoped_lock lock(writer_->mutex);
    return writer_->writes;
}

} // namespace lapis::desktop
