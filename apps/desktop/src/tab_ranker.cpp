#include "tab_ranker.hpp"

#include "platform/published_task.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr int kLogVersion = 1;
constexpr qint64 kLogLimit = qint64{4} * 1024 * 1024;
constexpr qint64 kRecordLimit = qint64{256} * 1024;
constexpr int kMaxCandidates = 64;
// Fitting: gradient steps on the mean negative log-likelihood of the person's
// choices plus `kPull` times the squared distance from the prior (category
// weights are pulled toward zero). Measured on the author's week of choices
// (164 decisions, see docs/architecture.md), 0.1 kept most of the prior's
// shape while still following the person.
constexpr double kPull = 0.1;
constexpr double kStep = 0.3;
constexpr int kIterations = 150;
constexpr double kStaleMinutes = 120.0;
constexpr double kSinceCapMinutes = 3.0 * 24 * 60;

QByteArray line(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QJsonArray toArray(const std::array<double, TabRanker::kFeatures>& values) {
    QJsonArray array;
    for (const double value : values)
        array.append(std::round(value * 1e4) / 1e4);
    return array;
}

// The logged candidates of a decision, validated: features of the right width
// and finite, a chosen index in range.
struct Logged {
    std::vector<std::array<double, TabRanker::kFeatures>> x;
    QStringList categories;
    std::size_t chosen{};
};
std::optional<Logged> logged(const QJsonObject& event) {
    const auto candidates = event.value(QStringLiteral("candidates")).toArray();
    const auto chosen = event.value(QStringLiteral("chosen")).toInt(-1);
    if (candidates.size() < 2 || candidates.size() > kMaxCandidates || chosen < 0 ||
        chosen >= candidates.size())
        return std::nullopt;
    Logged out;
    out.chosen = static_cast<std::size_t>(chosen);
    for (const auto& value : candidates) {
        const auto candidate = value.toObject();
        const auto x = candidate.value(QStringLiteral("x")).toArray();
        if (x.size() != static_cast<qsizetype>(TabRanker::kFeatures))
            return std::nullopt;
        std::array<double, TabRanker::kFeatures> row{};
        for (std::size_t i = 0; i < row.size(); ++i) {
            const auto item = x.at(static_cast<qsizetype>(i));
            if (!item.isDouble() || !std::isfinite(item.toDouble()))
                return std::nullopt;
            row[i] = item.toDouble();
        }
        out.x.push_back(row);
        out.categories.append(candidate.value(QStringLiteral("category")).toString().left(64));
    }
    return out;
}

std::vector<QJsonObject> readDecisions(const QString& path) {
    std::vector<QJsonObject> decisions;
    for (const auto& file : {path + QStringLiteral(".1"), path}) {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly))
            continue;
        while (!in.atEnd()) {
            const auto bytes = in.readLine(kRecordLimit);
            const auto object = QJsonDocument::fromJson(bytes).object();
            if (object.value(QStringLiteral("event")).toString() == QLatin1String("choice"))
                decisions.push_back(object);
        }
    }
    if (decisions.size() > static_cast<std::size_t>(TabRanker::kMaxDecisions))
        decisions.erase(decisions.begin(),
                        decisions.end() - static_cast<std::ptrdiff_t>(TabRanker::kMaxDecisions));
    return decisions;
}

QJsonObject modelObject(const TabRanker::Model& model) {
    QJsonObject categories;
    for (auto it = model.categories.cbegin(); it != model.categories.cend(); ++it)
        categories.insert(it.key(), std::round(it.value() * 1e4) / 1e4);
    return {{QStringLiteral("v"), kLogVersion},
            {QStringLiteral("decisions"), model.decisions},
            {QStringLiteral("weights"), toArray(model.weights)},
            {QStringLiteral("categories"), categories}};
}

