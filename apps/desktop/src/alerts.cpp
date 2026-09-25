#include "alerts.hpp"

#include "keymap.hpp"
#include "workspace.hpp"
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <array>
#include <cmath>
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

Alerts::Alerts(Workspace& workspace, const KeyMap& config, Player play, Looking looking,
               QObject* parent)
    : QObject(parent), config_(config), play_(std::move(play)), looking_(std::move(looking)) {
    repeat_.setInterval(kRepeatMs);
    connect(&repeat_, &QTimer::timeout, this, &Alerts::tick);
    connect(&workspace, &Workspace::agentNeedsYou, this, &Alerts::needsYou);
    connect(&workspace, &Workspace::turnFinished, this, &Alerts::finished);
}

void Alerts::preview(bool needsYou) {
    play_(needsYou ? Chime::needsYou : Chime::finished);
    last_.start();
}

bool Alerts::ring(Chime chime) {
    if (last_.isValid() && last_.elapsed() < quiet_ms_)
        return false;
    play_(chime);
    last_.start();
    return true;
}

void Alerts::needsYou(SessionPreview* item) {
    if (!config_.alertSound() || looking_(item))
        return;
    const auto found =
        std::find_if(waiting_.begin(), waiting_.end(),
                     [item](const Waiting& waiting) { return waiting.item == item; });
    if (found == waiting_.end())
        waiting_.push_back({item, 0});
    else
        found->played = 0; // a new request starts its count again
    if (ring(Chime::needsYou))
        for (auto& waiting : waiting_)
            ++waiting.played;
    if (!repeat_.isActive())
        repeat_.start();
}

void Alerts::tick() {
    // Answered, looked at, closed, or chimed for enough: done.
    std::erase_if(waiting_, [this](const Waiting& waiting) {
        return !waiting.item || waiting.item->attentionCount() == 0 || looking_(waiting.item) ||
               waiting.played >= config_.alertRepeat();
    });
    if (waiting_.empty() || !config_.alertSound()) {
        waiting_.clear();
        repeat_.stop();
        return;
    }
    // One chime covers every agent still waiting.
    if (ring(Chime::needsYou))
        for (auto& waiting : waiting_)
            ++waiting.played;
}

void Alerts::finished(SessionPreview* item) {
    if (config_.finishSound() && !looking_(item))
        ring(Chime::finished);
}

Notifier::Notifier(Workspace& workspace, const KeyMap& config, Post post, Background background,
                   QObject* parent)
    : QObject(parent), config_(config), post_(std::move(post)), background_(std::move(background)) {
    connect(&workspace, &Workspace::agentNeedsYou, this,
            [this](SessionPreview* item) { notify(item, true); });
    connect(&workspace, &Workspace::turnFinished, this,
            [this](SessionPreview* item) { notify(item, false); });
}

void Notifier::notify(const SessionPreview* item, bool needsYou) {
    if (item == nullptr || !config_.notify() || !background_())
        return;
    QString body = needsYou ? tr("Needs you") : tr("Finished a turn");
    if (needsYou && !item->attentionReason().isEmpty())
        body += QStringLiteral(": ") + item->attentionReason();
    post_(item->sessionId(), item->title(), body);
}
} // namespace lapis::desktop
