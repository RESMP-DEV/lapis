#include "harness_models.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <optional>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr qsizetype kLongestAnswer = qsizetype{4} << 20;
constexpr int kNewestSessions = 40;
constexpr qint64 kSessionHead = qint64{64} << 10;

// The CLI's default first, each model once, at most the number shown.
std::vector<ModelChoice> tidy(std::vector<ModelChoice> models) {
    std::stable_partition(models.begin(), models.end(),
                          [](const ModelChoice& model) { return model.isDefault; });
    std::vector<ModelChoice> unique;
    for (auto& model : models) {
        const bool seen = std::any_of(unique.begin(), unique.end(), [&](const ModelChoice& other) {
            return other.id == model.id;
        });
        if (!model.id.isEmpty() && !seen &&
            static_cast<int>(unique.size()) < HarnessModels::kMostShown)
            unique.push_back(std::move(model));
    }
    return unique;
}

// "provider/model" shown as the model, unless two providers offer one name.
std::vector<ModelChoice> named_after_model(std::vector<ModelChoice> models) {
    for (auto& model : models)
        model.name = model.id.section(QLatin1Char('/'), 1, -1);
    for (auto& model : models) {
        const auto same =
            std::count_if(models.begin(), models.end(),
                          [&](const ModelChoice& other) { return other.name == model.name; });
        if (model.name.isEmpty() || same > 1)
            model.name = model.id;
    }
    return models;
}

QString read_text(const QString& path, qint64 longest) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(longest)) : QString();
}
} // namespace

// {"data": [{"model": "gpt-6-astra", "displayName": "GPT-6-Astra",
// "isDefault": true, "hidden": false}]}
std::vector<ModelChoice> codex_models(const QJsonObject& result) {
    std::vector<ModelChoice> models;
    for (const auto& item : result.value(QStringLiteral("data")).toArray()) {
        const auto model = item.toObject();
        if (model.value(QStringLiteral("hidden")).toBool())
            continue;
        const auto id = model.value(QStringLiteral("model"))
                            .toString(model.value(QStringLiteral("id")).toString());
        models.push_back({.id = id,
                          .name = model.value(QStringLiteral("displayName")).toString(id),
                          .isDefault = model.value(QStringLiteral("isDefault")).toBool()});
    }
    return tidy(std::move(models));
}

// {"models": [{"value": "default", "displayName": "Default (recommended)",
// "description": "Opus 5.5 · Best for everyday, complex tasks"}, {"value":
// "opus", "displayName": "Opus 5.5"}, ...]}: the default is the model its
// description names.
std::vector<ModelChoice> claude_models(const QJsonObject& initialized) {
    std::vector<ModelChoice> models;
    QString default_name;
    for (const auto& item : initialized.value(QStringLiteral("models")).toArray()) {
        const auto model = item.toObject();
        const auto id = model.value(QStringLiteral("value")).toString();
        const auto name = model.value(QStringLiteral("displayName")).toString(id);
        if (id == QLatin1String("default")) {
            default_name = model.value(QStringLiteral("description"))
                               .toString()
                               .section(QStringLiteral(" · "), 0, 0)
                               .trimmed();
            continue;
        }
        models.push_back({.id = id, .name = name, .isDefault = false});
    }
    const auto named = std::find_if(models.begin(), models.end(), [&](const ModelChoice& model) {
        return !default_name.isEmpty() && model.name == default_name;
    });
    if (named != models.end())
        named->isDefault = true;
    else if (!default_name.isEmpty())
        models.insert(models.begin(),
                      {.id = QStringLiteral("default"), .name = default_name, .isDefault = true});
    return tidy(std::move(models));
}

// "Available models:\n  * grok-4.7-build-fast (default)\n  - grok-4.5"
std::vector<ModelChoice> grok_models(const QString& listing) {
    static const QRegularExpression line(
        QStringLiteral(R"(^\s*([*-])\s+(\S+)(\s+\(default\))?\s*$)"));
    std::vector<ModelChoice> models;
    for (const auto& text : listing.split(QLatin1Char('\n'))) {
        const auto match = line.match(text);
        if (match.hasMatch())
            models.push_back({.id = match.captured(2),
                              .name = match.captured(2),
                              .isDefault = match.captured(1) == QLatin1String("*") ||
                                           !match.captured(3).isEmpty()});
    }
    return tidy(std::move(models));
}

