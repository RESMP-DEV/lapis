#include "usage_limits.hpp"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <utility>

namespace lapis::desktop {
namespace {
constexpr qsizetype kLongestAnswer = qsizetype{4} << 20;
constexpr qsizetype kLongestStartup = qsizetype{64} << 10;
constexpr int kWeek = 7 * 24 * 60;

QString window_label(int minutes) {
    if (minutes > 0 && minutes % (24 * 60) == 0) {
        const int days = minutes / (24 * 60);
        return days == 1   ? QStringLiteral("Day")
               : days == 7 ? QStringLiteral("Week")
                           : QStringLiteral("%1 days").arg(days);
    }
    if (minutes > 0 && minutes % 60 == 0)
        return minutes == 60 ? QStringLiteral("Hour")
                             : QStringLiteral("%1 hours").arg(minutes / 60);
    return minutes > 0 ? QStringLiteral("%1 minutes").arg(minutes) : QStringLiteral("Limit");
}

QDateTime iso_time(const QJsonValue& value) {
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

// OMP's plan ids as the CLIs lapis knows them.
QString omp_provider(const QString& id) {
    static const QHash<QString, QString> names{
        {QStringLiteral("openai-codex"), QStringLiteral("codex")},
        {QStringLiteral("openai-codex-device"), QStringLiteral("codex")},
        {QStringLiteral("anthropic"), QStringLiteral("claude")},
        {QStringLiteral("xai-oauth"), QStringLiteral("grok")},
        {QStringLiteral("xai"), QStringLiteral("grok")},
        {QStringLiteral("kimi-code"), QStringLiteral("kimi")},
        {QStringLiteral("google-antigravity"), QStringLiteral("antigravity")},
        {QStringLiteral("google-gemini-cli"), QStringLiteral("gemini")},
        {QStringLiteral("zai"), QStringLiteral("zai")},
        {QStringLiteral("zai-coding-plan"), QStringLiteral("zai")},
        {QStringLiteral("github-copilot"), QStringLiteral("copilot")},
    };
    return names.value(id, id);
}

bool same_window(const UsageWindow& a, const UsageWindow& b) {
    return a.minutes == b.minutes && a.resets.isValid() && b.resets.isValid() &&
           std::abs(a.resets.secsTo(b.resets)) <= 120 && std::abs(a.percent - b.percent) <= 1.0;
}

// One OMP limit as a window; empty when it has no amount.
std::optional<UsageWindow> omp_window(const QJsonObject& limit) {
    const auto amount = limit.value(QStringLiteral("amount")).toObject();
    const auto window = limit.value(QStringLiteral("window")).toObject();
    double percent = -1;
    if (amount.contains(QStringLiteral("usedFraction")))
        percent = amount.value(QStringLiteral("usedFraction")).toDouble() * 100.0;
    else if (amount.value(QStringLiteral("limit")).toDouble() > 0)
        percent = 100.0 * amount.value(QStringLiteral("used")).toDouble() /
                  amount.value(QStringLiteral("limit")).toDouble();
    if (percent < 0)
        return std::nullopt;
    const int minutes =
        static_cast<int>(window.value(QStringLiteral("durationMs")).toDouble() / 60000.0);
    const auto resets = static_cast<qint64>(window.value(QStringLiteral("resetsAt")).toDouble());
    return UsageWindow{.label = minutes > 0 ? window_label(minutes)
                                            : limit.value(QStringLiteral("label")).toString(),
                       .percent = percent,
                       .resets = resets > 0 ? QDateTime::fromMSecsSinceEpoch(resets) : QDateTime(),
                       .minutes = minutes};
}
} // namespace

PlanLimits codex_limits(const QJsonObject& result) {
    PlanLimits limits{.provider = QStringLiteral("codex"), .source = QStringLiteral("codex")};
    const auto main = result.value(QStringLiteral("rateLimits")).toObject();
    limits.plan = main.value(QStringLiteral("planType")).toString();
    limits.accountId = result.value(QStringLiteral("accountId")).toString();
    const auto add = [&](const QJsonObject& bucket, const QString& name) {
        for (const auto* which : {"primary", "secondary"}) {
            const auto window = bucket.value(QLatin1String(which)).toObject();
            if (window.isEmpty())
                continue;
            const int minutes = window.value(QStringLiteral("windowDurationMins")).toInt();
            const auto resets =
                static_cast<qint64>(window.value(QStringLiteral("resetsAt")).toDouble());
            auto label = window_label(minutes);
            if (!name.isEmpty())
                label += QStringLiteral(", ") + name;
            limits.windows.push_back(
                {.label = label,
                 .percent = window.value(QStringLiteral("usedPercent")).toDouble(),
                 .resets = resets > 0 ? QDateTime::fromSecsSinceEpoch(resets) : QDateTime(),
                 .minutes = minutes});
        }
    };
    add(main, QString());
    // Other buckets (a model with its own limit), each named by the server.
    const auto others = result.value(QStringLiteral("rateLimitsByLimitId")).toObject();
    const auto main_id = main.value(QStringLiteral("limitId")).toString();
    for (auto it = others.begin(); it != others.end(); ++it) {
        if (it.key() == main_id)
            continue;
        const auto bucket = it.value().toObject();
        const auto name = bucket.value(QStringLiteral("limitName")).toString();
        add(bucket, name.isEmpty() ? it.key() : name);
    }
    if (!main.value(QStringLiteral("rateLimitReachedType")).toString().isEmpty())
        limits.note = QStringLiteral("Limit reached");
    limits.resetCredits = result.value(QStringLiteral("rateLimitResetCredits"))
                              .toObject()
                              .value(QStringLiteral("availableCount"))
                              .toInt();
    return limits;
}

PlanLimits claude_limits(const QJsonObject& response) {
    PlanLimits limits{.provider = QStringLiteral("claude"), .source = QStringLiteral("claude")};
    limits.plan = response.value(QStringLiteral("subscription_type")).toString();
    if (!response.value(QStringLiteral("rate_limits_available")).toBool()) {
        limits.note = QStringLiteral("No plan limits for this login");
        return limits;
    }
    const auto rates = response.value(QStringLiteral("rate_limits")).toObject();
    const auto add = [&](const QJsonObject& window, const QString& label, int minutes) {
        if (window.isEmpty() || window.value(QStringLiteral("utilization")).isNull())
            return;
        limits.windows.push_back({.label = label,
                                  .percent = window.value(QStringLiteral("utilization")).toDouble(),
                                  .resets = iso_time(window.value(QStringLiteral("resets_at"))),
                                  .minutes = minutes});
    };
    add(rates.value(QStringLiteral("five_hour")).toObject(), QStringLiteral("5 hours"), 5 * 60);
    add(rates.value(QStringLiteral("seven_day")).toObject(), QStringLiteral("Week"), kWeek);
    // Per-model weekly windows, as the server names them.
    const auto scoped = rates.value(QStringLiteral("model_scoped")).toArray();
    for (const auto& item : scoped) {
        const auto window = item.toObject();
        add(window,
            QStringLiteral("Week, ") + window.value(QStringLiteral("display_name")).toString(),
            kWeek);
    }
    if (scoped.isEmpty()) {
        add(rates.value(QStringLiteral("seven_day_opus")).toObject(), QStringLiteral("Week, Opus"),
            kWeek);
        add(rates.value(QStringLiteral("seven_day_sonnet")).toObject(),
            QStringLiteral("Week, Sonnet"), kWeek);
    }
    return limits;
}

// {"config": {"creditUsagePercent": 100.0, "currentPeriod": {"type":
// "USAGE_PERIOD_TYPE_WEEKLY", "start": ..., "end": ...}, ...}}
PlanLimits grok_limits(const QJsonObject& billing) {
    PlanLimits limits{.provider = QStringLiteral("grok"), .source = QStringLiteral("grok")};
    const auto config = billing.value(QStringLiteral("config")).toObject();
    if (!config.contains(QStringLiteral("creditUsagePercent"))) {
        limits.note = QStringLiteral("No plan limits for this login");
        return limits;
    }
    const auto period = config.value(QStringLiteral("currentPeriod")).toObject();
    const auto start = iso_time(period.value(QStringLiteral("start")));
    const auto end = iso_time(period.value(QStringLiteral("end")));
    const int minutes =
        start.isValid() && end.isValid() ? static_cast<int>(start.secsTo(end) / 60) : 0;
    limits.windows.push_back(
        {.label = window_label(minutes),
         .percent = config.value(QStringLiteral("creditUsagePercent")).toDouble(),
         .resets = end,
         .minutes = minutes});
    if (limits.windows.front().percent >= 100)
        limits.note = QStringLiteral("Limit reached");
    return limits;
}

// {"kind": "ok", "summary": {"window": {"duration": 1, "unit": "week"},
// "used": 0, "limit": 100, "reset_at": ...}, "limits": [...]}
PlanLimits kimi_limits(const QJsonObject& data) {
    PlanLimits limits{.provider = QStringLiteral("kimi"), .source = QStringLiteral("kimi")};
    const auto add = [&](const QJsonObject& row) {
        const auto window = row.value(QStringLiteral("window")).toObject();
        const int duration = window.value(QStringLiteral("duration")).toInt();
        const auto unit = window.value(QStringLiteral("unit")).toString();
        const int scale = unit == QLatin1String("week")     ? kWeek
                          : unit == QLatin1String("day")    ? 24 * 60
                          : unit == QLatin1String("hour")   ? 60
                          : unit == QLatin1String("minute") ? 1
                                                            : 0;
        const double limit = row.value(QStringLiteral("limit")).toDouble();
        if (limit <= 0)
            return;
        const int minutes = duration * scale;
        limits.windows.push_back(
            {.label =
                 minutes > 0 ? window_label(minutes) : row.value(QStringLiteral("name")).toString(),
             .percent = 100.0 * row.value(QStringLiteral("used")).toDouble() / limit,
             .resets = iso_time(row.value(QStringLiteral("reset_at"))),
             .minutes = minutes});
    };
    add(data.value(QStringLiteral("summary")).toObject());
    for (const auto& row : data.value(QStringLiteral("limits")).toArray())
        add(row.toObject());
    return limits;
}

// {"reports": [{"provider": "openai-codex", "limits": [{"label", "window":
// {"durationMs", "resetsAt"}, "amount": {"usedFraction"}, "status"}],
// "metadata": {"email", "accountId", "planType"}}]}
std::vector<PlanLimits> omp_limits(const QJsonObject& result) {
    std::vector<PlanLimits> accounts;
    for (const auto& item : result.value(QStringLiteral("reports")).toArray()) {
        const auto report = item.toObject();
        const auto metadata = report.value(QStringLiteral("metadata")).toObject();
        PlanLimits limits{.provider =
                              omp_provider(report.value(QStringLiteral("provider")).toString()),
                          .source = QStringLiteral("omp"),
                          .account = metadata.value(QStringLiteral("email")).toString(),
                          .accountId = metadata.value(QStringLiteral("accountId")).toString(),
                          .plan = metadata.value(QStringLiteral("planType")).toString()};
        for (const auto& value : report.value(QStringLiteral("limits")).toArray()) {
            const auto limit = value.toObject();
            auto window = omp_window(limit);
            // The same window reported twice under two names counts once.
            if (!window ||
                std::any_of(limits.windows.begin(), limits.windows.end(),
                            [&](const UsageWindow& seen) { return same_window(seen, *window); }))
                continue;
            // Two windows of one length are told apart by the plan's own name.
            if (std::any_of(limits.windows.begin(), limits.windows.end(),
                            [&](const UsageWindow& seen) { return seen.label == window->label; }))
                window->label +=
                    QStringLiteral(", ") + limit.value(QStringLiteral("label")).toString();
            limits.windows.push_back(*window);
            if (limit.value(QStringLiteral("status")).toString() == QLatin1String("exhausted"))
                limits.note = QStringLiteral("Limit reached");
        }
        if (!limits.windows.empty())
            accounts.push_back(std::move(limits));
    }
    return accounts;
}

bool same_account(const PlanLimits& a, const PlanLimits& b) {
    if (a.provider != b.provider)
        return false;
    if (!a.accountId.isEmpty() && a.accountId == b.accountId)
        return true;
    return std::any_of(a.windows.begin(), a.windows.end(), [&](const UsageWindow& window) {
        return std::any_of(b.windows.begin(), b.windows.end(),
                           [&](const UsageWindow& other) { return same_window(window, other); });
    });
}

QString shell_words(const QStringList& words) {
    static const QRegularExpression plain(QStringLiteral(R"(^[A-Za-z0-9_./:=@%+,-]+$)"));
    QStringList quoted;
    for (const auto& word : words)
        quoted << (plain.match(word).hasMatch()
                       ? word
                       : QLatin1Char('\'') +
                             QString(word).replace(QLatin1Char('\''), QStringLiteral(R"('\'')")) +
                             QLatin1Char('\''));
    return quoted.join(QLatin1Char(' '));
}

UsageProbe::UsageProbe(Setup setup, Done done, QObject* parent)
    : QObject(parent), setup_(std::move(setup)), done_(std::move(done)) {}

UsageProbe::~UsageProbe() {
    if (process_) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
        delete process_.data();
    }
}

namespace {
// A loopback port free right now, for kimi web.
int free_port() {
    QTcpServer server;
    return server.listen(QHostAddress::LocalHost, 0) ? server.serverPort() : 0;
}

QStringList probe_arguments(const QString& provider, int port, const QString& sessions) {
    if (provider == QLatin1String("codex"))
        return {QStringLiteral("app-server")};
    if (provider == QLatin1String("claude"))
        return {QStringLiteral("-p"),
                QStringLiteral("--input-format"),
                QStringLiteral("stream-json"),
                QStringLiteral("--output-format"),
                QStringLiteral("stream-json"),
                QStringLiteral("--verbose"),
                QStringLiteral("--setting-sources"),
                QString(),
                QStringLiteral("--no-session-persistence")};
    if (provider == QLatin1String("grok"))
        return {QStringLiteral("agent"), QStringLiteral("--no-leader"), QStringLiteral("stdio")};
    if (provider == QLatin1String("kimi"))
        return {QStringLiteral("web"), QStringLiteral("--no-open"), QStringLiteral("--port"),
                QString::number(port)};
    return {QStringLiteral("--session-dir=") + sessions,
            QStringLiteral("--no-extensions"),
            QStringLiteral("--no-lsp"),
            QStringLiteral("--no-tools"),
            QStringLiteral("--no-skills"),
            QStringLiteral("--no-rules"),
            QStringLiteral("--no-title"),
            QStringLiteral("acp")};
}

QJsonObject rpc(int id, const char* method, const QJsonObject& params) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("method"), QString::fromLatin1(method)},
            {QStringLiteral("params"), params}};
}
} // namespace

