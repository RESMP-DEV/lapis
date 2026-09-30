#include "chime_sounds.hpp"
#include "keymap.hpp"
#include "platform/chime_file.hpp"
#include "platform/published_task.hpp"
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace lapis::desktop {
namespace {
constexpr int kRate = 44100;
constexpr double kTwoPi = 6.283185307179586;
constexpr double kLength = 0.62;

struct Tap {
    double frequency;
    double start;
    double gain;
};

// Each tap: a sine with a soft attack and a long decay, an octave shimmer and
// a brief inharmonic strike, which together read as glass.
std::vector<double> synthesize(const std::array<Tap, 2>& taps, double peak_db) {
    std::vector<double> out(static_cast<std::size_t>(kLength * kRate), 0.0);
    for (const auto& tap : taps) {
        const auto offset = static_cast<std::size_t>(tap.start * kRate);
        for (std::size_t i = offset; i < out.size(); ++i) {
            const double t = static_cast<double>(i - offset) / kRate;
            const double attack = std::min(t / 0.004, 1.0);
            const double body = std::sin(kTwoPi * tap.frequency * t) * std::exp(-t / 0.11);
            const double shimmer =
                0.22 * std::sin(kTwoPi * tap.frequency * 2.0 * t) * std::exp(-t / 0.05);
            const double strike =
                0.10 * std::sin(kTwoPi * tap.frequency * 3.98 * t) * std::exp(-t / 0.018);
            out[i] += tap.gain * attack * (body + shimmer + strike);
        }
    }
    const auto fade = static_cast<std::size_t>(0.02 * kRate);
    for (std::size_t i = 0; i < fade; ++i)
        out[out.size() - fade + i] *= 1.0 - static_cast<double>(i) / static_cast<double>(fade - 1);
    double peak = 0.0;
    for (const double sample : out)
        peak = std::max(peak, std::abs(sample));
    const double scale = std::pow(10.0, peak_db / 20.0) / peak;
    for (double& sample : out)
        sample *= scale;
    return out;
}

QByteArray wav(const std::vector<double>& samples) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const auto data = static_cast<quint32>(samples.size() * 2);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + data);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(kRate) << quint32(kRate * 2)
        << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << data;
    for (const double sample : samples)
        out << static_cast<qint16>(std::lround(std::clamp(sample, -1.0, 1.0) * 32767.0));
    return bytes;
}
} // namespace

QByteArray chime_wav(Chime chime) {
    constexpr double e6 = 1318.51;
    constexpr double a6 = 1760.0;
    if (chime == Chime::needsYou)
        return wav(synthesize({Tap{e6, 0.0, 1.0}, Tap{a6, 0.13, 1.0}}, -12.0));
    return wav(synthesize({Tap{a6, 0.0, 0.9}, Tap{e6, 0.13, 1.0}}, -18.0));
}

struct ChimeSounds::State {
    using Clock = std::chrono::steady_clock;
    struct File {
        platform::ChimeFile contents;
        QString cacheKey, error, playbackError;
        Clock::time_point nextCheck{};
        quint64 ticket{};
        bool loading{};
    };
    std::mutex mutex;
    QHash<QString, File> files;
    QStringList paths;
    QPointer<KeyMap> owner;
    QObject* dispatcher{}; // ChimeSounds owns it until active is cleared under the mutex.
    bool active{true};
    QString outputError;
    int jobs{};
    quint64 generation{}, ticket{};

    static File load(const QString& path, const File& previous) {
        File result;
        result.contents =
            platform::read_chime_file(path, previous.contents, ChimeSounds::kMaxFileBytes);
        if (!result.contents.error.isEmpty())
            result.error = QStringLiteral("Sound '%1': %2; using built-in chime")
                               .arg(path, result.contents.error);
        else
            result.cacheKey =
                path + QLatin1Char('\n') + QString::fromLatin1(result.contents.stamp.toHex());
        return result;
    }
};