// "gemini-3.8-flash-high\tGemini 3.8 Flash (High)", one per line.
std::vector<ModelChoice> agy_models(const QString& listing) {
    std::vector<ModelChoice> models;
    for (const auto& text : listing.split(QLatin1Char('\n'))) {
        const auto parts = text.split(QLatin1Char('\t'));
        if (parts.size() >= 2 && !parts.front().trimmed().isEmpty())
            models.push_back(
                {.id = parts.front().trimmed(), .name = parts.at(1).trimmed(), .isDefault = false});
    }
    return tidy(std::move(models));
}

// default_model = "kimi-code/k3" and [models."kimi-code/k3"] tables; only
// these names are read.
std::vector<ModelChoice> kimi_models(const QString& config) {
    static const QRegularExpression chosen(
        QStringLiteral(R"re(^\s*default_model\s*=\s*"([^"]+)")re"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression table(QStringLiteral(R"re(^\s*\[models\."([^"]+)"\]\s*$)re"),
                                          QRegularExpression::MultilineOption);
    const auto default_id = chosen.match(config).captured(1);
    std::vector<ModelChoice> models;
    for (auto it = table.globalMatch(config); it.hasNext();) {
        const auto id = it.next().captured(1);
        models.push_back({.id = id, .name = id, .isDefault = id == default_id});
    }
    return named_after_model(tidy(std::move(models)));
}

// {"recent": [{"providerID": "p", "modelID": "m"}], "favorite": [...]}
std::vector<ModelChoice> opencode_models(const QJsonObject& state) {
    std::vector<ModelChoice> models;
    for (const auto* list : {"favorite", "recent"})
        for (const auto& item : state.value(QLatin1String(list)).toArray()) {
            const auto model = item.toObject();
            const auto provider = model.value(QStringLiteral("providerID")).toString();
            const auto id = model.value(QStringLiteral("modelID")).toString();
            if (!provider.isEmpty() && !id.isEmpty())
                models.push_back(
                    {.id = provider + QLatin1Char('/') + id, .name = {}, .isDefault = false});
        }
    return named_after_model(tidy(std::move(models)));
}

// config.yml's modelRoles ("  default: provider/model"), the default role
// first, then the models the newest sessions used.
std::vector<ModelChoice> omp_models(const QString& config, const QStringList& recent) {
    std::vector<ModelChoice> models;
    bool roles = false;
    static const QRegularExpression role(QStringLiteral(R"(^\s+([A-Za-z0-9_-]+):\s*(\S+)\s*$)"));
    for (const auto& line : config.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1Char(' ')) && !line.trimmed().isEmpty())
            roles = line.trimmed() == QLatin1String("modelRoles:");
        const auto match = role.match(line);
        if (roles && match.hasMatch())
            models.push_back({.id = match.captured(2),
                              .name = {},
                              .isDefault = match.captured(1) == QLatin1String("default")});
    }
    for (const auto& id : recent)
        models.push_back({.id = id, .name = {}, .isDefault = false});
    return named_after_model(tidy(std::move(models)));
}

