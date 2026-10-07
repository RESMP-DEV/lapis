#include "interaction_log.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace lapis::desktop {
namespace {
int clamped_int(const QJsonObject& object, const char* name, int fallback, int low, int high) {
    const auto value = object.value(QLatin1String(name));
    if (!value.isDouble())
        return fallback;
    const double number = value.toDouble();
    if (number < low)
        return low;
    if (number > high)
        return high;
    return static_cast<int>(number);
}

constexpr mode_t kPrivateFile = S_IRUSR | S_IWUSR;
constexpr mode_t kPrivateFolder = S_IRWXU;
constexpr mode_t kShared = S_IRWXG | S_IRWXO;

// The folder exists and only its owner may use it; one created here starts
// that way. A wider folder is refused rather than changed.
bool private_folder(const QString& folder) {
    const QByteArray native = QFile::encodeName(folder);
    struct stat info{};
    if (::stat(native.constData(), &info) != 0) {
        if (!QDir().mkpath(folder))
            return false;
        if (::chmod(native.constData(), kPrivateFolder) != 0)
            return false;
        return ::stat(native.constData(), &info) == 0 && (info.st_mode & kShared) == 0;
    }
    return S_ISDIR(info.st_mode) && (info.st_mode & kShared) == 0 && info.st_uid == ::getuid();
}

bool write_all(int fd, const QByteArray& bytes) {
    const char* data = bytes.constData();
    qsizetype left = bytes.size();
    while (left > 0) {
        const ssize_t written = ::write(fd, data, static_cast<size_t>(left));
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += written;
        left -= written;
    }
    return true;
}

QString wall_time(qint64 wall_ms) {
    return QDateTime::fromMSecsSinceEpoch(wall_ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}
} // namespace

InteractionLogSettings parse_interaction_log(const QJsonValue& value) {
    using S = InteractionLogSettings;
    S settings;
    const auto object = value.toObject();
    if (object.value(QStringLiteral("enabled")).isBool())
        settings.enabled = object.value(QStringLiteral("enabled")).toBool();
    settings.pointerSampleMs = clamped_int(object, "pointerSampleMs", S::kDefaultPointerSampleMs, 0,
                                           S::kMaximumPointerSampleMs);
    settings.maxFileMiB =
        clamped_int(object, "maxFileMiB", S::kDefaultMaxFileMiB, 1, S::kMaximumMaxFileMiB);
    settings.maxFiles =
        clamped_int(object, "maxFiles", S::kDefaultMaxFiles, 1, S::kMaximumMaxFiles);
    return settings;
}

bool likely_secret_prompt(const QString& cursor_row) {
    static const QRegularExpression named(
        QStringLiteral("\\b(pass(word|phrase|code)s?|pins?|otp|one[- ]time (pass)?code|"
                       "verification code|security code)\\b"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression ending(QStringLiteral("\\b(secret|token|key)\\s*:\\s*$"),
                                           QRegularExpression::CaseInsensitiveOption);
    return named.match(cursor_row).hasMatch() || ending.match(cursor_row).hasMatch();
}

QString key_class(int qt_key, const QString& text) {
    switch (qt_key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return QStringLiteral("enter");
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
        return QStringLiteral("backspace");
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return QStringLiteral("tab");
    case Qt::Key_Escape:
        return QStringLiteral("escape");
    case Qt::Key_Space:
        return QStringLiteral("space");
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
        return QStringLiteral("modifier");
    default:
        break;
    }
    if (qt_key >= Qt::Key_Home && qt_key <= Qt::Key_PageDown)
        return QStringLiteral("navigation");
    if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F35)
        return QStringLiteral("function");
    return text.isEmpty() ? QStringLiteral("other") : QStringLiteral("character");
}

std::optional<PointerSampler::Sample> PointerSampler::move(qint64 now_ms, QPointF position) {
    if (interval_ms_ <= 0)
        return std::nullopt;
    if (pending_) {
        pending_->at_ms = now_ms;
        pending_->position = position;
        ++pending_->folded;
        return flush(now_ms, false);
    }
    if (last_emit_ms_ < 0 || now_ms - last_emit_ms_ >= interval_ms_) {
        last_emit_ms_ = now_ms;
        return Sample{now_ms, position, 1};
    }
    pending_ = Sample{now_ms, position, 1};
    return std::nullopt;
}

std::optional<PointerSampler::Sample> PointerSampler::flush(qint64 now_ms, bool force) {
    if (!pending_ || (!force && now_ms - last_emit_ms_ < interval_ms_))
        return std::nullopt;
    auto sample = *pending_;
    pending_.reset();
    last_emit_ms_ = now_ms;
    return sample;
}

qint64 PointerSampler::due_ms() const { return pending_ ? last_emit_ms_ + interval_ms_ : -1; }

InteractionWriter::InteractionWriter(QString path, Limits limits)
    : path_(std::move(path)), max_file_bytes_(std::max<qint64>(limits.maxFileBytes, 1024)),
      max_files_(std::max(limits.maxFiles, 1)), thread_([this] { run(); }) {}

InteractionWriter::~InteractionWriter() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
    if (fd_ >= 0)
        ::close(fd_);
}

bool InteractionWriter::append(QJsonObject record, qint64 wall_ms) {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_ || queue_.size() >= kQueueLimit) {
            ++dropped_;
            ++unreported_drops_;
            return false;
        }
        if (unreported_drops_ > 0) {
            queue_.push_back(
                {QJsonObject{{QStringLiteral("kind"), QStringLiteral("dropped")},
                             {QStringLiteral("count"), static_cast<qint64>(unreported_drops_)}},
                 wall_ms});
            ++queued_total_;
            unreported_drops_ = 0;
        }
        queue_.push_back({std::move(record), wall_ms});
        ++queued_total_;
    }
    wake_.notify_one();
    return true;
}