ChimeSounds::ChimeSounds() : state_(std::make_shared<State>()) {
    state_->dispatcher = &dispatcher_;
}
ChimeSounds::~ChimeSounds() {
    const std::lock_guard lock(state_->mutex);
    state_->active = false;
    state_->files.clear();
}
void ChimeSounds::reportLocked(const std::shared_ptr<State>& state) {
    if (!state->active)
        return;
    QStringList messages;
    for (const auto& path : state->paths) {
        const auto& file = state->files[path];
        if (!file.error.isEmpty())
            messages << file.error;
        if (!file.playbackError.isEmpty())
            messages << file.playbackError;
    }
    if (!state->outputError.isEmpty())
        messages << state->outputError;
    const auto text = messages.join(QLatin1Char('\n'));
    const auto generation = state->generation;
    // The mutex keeps dispatcher alive throughout enqueue; destruction marks
    // active=false before the QObject member can disappear.
    QMetaObject::invokeMethod(state->dispatcher, platform::PublishedTask([state, text, generation] {
                                  QPointer<KeyMap> owner;
                                  {
                                      const std::lock_guard lock(state->mutex);
                                      if (!state->active || state->generation != generation)
                                          return;
                                      owner = state->owner;
                                  }
                                  if (owner)
                                      owner->setChimeDiagnostic(text);
                              }),
                              Qt::QueuedConnection);
}
void ChimeSounds::configure(KeyMap& config) {
    QStringList paths;
    for (const auto& path : {config.alertSoundFile(), config.finishSoundFile()})
        if (!path.isEmpty() && !paths.contains(path))
            paths << path;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->owner != &config || state_->paths != paths) {
            state_->owner = &config;
            state_->paths = paths;
            ++state_->generation;
            for (const auto& key : state_->files.keys())
                if (!paths.contains(key))
                    state_->files.remove(key);
            for (const auto& path : paths)
                if (!state_->files.contains(path))
                    state_->files.insert(path, {});
            reportLocked(state_);
        }
    }
    pump(state_);
}
void ChimeSounds::pump(const std::shared_ptr<State>& state) {
    struct Job {
        QString path;
        State::File previous;
        quint64 ticket{};
    };
    std::vector<Job> jobs;
    {
        const std::lock_guard lock(state->mutex);
        if (!state->active)
            return;
        for (const auto& path : state->paths) {
            if (state->jobs >= 2)
                break;
            auto& file = state->files[path];
            if (file.loading || State::Clock::now() < file.nextCheck)
                continue;
            file.loading = true;
            file.ticket = ++state->ticket;
            ++state->jobs;
            jobs.push_back({path, file, file.ticket});
        }
    }
    for (const auto& job : jobs)
        QThreadPool::globalInstance()->start(platform::PublishedTask([state, job] {
            auto loaded = State::load(job.path, job.previous);
            {
                const std::lock_guard lock(state->mutex);
                --state->jobs;
                auto file = state->files.find(job.path);
                if (state->active && file != state->files.end() && file->ticket == job.ticket) {
                    // Playback may have completed while the stat/read was in
                    // flight. Preserve its newer status for the same bytes.
                    if (loaded.cacheKey == file->cacheKey)
                        loaded.playbackError = file->playbackError;
                    loaded.ticket = job.ticket;
                    loaded.loading = false;
                    loaded.nextCheck = State::Clock::now() + std::chrono::milliseconds(100);
                    *file = std::move(loaded);
                    reportLocked(state);
                }
            }
            // A configuration change may have replaced paths while both slots
            // were occupied. Load the newest selection when a slot becomes free.
            pump(state);
        }));
}
ChimeSound ChimeSounds::sound(Chime chime, KeyMap& config) {
    configure(config);
    const bool finished = chime == Chime::finished;
    const QString& own = finished ? config.finishSoundFile() : config.alertSoundFile();
    const bool borrowed = finished && own.isEmpty();
    const QString& path = borrowed ? config.alertSoundFile() : own;
    {
        const std::lock_guard lock(state_->mutex);
        const auto file = state_->files.constFind(path);
        if (file != state_->files.cend() && !file->contents.bytes.isEmpty())
            return {file->contents.bytes, borrowed ? 0.5F : 1.0F, file->cacheKey, path};
    }
    static const QByteArray needs = chime_wav(Chime::needsYou);
    static const QByteArray ended = chime_wav(Chime::finished);
    return {finished ? ended : needs, 1.0F, {}, {}};
}
void ChimeSounds::play(Chime chime, KeyMap& config, const Player& player) {
    const auto selected = sound(chime, config);
    const bool played = player(selected);
    if (!selected.path.isEmpty()) {
        const std::lock_guard lock(state_->mutex);
        auto file = state_->files.find(selected.path);
        if (file != state_->files.end() && file->cacheKey == selected.cacheKey) {
            const auto message =
                played ? QString()
                       : QStringLiteral("Sound '%1' could not be played; trying built-in chime")
                             .arg(selected.path);
            if (file->playbackError != message) {
                file->playbackError = message;
                reportLocked(state_);
            }
        }
    }
    const bool output_ok =
        played || (!selected.path.isEmpty() && player({chime_wav(chime), 1.0F, {}, {}}));
    {
        const std::lock_guard lock(state_->mutex);
        const auto message =
            output_ok ? QString() : QStringLiteral("Chime playback is unavailable");
        if (state_->outputError != message) {
            state_->outputError = message;
            reportLocked(state_);
        }
    }
}

} // namespace lapis::desktop