std::optional<TabRanker::Model> readModel(const QString& path) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly) || in.size() > kRecordLimit)
        return std::nullopt;
    const auto object = QJsonDocument::fromJson(in.readAll()).object();
    const auto weights = object.value(QStringLiteral("weights")).toArray();
    if (object.value(QStringLiteral("v")).toInt() != kLogVersion ||
        weights.size() != static_cast<qsizetype>(TabRanker::kFeatures))
        return std::nullopt;
    TabRanker::Model model;
    for (std::size_t i = 0; i < model.weights.size(); ++i) {
        const auto value = weights.at(static_cast<qsizetype>(i));
        if (!value.isDouble() || !std::isfinite(value.toDouble()))
            return std::nullopt;
        model.weights[i] = value.toDouble();
    }
    const auto categories = object.value(QStringLiteral("categories")).toObject();
    for (auto it = categories.begin(); it != categories.end(); ++it)
        if (it.value().isDouble() && std::isfinite(it.value().toDouble()))
            model.categories.insert(it.key().left(64), it.value().toDouble());
    model.decisions = object.value(QStringLiteral("decisions")).toInt();
    return model;
}

void writeModel(const QString& path, const TabRanker::Model& model) {
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning() << "Tab ranker: cannot write the model:" << out.errorString();
        return;
    }
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    out.write(QJsonDocument(modelObject(model)).toJson(QJsonDocument::Compact));
    if (!out.commit())
        qWarning() << "Tab ranker: cannot write the model:" << out.errorString();
}
} // namespace

TabAwaySettings parse_tab_away(const QJsonValue& value) {
    TabAwaySettings settings;
    const auto object = value.toObject();
    if (object.value(QStringLiteral("rank")).toString() == QLatin1String("fixed"))
        settings.learned = false;
    if (const auto work = object.value(QStringLiteral("work")); work.isArray()) {
        settings.work.clear();
        for (const auto& name : work.toArray())
            if (name.isString() && !name.toString().trimmed().isEmpty() &&
                settings.work.size() < 32)
                settings.work.append(name.toString().trimmed().left(80));
    }
    return settings;
}

// Work agents first, then requests, then turns not yet seen, then the newest,
// as weights rather than strict tiers: an hour more of waiting outweighs about
// one step. In the author's choices the agent they went to had usually just
// finished; oldest-first never matched their first choice on the held-out
// days (see docs/architecture.md).
TabRanker::Weights TabRanker::prior() { return {3.2, 3.0, 2.5, 0.3, -0.5, 0.5, -1.0, 0.3, -0.2}; }

std::vector<std::array<double, TabRanker::kFeatures>>
TabRanker::features(const std::vector<TabCandidate>& candidates) {
    double newest = std::numeric_limits<double>::infinity();
    for (const auto& candidate : candidates)
        newest = std::min(newest, std::max(0.0, candidate.waitMinutes));
    std::vector<std::array<double, kFeatures>> rows;
    rows.reserve(candidates.size());
    for (const auto& c : candidates) {
        const double wait = std::max(0.0, c.waitMinutes);
        rows.push_back({c.work ? 1.0 : 0.0, c.request ? 1.0 : 0.0, c.unseen ? 1.0 : 0.0,
                        c.guess ? 1.0 : 0.0, std::log1p(wait), wait == newest ? 1.0 : 0.0,
                        wait > kStaleMinutes ? 1.0 : 0.0,
                        std::log1p(static_cast<double>(std::max(0, c.turnsLastHour))),
                        std::log1p(std::clamp(c.sinceTurnMinutes, 0.0, kSinceCapMinutes))});
    }
    return rows;
}

namespace {
// One decision's share of the gradient of the mean negative log-likelihood.
void accumulate(const TabRanker::Model& model, const Logged& decision, double n,
                TabRanker::Weights& gradient, QHash<QString, double>& category_gradient) {
    const auto count = decision.x.size();
    std::vector<double> scores(count, 0.0);
    for (std::size_t c = 0; c < count; ++c) {
        double s = model.categories.value(decision.categories.at(static_cast<qsizetype>(c)));
        for (std::size_t f = 0; f < TabRanker::kFeatures; ++f)
            s += model.weights[f] * decision.x[c][f];
        scores[c] = s;
    }
    const double top = *std::max_element(scores.begin(), scores.end());
    double sum = 0.0;
    for (auto& s : scores)
        sum += s = std::exp(s - top);
    for (std::size_t c = 0; c < count; ++c) {
        const double weight = scores[c] / sum - (c == decision.chosen ? 1.0 : 0.0);
        for (std::size_t f = 0; f < TabRanker::kFeatures; ++f)
            gradient[f] += weight * decision.x[c][f] / n;
        category_gradient[decision.categories.at(static_cast<qsizetype>(c))] += weight / n;
    }
}
} // namespace