void InteractionWriter::drain() {
    std::unique_lock lock(mutex_);
    const auto target = queued_total_;
    drained_.wait(lock, [this, target] { return written_total_ >= target || stopping_; });
}

quint64 InteractionWriter::dropped() const {
    const std::lock_guard lock(mutex_);
    return dropped_;
}

quint64 InteractionWriter::failures() const {
    const std::lock_guard lock(mutex_);
    return failures_;
}

void InteractionWriter::run() {
    std::deque<Item> batch;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_)
                return;
            batch.swap(queue_);
        }
        const auto count = batch.size();
        const bool written = writeBatch(batch);
        batch.clear();
        {
            const std::lock_guard lock(mutex_);
            written_total_ += count;
            if (!written)
                ++failures_;
        }
        drained_.notify_all();
    }
}

// The file is opened (and made private) on first use and after a rotation.
bool InteractionWriter::openFile() {
    if (fd_ >= 0)
        return true;
    if (!private_folder(QFileInfo(path_).absolutePath()))
        return false;
    const QByteArray native = QFile::encodeName(path_);
    fd_ = ::open(native.constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, kPrivateFile);
    if (fd_ < 0)
        return false;
    struct stat info{};
    if (::fchmod(fd_, kPrivateFile) != 0 || ::fstat(fd_, &info) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    size_ = info.st_size;
    return true;
}

// interaction.jsonl becomes .1, .1 becomes .2, and the oldest beyond the
// kept count is removed. With one file kept the current one starts over.
bool InteractionWriter::rotate() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    const auto numbered = [this](int index) {
        return path_ + QLatin1Char('.') + QString::number(index);
    };
    if (max_files_ <= 1)
        return QFile::remove(path_) || !QFileInfo::exists(path_);
    QFile::remove(numbered(max_files_ - 1));
    for (int index = max_files_ - 2; index >= 1; --index)
        if (QFileInfo::exists(numbered(index)) &&
            !QFile::rename(numbered(index), numbered(index + 1)))
            return false;
    return QFile::rename(path_, numbered(1));
}

bool InteractionWriter::writeBatch(std::deque<Item>& batch) {
    QByteArray pending;
    bool ok = true;
    for (auto& item : batch) {
        item.record.insert(QStringLiteral("wall"), wall_time(item.wall_ms));
        const QByteArray line = QJsonDocument(item.record).toJson(QJsonDocument::Compact) + '\n';
        if (!openFile()) {
            ok = false;
            break;
        }
        if (size_ + pending.size() + line.size() > max_file_bytes_ && size_ + pending.size() > 0) {
            ok = write_all(fd_, pending) && ok;
            pending.clear();
            if (!rotate() || !openFile()) {
                ok = false;
                break;
            }
        }
        pending += line;
    }
    if (ok && !pending.isEmpty() && fd_ >= 0) {
        ok = write_all(fd_, pending);
        if (ok)
            size_ += pending.size();
    }
    if (!ok) {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        bool warn = false;
        {
            const std::lock_guard lock(mutex_);
            warn = !warned_;
            warned_ = true;
        }
        if (warn)
            qWarning().noquote() << "Interaction log: cannot write" << path_
                                 << "(the folder must be private to you); records are dropped";
    }
    return ok;
}

} // namespace lapis::desktop