namespace {
QString home_setting(const char* variable, const QString& fallback) {
    const auto set = qEnvironmentVariable(variable);
    return set.isEmpty() ? fallback : set;
}

std::vector<ModelChoice> read_kimi(const QString& home) {
    return kimi_models(
        read_text(home_setting("KIMI_CODE_HOME", home + QStringLiteral("/.kimi-code")) +
                      QStringLiteral("/config.toml"),
                  qint64{1} << 20));
}

std::vector<ModelChoice> read_opencode(const QString& home) {
    const auto state = home_setting("XDG_STATE_HOME", home + QStringLiteral("/.local/state"));
    return opencode_models(
        QJsonDocument::fromJson(
            read_text(state + QStringLiteral("/opencode/model.json"), qint64{1} << 20).toUtf8())
            .object());
}

// The model each of OMP's newest sessions was last switched to.
QStringList omp_recent(const QString& sessions) {
    std::vector<QFileInfo> files;
    QDirIterator it(sessions, {QStringLiteral("*.jsonl")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        files.push_back(it.fileInfo());
    }
    std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() > b.lastModified();
    });
    QStringList recent;
    for (std::size_t i = 0; i < files.size() && i < kNewestSessions; ++i) {
        QString last;
        for (const auto& line :
             read_text(files[i].filePath(), kSessionHead).split(QLatin1Char('\n')))
            if (line.contains(QStringLiteral("\"type\":\"model_change\"")))
                last = QJsonDocument::fromJson(line.toUtf8())
                           .object()
                           .value(QStringLiteral("model"))
                           .toString(last);
        if (!last.isEmpty() && !recent.contains(last))
            recent << last;
    }
    return recent;
}

std::vector<ModelChoice> read_omp(const QString& home) {
    const auto agent = home + QStringLiteral("/.omp/agent");
    return omp_models(read_text(agent + QStringLiteral("/config.yml"), qint64{1} << 20),
                      omp_recent(agent + QStringLiteral("/sessions")));
}
} // namespace

HarnessModels::HarnessModels(Programs programs, QString home, QObject* parent)
    : QObject(parent), programs_(std::move(programs)), home_(std::move(home)) {
    pool_.setMaxThreadCount(1);
    connect(&timer_, &QTimer::timeout, this, [this] {
        timer_.setInterval(kRefreshMs);
        refresh();
    });
}

HarnessModels::~HarnessModels() {
    for (auto& asking : asking_)
        if (auto* process = asking.process.data()) {
            process->disconnect(this);
            process->kill();
            process->waitForFinished(1000);
            delete process;
        }
    pool_.waitForDone();
}

void HarnessModels::start() { timer_.start(kFirstMs); }

void HarnessModels::refresh() {
    ask(QStringLiteral("codex"), {QStringLiteral("app-server")},
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"clientInfo":{"name":"lapis","version":"1"}}})"
        "\n");
    // No settings (so none of the person's hooks run) and no transcript.
    ask(QStringLiteral("claude"),
        {QStringLiteral("-p"), QStringLiteral("--input-format"), QStringLiteral("stream-json"),
         QStringLiteral("--output-format"), QStringLiteral("stream-json"),
         QStringLiteral("--verbose"), QStringLiteral("--setting-sources"), QString(),
         QStringLiteral("--no-session-persistence")},
        R"({"type":"control_request","request_id":"models","request":{"subtype":"initialize"}})"
        "\n");
    ask(QStringLiteral("grok"), {QStringLiteral("models")}, {});
    ask(QStringLiteral("agy"), {QStringLiteral("models")}, {});
    readFiles();
}

std::vector<ModelChoice> HarnessModels::models(const QString& harness) const {
    return models_.value(harness);
}

void HarnessModels::set(const QString& harness, std::vector<ModelChoice> models) {
    if (models_.value(harness) == models)
        return;
    if (models.empty())
        models_.remove(harness);
    else
        models_.insert(harness, std::move(models));
    emit changed();
}