TabRanker::Model fit_tab_ranker(const std::vector<QJsonObject>& decisions) {
    std::vector<Logged> data;
    for (const auto& event : decisions)
        if (auto one = logged(event))
            data.push_back(std::move(*one));
    TabRanker::Model model;
    model.weights = TabRanker::prior();
    model.decisions = static_cast<int>(data.size());
    if (data.empty())
        return model;
    const auto prior = TabRanker::prior();
    const double n = static_cast<double>(data.size());
    for (int step = 0; step < kIterations; ++step) {
        TabRanker::Weights gradient{};
        for (std::size_t i = 0; i < gradient.size(); ++i)
            gradient[i] = kPull * (model.weights[i] - prior[i]);
        QHash<QString, double> category_gradient;
        for (auto it = model.categories.cbegin(); it != model.categories.cend(); ++it)
            category_gradient.insert(it.key(), kPull * it.value());
        for (const auto& decision : data)
            accumulate(model, decision, n, gradient, category_gradient);
        for (std::size_t f = 0; f < TabRanker::kFeatures; ++f)
            model.weights[f] -= kStep * gradient[f];
        for (auto it = category_gradient.cbegin(); it != category_gradient.cend(); ++it)
            model.categories[it.key()] -= kStep * it.value();
    }
    return model;
}

// Worker handoff: the owner pointer is read and calls are posted only while
// the ranker is alive (the destructor clears `active` under the mutex).
struct TabRanker::Shared {
    std::mutex mutex;
    bool active{true};
    bool running{};
    bool again{};
    TabRanker* owner{};
};

TabRanker::TabRanker(QString folder, QObject* parent)
    : QObject(parent), folder_(std::move(folder)), shared_(std::make_shared<Shared>()) {
    shared_->owner = this;
    model_.weights = prior();
    if (!folder_.isEmpty()) {
        log_path_ = QDir(folder_).filePath(QStringLiteral("tab_away.jsonl"));
        model_path_ = QDir(folder_).filePath(QStringLiteral("tab_away_model.json"));
        if (auto saved = readModel(model_path_))
            model_ = std::move(*saved);
        // The log may hold choices the saved model has not seen.
        if (QFileInfo::exists(log_path_))
            refit();
    }
}

TabRanker::~TabRanker() {
    const std::lock_guard lock(shared_->mutex);
    shared_->active = false;
    shared_->owner = nullptr;
}

std::vector<double> TabRanker::scores(const std::vector<TabCandidate>& candidates) const {
    std::vector<double> out;
    out.reserve(candidates.size());
    const auto rows = features(candidates);
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        if (!learned_) {
            // The fixed order: a guess not yet seen, then an unseen turn or a
            // request, then a guess already seen; longest waiting within each.
            const auto& item = candidates[c];
            const int tier = item.guess && !item.guessSeen ? 0
                             : item.unseen || item.request ? 1
                                                           : 2;
            out.push_back(-1e6 * tier + item.waitMinutes);
            continue;
        }
        double s = model_.categories.value(candidates[c].category);
        for (std::size_t f = 0; f < kFeatures; ++f)
            s += model_.weights[f] * rows[c][f];
        out.push_back(s);
    }
    return out;
}

std::optional<std::size_t> TabRanker::pick(const std::vector<TabCandidate>& candidates,
                                           std::vector<double>* scores_out) const {
    const auto s = scores(candidates);
    std::optional<std::size_t> best;
    for (std::size_t c = 0; c < candidates.size(); ++c)
        if (!best || s[c] > s[*best] ||
            (s[c] == s[*best] && candidates[c].waitMinutes > candidates[*best].waitMinutes))
            best = c;
    if (scores_out)
        *scores_out = s;
    return best;
}

namespace {
QJsonArray loggedCandidates(const std::vector<TabCandidate>& candidates,
                            const std::vector<double>* scores) {
    QJsonArray out;
    const auto rows = TabRanker::features(candidates);
    for (std::size_t c = 0; c < candidates.size() && c < kMaxCandidates; ++c) {
        QJsonObject item{{QStringLiteral("agent"), candidates[c].session},
                         {QStringLiteral("category"), candidates[c].category},
                         {QStringLiteral("x"), toArray(rows[c])}};
        if (candidates[c].guessSeen)
            item.insert(QStringLiteral("guess_seen"), true);
        if (scores)
            item.insert(QStringLiteral("score"), std::round((*scores)[c] * 1e4) / 1e4);
        out.append(item);
    }
    return out;
}
} // namespace