void UsageProbe::start() {
    const auto& provider = setup_.provider;
    const bool kimi = provider == QLatin1String("kimi");
    if (kimi)
        port_ = free_port();
    auto program = setup_.program;
    auto arguments = probe_arguments(provider, port_, scratch_.path());
    if (!setup_.host.isEmpty()) {
        auto command = shell_words(QStringList{program} + arguments);
        // kimi web never reads its input: stop it when the connection closes.
        if (kimi)
            command += QStringLiteral(
                R"( & k=$!; trap 'kill $k 2>/dev/null' EXIT HUP TERM; cat >/dev/null)");
        QStringList ssh{QStringLiteral("-T"), QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=8")};
        if (kimi)
            ssh << QStringLiteral("-o") << QStringLiteral("ExitOnForwardFailure=yes")
                << QStringLiteral("-L") << QStringLiteral("%1:127.0.0.1:%1").arg(port_);
        ssh << setup_.host
            << QStringLiteral(R"(cd /tmp 2>/dev/null; exec "${SHELL:-/bin/sh}" -lic %1)")
                   .arg(shell_words({command}));
        program = setup_.ssh;
        arguments = ssh;
    }
    process_ = new QProcess;
    process_->setProgram(program);
    process_->setArguments(arguments);
    process_->setWorkingDirectory(scratch_.isValid() ? scratch_.path() : QDir::tempPath());
    process_->setStandardErrorFile(QProcess::nullDevice());
    connect(process_, &QProcess::readyReadStandardOutput, this,
            kimi ? &UsageProbe::kimiOutput : &UsageProbe::read);
    connect(process_, &QProcess::finished, this, [this](int code) {
        // A shell that cannot find the CLI exits 127; ssh that cannot connect, 255.
        const bool unreachable = !setup_.host.isEmpty() && code == 255;
        finish({.limits = std::nullopt,
                .failure = code == 127   ? QStringLiteral("is not installed")
                           : unreachable ? QStringLiteral("could not connect")
                                         : QStringLiteral("ended without an answer"),
                .absent = code == 127});
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({.limits = std::nullopt,
                    .failure = QStringLiteral("could not start"),
                    .absent = setup_.host.isEmpty()});
    });
    QTimer::singleShot(kAnswerMs, this, [this] {
        finish(
            {.limits = std::nullopt, .failure = QStringLiteral("did not answer"), .absent = false});
    });
    process_->start();
    if (provider == QLatin1String("codex"))
        send(rpc(1, "initialize",
                 {{QStringLiteral("clientInfo"),
                   QJsonObject{{QStringLiteral("name"), QStringLiteral("lapis")},
                               {QStringLiteral("version"), QStringLiteral("1")}}}}));
    else if (provider == QLatin1String("claude"))
        send({{QStringLiteral("type"), QStringLiteral("control_request")},
              {QStringLiteral("request_id"), QStringLiteral("usage")},
              {QStringLiteral("request"),
               QJsonObject{{QStringLiteral("subtype"), QStringLiteral("get_usage")},
                           {QStringLiteral("skip_behaviors"), true}}}});
    else if (!kimi)
        send(rpc(1, "initialize",
                 {{QStringLiteral("protocolVersion"), 1},
                  {QStringLiteral("clientCapabilities"), QJsonObject{}}}));
}

void UsageProbe::send(const QJsonObject& message) {
    if (process_)
        process_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void UsageProbe::read() {
    if (!process_)
        return;
    pending_ += process_->readAllStandardOutput();
    if (pending_.size() > kLongestAnswer)
        return finish({.limits = std::nullopt,
                       .failure = QStringLiteral("answered too much"),
                       .absent = false});
    for (auto end = pending_.indexOf('\n'); end >= 0 && !finished_; end = pending_.indexOf('\n')) {
        const auto message = QJsonDocument::fromJson(pending_.left(end)).object();
        pending_.remove(0, end + 1);
        if (!message.isEmpty())
            line(message);
    }
}

void UsageProbe::line(const QJsonObject& message) {
    const auto& provider = setup_.provider;
    const auto answered = [this](std::vector<PlanLimits> limits) {
        const bool none = std::all_of(limits.begin(), limits.end(),
                                      [](const PlanLimits& item) { return item.windows.empty(); });
        finish({.limits = std::move(limits),
                .failure = none ? QStringLiteral("has no plan limits") : QString(),
                .absent = none});
    };
    // An error answer means the CLI runs but is not signed in to a plan.
    const auto refused = [this](const QJsonObject& reply) {
        finish({.limits = std::nullopt,
                .failure = reply.value(QStringLiteral("error"))
                               .toObject()
                               .value(QStringLiteral("message"))
                               .toString(QStringLiteral("refused")),
                .absent = true});
    };
    if (provider == QLatin1String("claude")) {
        const auto response = message.value(QStringLiteral("response")).toObject();
        if (message.value(QStringLiteral("type")).toString() != QLatin1String("control_response") ||
            response.value(QStringLiteral("request_id")).toString() != QLatin1String("usage"))
            return;
        if (response.value(QStringLiteral("subtype")).toString() != QLatin1String("success"))
            return finish({.limits = std::nullopt,
                           .failure = response.value(QStringLiteral("error")).toString(),
                           .absent = true});
        return answered({claude_limits(response.value(QStringLiteral("response")).toObject())});
    }
    const int id = message.value(QStringLiteral("id")).toInt();
    const auto result = message.value(QStringLiteral("result")).toObject();
    if (id == 0)
        return;
    if (message.contains(QStringLiteral("error")))
        return refused(message);
    if (provider == QLatin1String("codex")) {
        if (id == 1) {
            send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                  {QStringLiteral("method"), QStringLiteral("initialized")}});
            send(rpc(2, "account/rateLimits/read", {}));
        } else if (id == 2) {
            answered({codex_limits(result)});
        }
        return;
    }
    if (provider == QLatin1String("grok")) {
        if (id == 1)
            send(rpc(2, "_x.ai/billing", {}));
        else if (id == 2)
            answered({grok_limits(result)});
        return;
    }
    // omp: a session (kept in the scratch folder), then every account's usage.
    if (id == 1)
        send(rpc(2, "session/new",
                 {{QStringLiteral("cwd"), scratch_.path()},
                  {QStringLiteral("mcpServers"), QJsonArray{}}}));
    else if (id == 2)
        send(rpc(3, "_omp/usage", {}));
    else if (id == 3)
        answered(omp_limits(result));
}