void HarnessModels::ask(const QString& harness, const QStringList& arguments,
                        const QByteArray& input) {
    const auto program = programs_ ? programs_(harness) : QString();
    if (program.isEmpty()) {
        set(harness, {});
        return;
    }
    if (std::any_of(asking_.begin(), asking_.end(),
                    [&](const Asking& asking) { return asking.harness == harness; }))
        return;
    auto* process = new QProcess(this);
    process->setProgram(program);
    process->setArguments(arguments);
    process->setWorkingDirectory(QDir::tempPath());
    process->setStandardErrorFile(QProcess::nullDevice());
    asking_.push_back({harness, process, {}});
    connect(process, &QProcess::readyReadStandardOutput, this, [this, harness, process] {
        const auto found = std::find_if(asking_.begin(), asking_.end(), [&](const Asking& asking) {
            return asking.harness == harness;
        });
        if (found == asking_.end())
            return;
        found->output += process->readAllStandardOutput();
        if (found->output.size() > kLongestAnswer)
            process->kill();
        else if (harness == QLatin1String("codex") || harness == QLatin1String("claude"))
            answered(harness);
    });
    connect(process, &QProcess::finished, this, [this, harness, process] {
        const auto found = std::find_if(asking_.begin(), asking_.end(), [&](const Asking& asking) {
            return asking.harness == harness;
        });
        if (found != asking_.end()) {
            const auto output = found->output;
            asking_.erase(found);
            if (harness == QLatin1String("grok"))
                set(harness, grok_models(QString::fromUtf8(output)));
            else if (harness == QLatin1String("agy"))
                set(harness, agy_models(QString::fromUtf8(output)));
        }
        process->deleteLater();
    });
    QTimer::singleShot(kAnswerMs, process, [process] { process->kill(); });
    process->start();
    if (!input.isEmpty())
        process->write(input);
}

// Codex and Claude answer one JSON message per line; once the list arrives
// the CLI is stopped.
void HarnessModels::answered(const QString& harness) {
    const auto found = std::find_if(asking_.begin(), asking_.end(), [&](const Asking& asking) {
        return asking.harness == harness;
    });
    if (found == asking_.end())
        return;
    auto* process = found->process.data();
    for (auto end = found->output.indexOf('\n'); end >= 0; end = found->output.indexOf('\n')) {
        const auto message = QJsonDocument::fromJson(found->output.left(end)).object();
        found->output.remove(0, end + 1);
        std::optional<std::vector<ModelChoice>> models;
        if (harness == QLatin1String("codex")) {
            const auto id = message.value(QStringLiteral("id")).toInt();
            if (id == 1 && process != nullptr)
                process->write(R"({"jsonrpc":"2.0","method":"initialized"})"
                               "\n"
                               R"({"jsonrpc":"2.0","id":2,"method":"model/list","params":{}})"
                               "\n");
            else if (id == 2)
                models = codex_models(message.value(QStringLiteral("result")).toObject());
        } else if (message.value(QStringLiteral("type")).toString() ==
                   QLatin1String("control_response")) {
            models = claude_models(message.value(QStringLiteral("response"))
                                       .toObject()
                                       .value(QStringLiteral("response"))
                                       .toObject());
        }
        if (models) {
            set(harness, std::move(*models));
            if (process != nullptr)
                process->kill(); // finished then removes it
            return;
        }
    }
}

// Kimi, OpenCode and OMP keep their lists in files; read on a worker thread,
// since OMP's newest sessions are found among all of them.
void HarnessModels::readFiles() {
    QStringList installed;
    for (const auto* id : {"kimi", "opencode", "omp"})
        if (programs_ && !programs_(QString::fromLatin1(id)).isEmpty())
            installed << QString::fromLatin1(id);
    for (const auto* id : {"kimi", "opencode", "omp"})
        if (!installed.contains(QString::fromLatin1(id)))
            set(QString::fromLatin1(id), {});
    pool_.start([this, installed, home = home_] {
        QHash<QString, std::vector<ModelChoice>> found;
        if (installed.contains(QStringLiteral("kimi")))
            found.insert(QStringLiteral("kimi"), read_kimi(home));
        if (installed.contains(QStringLiteral("opencode")))
            found.insert(QStringLiteral("opencode"), read_opencode(home));
        if (installed.contains(QStringLiteral("omp")))
            found.insert(QStringLiteral("omp"), read_omp(home));
        QMetaObject::invokeMethod(
            this,
            [this, found = std::move(found)]() mutable {
                for (auto it = found.begin(); it != found.end(); ++it)
                    set(it.key(), std::move(it.value()));
            },
            Qt::QueuedConnection);
    });
}
} // namespace lapis::desktop