void TabRanker::recordTab(const std::vector<TabCandidate>& candidates,
                          const std::vector<double>& scores, std::size_t picked) {
    record(
        {{QStringLiteral("event"), QStringLiteral("tab")},
         {QStringLiteral("rank"), learned_ ? QStringLiteral("learned") : QStringLiteral("fixed")},
         {QStringLiteral("picked"), static_cast<int>(picked)},
         {QStringLiteral("candidates"), loggedCandidates(candidates, &scores)}});
}

void TabRanker::learn(const std::vector<TabCandidate>& candidates, std::size_t chosen,
                      bool viaTab) {
    if (candidates.size() < 2 || candidates.size() > kMaxCandidates || chosen >= candidates.size())
        return;
    QJsonObject event{
        {QStringLiteral("event"), QStringLiteral("choice")},
        {QStringLiteral("via"), viaTab ? QStringLiteral("tab") : QStringLiteral("person")},
        {QStringLiteral("chosen"), static_cast<int>(chosen)},
        {QStringLiteral("candidates"), loggedCandidates(candidates, nullptr)}};
    if (log_path_.isEmpty()) {
        memory_.push_back(event);
        if (memory_.size() > static_cast<std::size_t>(kMaxDecisions))
            memory_.erase(memory_.begin());
    }
    record(event);
    refit();
}

void TabRanker::record(QJsonObject event) const {
    if (log_path_.isEmpty())
        return;
    event.insert(QStringLiteral("v"), kLogVersion);
    event.insert(QStringLiteral("t"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const auto encoded = line(event);
    if (encoded.size() > kRecordLimit)
        return;
    QFile file(log_path_);
    if (file.exists() && file.size() + encoded.size() > kLogLimit) {
        const auto previous = log_path_ + QStringLiteral(".1");
        QFile::remove(previous);
        if (!file.rename(previous)) {
            qWarning() << "Tab ranker: cannot rotate the log";
            return;
        }
        file.setFileName(log_path_);
    }
    QDir().mkpath(folder_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly,
                   QFile::ReadOwner | QFile::WriteOwner) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        qWarning() << "Tab ranker: cannot write the log:" << file.errorString();
        return;
    }
    if (file.write(encoded) != encoded.size() || !file.flush())
        qWarning() << "Tab ranker: cannot append the log:" << file.errorString();
}

bool TabRanker::refitting() const {
    const std::lock_guard lock(shared_->mutex);
    return shared_->running || shared_->again;
}

// One refit at a time; a choice made during one queues exactly one more.
void TabRanker::refit() {
    {
        const std::lock_guard lock(shared_->mutex);
        if (shared_->running) {
            shared_->again = true;
            return;
        }
        shared_->running = true;
    }
    QThreadPool::globalInstance()->start(platform::PublishedTask(
        [shared = shared_, path = log_path_, model_path = model_path_, memory = memory_] {
            auto model = fit_tab_ranker(path.isEmpty() ? memory : readDecisions(path));
            if (!model_path.isEmpty() && model.decisions > 0)
                writeModel(model_path, model);
            const std::lock_guard lock(shared->mutex);
            if (!shared->active)
                return;
            QMetaObject::invokeMethod(shared->owner,
                                      platform::PublishedTask([shared, model = std::move(model)] {
                                          TabRanker* owner = nullptr;
                                          bool again = false;
                                          {
                                              const std::lock_guard inner(shared->mutex);
                                              if (!shared->active)
                                                  return;
                                              owner = shared->owner;
                                              shared->running = false;
                                              again = std::exchange(shared->again, false);
                                          }
                                          owner->adopt(model);
                                          if (again)
                                              owner->refit();
                                      }),
                                      Qt::QueuedConnection);
        }));
}

void TabRanker::adopt(Model model) {
    model_ = std::move(model);
    emit modelChanged();
}

} // namespace lapis::desktop