// kimi web prints its address and token once it listens; the usage is then
// one authorized GET on loopback (forwarded from the host over ssh).
void UsageProbe::kimiOutput() {
    if (!process_)
        return;
    text_ += process_->readAllStandardOutput();
    if (asked_ || text_.size() > kLongestStartup)
        return;
    static const QRegularExpression token(QStringLiteral(R"(#token=([A-Za-z0-9._~-]+))"));
    const auto match = token.match(QString::fromUtf8(text_));
    if (!match.hasMatch())
        return;
    asked_ = true;
    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/oauth/usage").arg(port_)));
    request.setRawHeader("Authorization", "Bearer " + match.captured(1).toUtf8());
    auto* reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const auto body = QJsonDocument::fromJson(reply->readAll()).object();
        const auto data = body.value(QStringLiteral("data")).toObject();
        if (reply->error() != QNetworkReply::NoError && body.isEmpty())
            return finish(
                {.limits = std::nullopt, .failure = reply->errorString(), .absent = false});
        if (data.value(QStringLiteral("kind")).toString() != QLatin1String("ok"))
            return finish({.limits = std::nullopt,
                           .failure = data.value(QStringLiteral("message")).toString(),
                           .absent = true});
        auto limits = kimi_limits(data);
        const bool none = limits.windows.empty();
        finish({.limits = std::vector<PlanLimits>{std::move(limits)},
                .failure = none ? QStringLiteral("has no plan limits") : QString(),
                .absent = none});
    });
}

void UsageProbe::finish(const Result& result) {
    if (finished_)
        return;
    finished_ = true;
    if (auto* process = process_.data()) {
        process->disconnect(this);
        process_.clear();
        // Deleted once it has exited, so it is reaped rather than left running.
        if (process->state() == QProcess::NotRunning) {
            process->deleteLater();
        } else {
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
            process->kill();
        }
    }
    if (done_)
        done_(result);
}
} // namespace lapis::desktop
