#include "usage.hpp"
#include "workspace.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using lapis::desktop::TokenCount;
using lapis::desktop::TokenLedger;
using lapis::desktop::Usage;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// A local day's noon as the CLIs write it: UTC with a Z.
QByteArray stamp(QDate day) {
    return QDateTime(day, QTime(12, 0)).toUTC().toString(Qt::ISODateWithMs).toUtf8();
}

void write(const QString& path, const QByteArray& text, bool append = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(append ? QIODevice::Append : QIODevice::WriteOnly), "open a fixture");
    require(file.write(text) == text.size(), "write a fixture");
}

// A running total, and the last call's own use (by default all of it).
QByteArray codex_total(const QByteArray& when, int input, int cached, int output,
                       int last_input = -1, int last_cached = 0, int last_output = 0) {
    if (last_input < 0) {
        last_input = input;
        last_cached = cached;
        last_output = output;
    }
    const auto usage = [](int in, int hit, int out) {
        return R"({"input_tokens":)" + QByteArray::number(in) + R"(,"cached_input_tokens":)" +
               QByteArray::number(hit) + R"(,"cache_write_input_tokens":0,"output_tokens":)" +
               QByteArray::number(out) + R"(,"reasoning_output_tokens":1,"total_tokens":0})";
    };
    return R"({"timestamp":")" + when +
           R"(","ordinal":3,"type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":)" +
           usage(input, cached, output) + R"(,"last_token_usage":)" +
           usage(last_input, last_cached, last_output) +
           R"(},"rate_limits":{"primary":{"used_percent":5.0}}}})"
           "\n";
}

QByteArray codex_model(const QByteArray& when, const QByteArray& model) {
    return R"({"timestamp":")" + when +
           R"(","type":"turn_context","payload":{"cwd":"/w","model":")" + model +
           R"(","effort":"high"}})"
           "\n";
}

QByteArray claude_line(const QByteArray& when, const QByteArray& id, const QByteArray& request,
                       const QByteArray& model, int input, int written, int read, int output,
                       const QByteArray& content = R"([{"type":"text","text":"ok"}])") {
    return R"({"parentUuid":"p","isSidechain":false,"message":{"model":")" + model + R"(","id":")" +
           id + R"(","type":"message","role":"assistant","content":)" + content +
           R"(,"stop_reason":null,"usage":{"input_tokens":)" + QByteArray::number(input) +
           R"(,"cache_creation_input_tokens":)" + QByteArray::number(written) +
           R"(,"cache_read_input_tokens":)" + QByteArray::number(read) +
           R"(,"cache_creation":{"ephemeral_5m_input_tokens":0},"output_tokens":)" +
           QByteArray::number(output) + R"(,"service_tier":"standard"}},"requestId":")" + request +
           R"(","type":"assistant","uuid":"u","timestamp":")" + when +
           R"("})"
           "\n";
}

TokenCount model_on(const QHash<QString, TokenLedger::Days>& days, const char* provider, QDate day,
                    const char* model) {
    const auto& by_day = days.value(QString::fromLatin1(provider));
    const auto found = by_day.find(day);
    return found == by_day.end() ? TokenCount{} : found->second.value(QString::fromLatin1(model));
}

// Codex's running totals become per-call use, by the model of the turn;
// Claude's repeated message lines count once, across files and subagents;
// a line still being written waits; other lines that mention usage do not
// count; files untouched since the window began are not read.
void counts_tokens_from_transcripts() {
    QTemporaryDir home;
    require(home.isValid(), "temporary home");
    const auto codex = home.filePath(QStringLiteral("codex"));
    const auto claude = home.filePath(QStringLiteral("claude"));
    const auto today = QDate::currentDate();
    const auto now = stamp(today);
    const auto yesterday = stamp(today.addDays(-1));

    write(
        codex + QStringLiteral("/2026/09/24/rollout-a.jsonl"),
        R"({"timestamp":")" + now +
            R"(","type":"session_meta","payload":{"id":"a","source":"cli"}})"
            "\n" +
            codex_model(now, "gpt-6-astra") + R"({"timestamp":")" + now +
            R"(","type":"event_msg","payload":{"type":"token_count","info":null}})"
            "\n" +
            codex_total(now, 1000, 600, 50) + codex_total(now, 1000, 600, 50) +
            codex_total(now, 2500, 1600, 120) + R"({"timestamp":")" + now +
            R"(","type":"response_item","payload":{"type":"message","content":"\"payload\":{\"type\":\"token_count\"},\"total_token_usage\":{\"input_tokens\":99999}"}})"
            "\n" +
            codex_model(now, "gpt-6-sol") + codex_total(now, 4000, 3000, 200));
    // A fork that starts its total at zero, and one that starts from its
    // parent's total: only the fork's own calls count.
    write(codex + QStringLiteral("/2026/09/23/rollout-b.jsonl"),
          codex_model(yesterday, "gpt-6-astra") + codex_total(yesterday, 300, 100, 10));
    write(codex + QStringLiteral("/2026/09/23/rollout-c.jsonl"),
          codex_model(yesterday, "gpt-6-luna") +
              codex_total(yesterday, 900000, 800000, 9000, 50, 0, 5) +
              codex_total(yesterday, 900100, 800000, 9010));

    const auto project = claude + QStringLiteral("/-w");
    write(
        project + QStringLiteral("/s1.jsonl"),
        R"({"type":"user","message":{"role":"user","content":"hi"},"timestamp":")" + now +
            R"("})"
            "\n" +
            claude_line(now, "msg_1", "req_1", "claude-opus-5-5", 10, 100, 1000, 5) +
            claude_line(now, "msg_1", "req_1", "claude-opus-5-5", 10, 100, 1000, 5,
                        R"([{"type":"tool_use","id":"toolu_1","name":"Bash","input":{}}])") +
            R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","content":"ok"}]},"toolUseResult":{"usage":{"input_tokens":999999,"output_tokens":999999},"type":"assistant"},"timestamp":")" +
            now +
            R"("})"
            "\n" +
            R"({"message":{"diagnostics":{},"id":"x","container":null,"model":"<synthetic>","role":"assistant","usage":{"input_tokens":7,"output_tokens":7}},"type":"assistant","timestamp":")" +
            now +
            R"("})"
            "\n" +
            claude_line(now, "msg_2", "req_2", "claude-fable-5-1", 20, 0, 2000, 40) +
            claude_line(yesterday, "msg_0", "req_0", "claude-fable-5-1", 1, 0, 0, 1));
    // Still being written: no newline yet.
    auto partial = claude_line(now, "msg_3", "req_3", "claude-fable-5-1", 3, 0, 0, 3);
    partial.chop(1);
    write(project + QStringLiteral("/s1.jsonl"), partial.left(60), true);
    // A later line of msg_5 has its final output count; a resumed session
    // repeats msg_1 and adds its own.
    write(
        project + QStringLiteral("/s2.jsonl"),
        claude_line(now, "msg_1", "req_1", "claude-opus-5-5", 10, 100, 1000, 5) +
            claude_line(
                now, "msg_4", "req_4", "claude-opus-5-5", 1, 2, 3, 4,
                R"([{"type":"tool_use","id":"toolu_2","name":"Probe","input":{"usage":{"input_tokens":5000,"note":"}"}}}])"));
    write(project + QStringLiteral("/s1/subagents/agent-x.jsonl"),
          claude_line(now, "msg_5", "req_5", "claude-fable-5-1", 5, 0, 0, 1) +
              claude_line(now, "msg_5", "req_5", "claude-fable-5-1", 5, 0, 0, 5));
    // Untouched since before the window: not read even though it would count.
    const auto old = project + QStringLiteral("/old.jsonl");
    write(old, claude_line(now, "msg_9", "req_9", "claude-opus-5-5", 1000, 0, 0, 0));
    {
        QFile file(old);
        require(file.open(QIODevice::ReadWrite) &&
                    file.setFileTime(QDateTime(today.addDays(-90), QTime(1, 0)),
                                     QFileDevice::FileModificationTime),
                "age a fixture");
    }

    TokenLedger ledger({codex, claude});
    ledger.scan(today.addDays(-29));
    auto days = ledger.days();
    require(model_on(days, "codex", today, "gpt-6-astra") ==
                TokenCount{.input = 900, .cacheRead = 1600, .cacheWrite = 0, .output = 120},
            "Codex: running totals become the calls' use, once each");
    require(model_on(days, "codex", today, "gpt-6-sol") ==
                TokenCount{.input = 100, .cacheRead = 1400, .cacheWrite = 0, .output = 80},
            "Codex: a new turn's model takes the next calls");
    require(model_on(days, "codex", today.addDays(-1), "gpt-6-astra").total() == 310,
            "Codex: a fork counts from its own zero, on its own day");
    require(model_on(days, "codex", today.addDays(-1), "gpt-6-luna").total() == 55 + 110,
            "Codex: a fork that starts from its parent's total counts only its own calls");
    require(model_on(days, "claude", today, "claude-opus-5-5") ==
                TokenCount{.input = 11, .cacheRead = 1003, .cacheWrite = 102, .output = 9},
            "Claude: a message counts once across its lines and a resumed session");
    require(model_on(days, "claude", today, "claude-fable-5-1").output == 40 + 5,
            "Claude: a message counts at its final output, not its first line's");
    require(model_on(days, "claude", today, "claude-fable-5-1").total() == 2060 + 10,
            "Claude: subagents count; tool results, synthetic lines and partial lines do not");
    require(model_on(days, "claude", today.addDays(-1), "claude-fable-5-1").total() == 2,
            "Claude: lines count on their local day");
    require(!days.value(QStringLiteral("claude")).empty() &&
                model_on(days, "claude", today, "<synthetic>").total() == 0,
            "no synthetic model");

    // The rest of the partial line arrives; only new bytes are read.
    const auto before = ledger.bytesRead();
    write(project + QStringLiteral("/s1.jsonl"), partial.mid(60) + "\n", true);
    ledger.scan(today.addDays(-29));
    days = ledger.days();
    require(model_on(days, "claude", today, "claude-fable-5-1").total() == 2060 + 10 + 6,
            "a finished line counts once complete");
    require(ledger.bytesRead() - before < 1024, "a rescan reads what was appended");
    ledger.scan(today.addDays(-29));
    require(ledger.days() == days, "a rescan with nothing new changes nothing");
}

// The answers as each CLI gave them live on September 24 (identifiers and
// marketing text removed).
void reads_plan_limits() {
    const auto codex = lapis::desktop::codex_limits(QJsonDocument::fromJson(R"({
        "rateLimits": {"limitId": "codex", "limitName": null,
            "primary": {"usedPercent": 100, "windowDurationMins": 10080, "resetsAt": 1790558544},
            "secondary": {"usedPercent": 12, "windowDurationMins": 300, "resetsAt": 1790400000},
            "planType": "pro", "rateLimitReachedType": "rate_limit_reached"},
        "rateLimitsByLimitId": {
            "codex": {"limitId": "codex"},
            "codex_other": {"limitId": "codex_other", "limitName": "GPT-6 Luna",
                "primary": {"usedPercent": 3, "windowDurationMins": 10080, "resetsAt": 1790558544}}},
        "rateLimitResetCredits": {"availableCount": 1}})")
                                                        .object());
    require(codex.windows.size() == 3, "Codex: both windows and the other bucket");
    require(codex.windows[0].label == QStringLiteral("Week") && codex.windows[0].percent == 100 &&
                codex.windows[0].resets == QDateTime::fromSecsSinceEpoch(1790558544) &&
                codex.windows[0].minutes == 10080,
            "Codex: the weekly window");
    require(codex.windows[1].label == QStringLiteral("5 hours"), "Codex: the five-hour window");
    require(codex.windows[2].label == QStringLiteral("Week, GPT-6 Luna"), "Codex: a named bucket");
    require(codex.plan == QStringLiteral("pro") && codex.note == QStringLiteral("Limit reached") &&
                codex.resetCredits == 1,
            "Codex: plan, reached, free resets");

    const auto claude = lapis::desktop::claude_limits(QJsonDocument::fromJson(R"({
        "subscription_type": "max", "rate_limits_available": true,
        "rate_limits": {
            "five_hour": {"utilization": 87, "resets_at": "2026-09-25T03:09:59.945481+00:00"},
            "seven_day": {"utilization": 23, "resets_at": "2026-09-28T11:59:59.945505+00:00"},
            "seven_day_opus": null,
            "model_scoped": [{"display_name": "Fable", "utilization": 0, "resets_at": "2026-09-28T12:00:00+00:00"}]}})")
                                                          .object());
    require(claude.windows.size() == 3 && claude.plan == QStringLiteral("max"), "Claude: windows");
    require(claude.windows[0].label == QStringLiteral("5 hours") &&
                claude.windows[0].percent == 87 && claude.windows[0].minutes == 300 &&
                claude.windows[0].resets.toUTC() ==
                    QDateTime(QDate(2026, 9, 25), QTime(3, 9, 59, 945), QTimeZone::UTC),
            "Claude: the five-hour window and its reset");
    require(claude.windows[1].label == QStringLiteral("Week") && claude.windows[1].percent == 23,
            "Claude: the week");
    require(claude.windows[2].label == QStringLiteral("Week, Fable"), "Claude: a model's week");
    const auto api = lapis::desktop::claude_limits(
        QJsonDocument::fromJson(
            R"({"subscription_type": null, "rate_limits_available": false, "rate_limits": null})")
            .object());
    require(api.windows.empty() && !api.note.isEmpty(), "Claude: an API key has no plan limits");

    const auto grok = lapis::desktop::grok_limits(QJsonDocument::fromJson(R"({"config": {
        "creditUsagePercent": 100.0,
        "currentPeriod": {"type": "USAGE_PERIOD_TYPE_WEEKLY",
            "start": "2026-09-21T18:48:35.175614+00:00", "end": "2026-09-28T18:48:35.175614+00:00"},
        "onDemandCap": {"val": 0}, "isUnifiedBillingUser": true}})")
                                                      .object());
    require(grok.windows.size() == 1 && grok.windows[0].label == QStringLiteral("Week") &&
                grok.windows[0].percent == 100 && grok.windows[0].minutes == 10080 &&
                grok.note == QStringLiteral("Limit reached"),
            "Grok: the weekly credits");

    const auto kimi = lapis::desktop::kimi_limits(QJsonDocument::fromJson(R"({"kind": "ok",
        "summary": {"window": {"duration": 1, "unit": "week"}, "used": 25, "limit": 100,
            "reset_at": "2026-10-01T06:00:06.646803Z"},
        "limits": [{"window": {"duration": 5, "unit": "hour"}, "used": 1, "limit": 4,
            "reset_at": "2026-09-25T03:00:06.646803Z"}], "extra_usage": null})")
                                                      .object());
    require(kimi.windows.size() == 2 && kimi.windows[0].label == QStringLiteral("Week") &&
                kimi.windows[0].percent == 25 &&
                kimi.windows[1].label == QStringLiteral("5 hours") &&
                kimi.windows[1].percent == 25 && kimi.windows[1].minutes == 300,
            "Kimi: the week and five hours, as used over the limit");

    const auto omp = lapis::desktop::omp_limits(QJsonDocument::fromJson(R"json({"reports": [
        {"provider": "openai-codex", "metadata": {"planType": "pro", "email": "a@example.com", "accountId": "acct-a"},
         "limits": [{"label": "7 days", "window": {"durationMs": 604800000, "resetsAt": 1790558545000},
                     "amount": {"usedFraction": 1, "unit": "percent"}, "status": "exhausted"}]},
        {"provider": "kimi-code", "metadata": {"accountId": "k1"},
         "limits": [{"label": "Weekly limit", "window": {"durationMs": 604800000, "resetsAt": 1790834406646},
                     "amount": {"used": 0, "limit": 100, "unit": "unknown"}},
                    {"label": "limit_7d", "window": {"durationMs": 604800000, "resetsAt": 1790834405000},
                     "amount": {"usedFraction": 0, "unit": "percent"}}]},
        {"provider": "xai-oauth", "metadata": {},
         "limits": [{"label": "SuperGrok Weekly Credits", "window": {"durationMs": 604800000, "resetsAt": 1790621315175},
                     "amount": {"usedFraction": 1}},
                    {"label": "Grok Build (Weekly)", "window": {"durationMs": 604800000, "resetsAt": 1790621315175},
                     "amount": {"usedFraction": 0.5}}]},
        {"provider": "cursor", "metadata": {}, "limits": []}]})json")
                                                    .object());
    require(omp.size() == 3, "OMP: accounts with windows");
    require(omp[0].provider == QStringLiteral("codex") &&
                omp[0].account == QStringLiteral("a@example.com") &&
                omp[0].accountId == QStringLiteral("acct-a") &&
                omp[0].plan == QStringLiteral("pro") && omp[0].windows.size() == 1 &&
                omp[0].windows[0].percent == 100 && omp[0].note == QStringLiteral("Limit reached"),
            "OMP: a Codex account as the CLI names it");
    require(omp[1].provider == QStringLiteral("kimi") && omp[1].windows.size() == 1,
            "OMP: one window reported under two names counts once");
    require(omp[2].provider == QStringLiteral("grok") && omp[2].windows.size() == 2 &&
                omp[2].windows[1].label == QStringLiteral("Week, Grok Build (Weekly)"),
            "OMP: two windows of one length keep the plan's own names");
    require(lapis::desktop::same_account(omp[0], codex),
            "the same Codex account, told by its weekly window");
    require(!lapis::desktop::same_account(omp[0], omp[2]),
            "different plans are different accounts");
    require(
        lapis::desktop::shell_words({QStringLiteral("codex"), QStringLiteral("--setting-sources"),
                                     QString(), QStringLiteral("it's")}) ==
            QStringLiteral(R"(codex --setting-sources '' 'it'\''s')"),
        "shell words are quoted for a remote shell");
}

template <typename Done> void wait_for(Done done, const char* what) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    require(done(), what);
}

QVariantMap machine_of(const Usage& usage, const QString& host) {
    for (const auto& item : usage.machines())
        if (item.toMap().value(QStringLiteral("host")).toString() == host)
            return item.toMap();
    return {};
}
QVariantMap provider(const Usage& usage, const char* id, const QString& host = QString()) {
    for (const auto& item : machine_of(usage, host).value(QStringLiteral("providers")).toList())
        if (item.toMap().value(QStringLiteral("id")).toString() == QLatin1String(id))
            return item.toMap();
    return {};
}
QStringList provider_ids(const Usage& usage, const QString& host = QString()) {
    QStringList ids;
    for (const auto& item : machine_of(usage, host).value(QStringLiteral("providers")).toList())
        ids << item.toMap().value(QStringLiteral("id")).toString();
    return ids;
}
QVariantList accounts(const Usage& usage, const char* id, const QString& host = QString()) {
    return provider(usage, id, host).value(QStringLiteral("accounts")).toList();
}
double first_percent(const QVariantList& list) {
    return list.isEmpty() ? -1
                          : list.front()
                                .toMap()
                                .value(QStringLiteral("windows"))
                                .toList()
                                .front()
                                .toMap()
                                .value(QStringLiteral("percent"))
                                .toDouble();
}

// Stand-ins for each CLI's usage interface, for ssh, and for a remote login
// shell that finds only the stand-ins.
struct Fakes {
    QTemporaryDir dir;
    QHash<QString, QString> programs;
    QString home; // the remote machine's
};
void make_fakes(Fakes& fakes) {
    require(fakes.dir.isValid(), "temporary directory");
    const auto bin = fakes.dir.filePath(QStringLiteral("bin"));
    QDir().mkpath(bin);
    fakes.home = fakes.dir.filePath(QStringLiteral("home"));
    QDir().mkpath(fakes.home);
    const auto add = [&](const QString& name, const QByteArray& body) {
        const auto path = bin + QLatin1Char('/') + name;
        write(path, "#!/bin/sh\n" + body);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
        fakes.programs.insert(name, path);
    };
    const auto args = [&](const char* name) {
        return QByteArray(R"(for a in "$@"; do printf '[%s]\n' "$a"; done > ")") +
               fakes.dir.filePath(QStringLiteral("%1-args").arg(QLatin1String(name))).toUtf8() +
               "\"\n";
    };
    add(QStringLiteral("codex"), args("codex") + R"(read -r line
echo '{"id":1,"result":{}}'
read -r line
read -r line
case "$line" in *account/rateLimits/read*) ;; *) exit 3 ;; esac
echo '{"id":2,"result":{"accountId":"acct-1","rateLimits":{"limitId":"codex","primary":{"usedPercent":40,"windowDurationMins":10080,"resetsAt":4071513600},"planType":"pro"}}}'
cat >/dev/null
)");
    add(QStringLiteral("claude"), args("claude") + R"(read -r line
case "$line" in *get_usage*) ;; *) exit 3 ;; esac
echo '{"type":"system","subtype":"init"}'
echo '{"type":"control_response","response":{"subtype":"success","request_id":"usage","response":{"subscription_type":"max","rate_limits_available":true,"rate_limits":{"five_hour":{"utilization":87,"resets_at":"2099-01-01T05:00:00+00:00"},"seven_day":{"utilization":23,"resets_at":"2099-01-08T00:00:00+00:00"}}}}}'
cat >/dev/null
)");
    add(QStringLiteral("grok"), args("grok") + R"(read -r line
echo '{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1}}'
read -r line
case "$line" in *_x.ai/billing*) ;; *) exit 3 ;; esac
echo '{"jsonrpc":"2.0","id":2,"result":{"config":{"creditUsagePercent":100.0,"currentPeriod":{"type":"USAGE_PERIOD_TYPE_WEEKLY","start":"2099-01-01T00:00:00Z","end":"2099-01-08T00:00:00Z"}}}}'
cat >/dev/null
)");
    add(QStringLiteral("omp"), args("omp") + R"(read -r line
echo '{"jsonrpc":"2.0","id":1,"result":{}}'
read -r line
case "$line" in *session/new*) ;; *) exit 3 ;; esac
echo '{"jsonrpc":"2.0","id":2,"result":{"sessionId":"s"}}'
read -r line
case "$line" in *_omp/usage*) ;; *) exit 3 ;; esac
echo '{"jsonrpc":"2.0","id":3,"result":{"reports":[{"provider":"openai-codex","metadata":{"accountId":"acct-1","email":"one@example.com"},"limits":[{"label":"7 days","window":{"durationMs":604800000,"resetsAt":4071513600000},"amount":{"usedFraction":0.4}}]},{"provider":"openai-codex","metadata":{"accountId":"acct-2","email":"two@example.com","planType":"pro"},"limits":[{"label":"7 days","window":{"durationMs":604800000,"resetsAt":4071000000000},"amount":{"usedFraction":0.1}}]},{"provider":"kimi-code","metadata":{"accountId":"k1"},"limits":[{"label":"Weekly limit","window":{"durationMs":604800000,"resetsAt":4071513600000},"amount":{"used":30,"limit":100}}]},{"provider":"zai","metadata":{},"limits":[{"label":"5h","window":{"durationMs":18000000,"resetsAt":4070926800000},"amount":{"usedFraction":0.2}}]}]}}'
cat >/dev/null
)");
    // kimi web: a loopback server that prints its token and answers usage.
    add(QStringLiteral("kimi"), args("kimi") + R"(exec python3 - "$@" <<'PY'
import http.server, json, sys
port = int(sys.argv[sys.argv.index("--port") + 1])
class Usage(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        ok = self.path == "/api/v1/oauth/usage" and self.headers.get("Authorization") == "Bearer secret-1"
        body = json.dumps({"code": 0, "data": {"kind": "ok",
            "summary": {"window": {"duration": 1, "unit": "week"}, "used": 30, "limit": 100, "reset_at": "2099-01-08T00:00:00Z"},
            "limits": [{"window": {"duration": 5, "unit": "hour"}, "used": 10, "limit": 100, "reset_at": "2099-01-01T05:00:00Z"}]}}
            if ok else {"code": 401}).encode()
        self.send_response(200 if ok else 401)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass
server = http.server.HTTPServer(("127.0.0.1", port), Usage)
print("  Local:    http://127.0.0.1:%d/#token=secret-1" % port, flush=True)
server.serve_forever()
PY
)");
    const auto home = fakes.home.toUtf8();
    const auto shell = (bin + QStringLiteral("/login-shell")).toUtf8();
    add(QStringLiteral("login-shell"), "for last; do :; done\nPATH='" + bin.toUtf8() +
                                           R"(:/usr/bin:/bin' exec /bin/sh -c "$last")"
                                           "\n");
    // Grok is not installed on the remote machine: only these CLIs are there.
    QFile::remove(bin + QStringLiteral("/grok-remote"));
    add(QStringLiteral("ssh"), R"(while [ $# -gt 0 ]; do
  case "$1" in -T) shift ;; -o|-L) shift 2 ;; *) break ;; esac
done
host=$1
shift
[ "$host" = offline ] && exit 255
HOME=')" + home + "' SHELL='" + shell +
                                   R"(' exec /bin/sh -c "$1"
)");
}

// Every signed-in CLI is asked in its own protocol, here and over ssh; OMP's
// accounts join them, an account two sources report shows once, and a CLI
// that is not installed, not signed in, or unreachable is left out or said so.
void asks_every_signed_in_cli() {
    Fakes fakes;
    make_fakes(fakes);
    // devbox: Codex and Claude transcripts of its own; no Grok there.
    const auto today = QDate::currentDate();
    write(fakes.home + QStringLiteral("/.codex/sessions/2026/09/24/rollout-r.jsonl"),
          codex_model(stamp(today), "gpt-6-sol") + codex_total(stamp(today), 5000, 4000, 100));
    write(fakes.home + QStringLiteral("/.claude/projects/p/s.jsonl"),
          claude_line(stamp(today), "msg_r", "req_r", "claude-opus-5-5", 1, 2, 3, 4));
    const auto remote_grok = fakes.programs.value(QStringLiteral("grok"));
    QFile::rename(remote_grok, remote_grok + QStringLiteral(".here"));
    fakes.programs.insert(QStringLiteral("grok"), remote_grok + QStringLiteral(".here"));

    auto programs = fakes.programs;
    Usage usage(
        [&programs](const QString& id) { return programs.value(id); },
        {fakes.dir.filePath(QStringLiteral("none")), fakes.dir.filePath(QStringLiteral("none"))});
    usage.setMachines(
        {QStringLiteral("devbox"), QStringLiteral("offline"), QStringLiteral("-bad")});
    require(usage.machines().size() == 3,
            "the configured machines, without one that looks like an option");
    usage.refresh();
    wait_for(
        [&] {
            return provider_ids(usage).size() == 5 &&
                   provider(usage, "codex", QStringLiteral("devbox"))
                           .value(QStringLiteral("month"))
                           .toMap()
                           .value(QStringLiteral("total"))
                           .toLongLong() > 0 &&
                   !provider(usage, "kimi", QStringLiteral("devbox")).isEmpty() &&
                   !provider(usage, "claude", QStringLiteral("devbox")).isEmpty() &&
                   !machine_of(usage, QStringLiteral("offline"))
                        .value(QStringLiteral("note"))
                        .toString()
                        .isEmpty() &&
                   !usage.counting();
        },
        "every CLI answers here and on devbox; offline says why not");

    require(provider_ids(usage) == QStringList({QStringLiteral("codex"), QStringLiteral("claude"),
                                                QStringLiteral("grok"), QStringLiteral("kimi"),
                                                QStringLiteral("zai")}),
            "this Mac: the signed-in CLIs in the usual order, then OMP's other plans");
    const auto codex = accounts(usage, "codex");
    require(codex.size() == 2 && first_percent(codex) == 40 &&
                codex[1].toMap().value(QStringLiteral("label")).toString() ==
                    QStringLiteral("two@example.com") &&
                codex[1].toMap().value(QStringLiteral("source")).toString() ==
                    QStringLiteral("omp"),
            "Codex: the CLI's sign-in, and OMP's other account; the one both report shows once");
    require(accounts(usage, "kimi").size() == 1 && first_percent(accounts(usage, "kimi")) == 30,
            "Kimi: kimi web's answer, the same account OMP holds shown once");
    require(first_percent(accounts(usage, "grok")) == 100 &&
                first_percent(accounts(usage, "claude")) == 87,
            "Grok and Claude answer");

    QStringList meter;
    for (const auto& row : usage.meter())
        meter << row.toMap().value(QStringLiteral("id")).toString() + QLatin1Char(' ') +
                     QString::number(row.toMap().value(QStringLiteral("percent")).toDouble()) +
                     QLatin1Char(' ') + row.toMap().value(QStringLiteral("label")).toString();
    require(meter == QStringList({QStringLiteral("codex 40 wk"), QStringLiteral("claude 87 5h"),
                                  QStringLiteral("grok 100 wk"), QStringLiteral("kimi 30 wk"),
                                  QStringLiteral("zai 20 5h")}),
            "the meter: each plan at its tightest window");
    usage.setMeterOrder(
        {QStringLiteral("Grok"), QStringLiteral("codex"), QStringLiteral("missing")});
    require(usage.meter().size() == 2 &&
                usage.meter()[0].toMap().value(QStringLiteral("id")).toString() ==
                    QStringLiteral("grok"),
            "the config's meter order, leaving out what is not signed in");

    require(!provider_ids(usage, QStringLiteral("devbox")).contains(QStringLiteral("grok")) &&
                !provider_ids(usage, QStringLiteral("devbox")).contains(QStringLiteral("zai")),
            "devbox: a CLI not installed there is left out, and OMP is asked here only");
    require(first_percent(accounts(usage, "codex", QStringLiteral("devbox"))) == 40,
            "devbox: its CLIs answer over ssh");
    const auto remote = provider(usage, "codex", QStringLiteral("devbox"));
    require(
        remote.value(QStringLiteral("today")).toMap().value(QStringLiteral("total")).toLongLong() ==
                5100 &&
            provider(usage, "claude", QStringLiteral("devbox"))
                    .value(QStringLiteral("today"))
                    .toMap()
                    .value(QStringLiteral("total"))
                    .toLongLong() == 10,
        "devbox: its transcripts counted there by python3");

    const auto recorded = [&](const char* name) {
        QFile file(fakes.dir.filePath(QStringLiteral("%1-args").arg(QLatin1String(name))));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    require(recorded("claude").contains("[--setting-sources]\n[]\n") &&
                recorded("claude").contains("[--no-session-persistence]"),
            "Claude runs without the person's settings and writes no transcript");
    require(recorded("grok").contains("[agent]\n[--no-leader]\n[stdio]"),
            "Grok runs its own agent");
    const auto omp_args = recorded("omp");
    const auto session = omp_args.mid(omp_args.indexOf("[--session-dir=") + 15);
    require(omp_args.contains("[--no-extensions]") && omp_args.endsWith("[acp]\n") &&
                !QFileInfo::exists(QString::fromUtf8(session.left(session.indexOf(']')))),
            "OMP keeps its session in a folder removed afterwards");

    // Signed out of Grok: it leaves the dashboard and the meter.
    write(fakes.programs.value(QStringLiteral("grok")), R"(#!/bin/sh
read -r line
echo '{"jsonrpc":"2.0","id":1,"result":{}}'
read -r line
echo '{"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Not signed in"}}'
cat >/dev/null
)");
    // Codex ends without answering: its last answer stays, with a note.
    const auto broken = fakes.dir.filePath(QStringLiteral("bin/codex-broken"));
    write(broken, "#!/bin/sh\nexit 1\n");
    QFile::setPermissions(broken,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    programs.insert(QStringLiteral("codex"), broken);
    usage.refresh();
    wait_for(
        [&] { return !provider_ids(usage).contains(QStringLiteral("grok")) && !usage.counting(); },
        "a signed-out CLI leaves");
    wait_for(
        [&] {
            return accounts(usage, "codex")
                .front()
                .toMap()
                .value(QStringLiteral("note"))
                .toString()
                .contains(QStringLiteral("without an answer"));
        },
        "a failed check keeps the last answer and says so");
    require(first_percent(accounts(usage, "codex")) == 40, "the last answer is kept");
}

// A window used faster than it lasts says when it runs out: 80% used half way
// through a week runs out in another 21 hours, at 160% by the reset.
void tells_when_a_window_runs_out() {
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    const auto resets = QDateTime::currentDateTime().addSecs(qint64{7} * 24 * 3600 / 2);
    const auto codex = dir.filePath(QStringLiteral("codex"));
    write(codex,
          "#!/bin/sh\nread -r line\necho '{\"id\":1,\"result\":{}}'\nread -r line\nread -r line\n"
          "echo '{\"id\":2,\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":80,"
          "\"windowDurationMins\":10080,\"resetsAt\":" +
              QByteArray::number(resets.toSecsSinceEpoch()) + "}}}}'\ncat >/dev/null\n");
    QFile::setPermissions(codex,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    Usage usage([&](const QString& id) { return id == QLatin1String("codex") ? codex : QString(); },
                {dir.filePath(QStringLiteral("none")), dir.filePath(QStringLiteral("none"))});
    usage.refresh();
    wait_for([&] { return !accounts(usage, "codex").isEmpty() && !usage.counting(); },
             "Codex answers");
    const auto window = accounts(usage, "codex")
                            .front()
                            .toMap()
                            .value(QStringLiteral("windows"))
                            .toList()
                            .front()
                            .toMap();
    const auto runs_out = window.value(QStringLiteral("runsOut")).toDateTime();
    require(std::abs(window.value(QStringLiteral("pace")).toDouble() - 160) < 1,
            "at this pace, 160% by the reset");
    require(runs_out.isValid() &&
                std::abs(QDateTime::currentDateTime().secsTo(runs_out) - qint64{21} * 3600) < 300,
            "at this pace, it runs out in 21 hours");
}

// A month of synthetic transcripts: how fast the first count and a rescan run.
void counts_a_month_quickly() {
    QTemporaryDir home;
    require(home.isValid(), "temporary home");
    const auto today = QDate::currentDate();
    QByteArray tool(3000, 'x');
    qint64 bytes = 0;
    for (int session = 0; session < 40; ++session) {
        QByteArray codex;
        QByteArray claude;
        const auto when = stamp(today.addDays(-(session % 20)));
        codex += codex_model(when, "gpt-6-astra");
        for (int call = 1; call <= 400; ++call) {
            codex +=
                R"({"timestamp":")" + when +
                R"(","type":"response_item","payload":{"type":"function_call_output","output":")" +
                tool +
                R"("}})"
                "\n";
            codex += codex_total(when, call * 1000, call * 800, call * 10);
            const auto id = QByteArray::number(session * 1000 + call);
            const auto line = claude_line(when, "msg_" + id, "req_" + id, "claude-opus-5-5", 1, 2,
                                          3, 4, R"([{"type":"text","text":")" + tool + R"("}])");
            claude += line + line;
        }
        bytes += codex.size() + claude.size();
        write(home.filePath(QStringLiteral("codex/rollout-%1.jsonl").arg(session)), codex);
        write(home.filePath(QStringLiteral("claude/p/%1.jsonl").arg(session)), claude);
    }
    TokenLedger ledger(
        {home.filePath(QStringLiteral("codex")), home.filePath(QStringLiteral("claude"))});
    QElapsedTimer timer;
    timer.start();
    ledger.scan(today.addDays(-29));
    const auto first = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    const auto days = ledger.days();
    qint64 claude_total = 0;
    for (const auto& [day, models] : days.value(QStringLiteral("claude")))
        for (const auto& count : models)
            claude_total += count.total();
    require(claude_total == qint64{40} * 400 * 10, "every synthetic message counted once");
    timer.restart();
    ledger.scan(today.addDays(-29));
    const auto again = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    const auto megabytes = static_cast<double>(bytes) / 1e6;
    std::cout << "usage: first count " << megabytes << " MB in " << first << " ms ("
              << megabytes * 1e3 / std::max(first, 0.001) << " MB/s); rescan with nothing new "
              << again << " ms\n";
    require(again < first, "a rescan costs less than the first count");
}

// Opt-in measurement against a real home (read only): how long the first
// count and a rescan take, and the totals for comparison with other tools.
void measure_home(const QString& home) {
    TokenLedger ledger(
        {home + QStringLiteral("/.codex/sessions"), home + QStringLiteral("/.claude/projects")});
    const auto today = QDate::currentDate();
    const auto since = std::min(QDate(today.year(), today.month(), 1), today.addDays(-29));
    QElapsedTimer timer;
    timer.start();
    ledger.scan(since);
    const auto first = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    const auto read = ledger.bytesRead();
    timer.restart();
    ledger.scan(since);
    const auto again = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    std::cout << "measure: read " << static_cast<double>(read) / 1e9 << " GB in " << first
              << " ms; rescan " << again << " ms\n";
    const auto days = ledger.days();
    for (auto provider = days.cbegin(); provider != days.cend(); ++provider) {
        TokenCount day;
        TokenCount month;
        for (const auto& [date, models] : *provider)
            for (auto model = models.cbegin(); model != models.cend(); ++model) {
                if (date >= QDate(today.year(), today.month(), 1))
                    month += *model;
                if (date == today)
                    day += *model;
                if (date == today)
                    std::cout << "measure: " << provider.key().toStdString() << " today "
                              << model.key().toStdString() << " input " << model->input
                              << " cacheRead " << model->cacheRead << " cacheWrite "
                              << model->cacheWrite << " output " << model->output << '\n';
            }
        std::cout << "measure: " << provider.key().toStdString() << " today " << day.total()
                  << " (input " << day.input << ", cache read " << day.cacheRead << ", cache write "
                  << day.cacheWrite << ", output " << day.output << "); month " << month.total()
                  << '\n';
    }
}

// Opt-in live check against the installed CLIs, here and on the listed ssh
// hosts: what each dashboard shows (accounts by number, not by name).
void show_live(const QString& hosts) {
    Usage usage(
        [](const QString& id) {
            return id == QLatin1String("ssh") ? QStandardPaths::findExecutable(id)
                                              : lapis::desktop::harness_program(id);
        },
        {QDir::home().filePath(QStringLiteral(".codex/sessions")),
         QDir::home().filePath(QStringLiteral(".claude/projects"))});
    usage.setMachines(hosts.split(QLatin1Char(','), Qt::SkipEmptyParts));
    QElapsedTimer timer;
    timer.start();
    usage.refresh();
    // Every check has answered once nothing has changed for three seconds.
    qint64 quiet = 0;
    QObject::connect(&usage, &Usage::changed, [&] { quiet = timer.elapsed(); });
    while (timer.elapsed() < 120000 && (usage.counting() || timer.elapsed() - quiet < 3000)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    std::cout << "live: settled after " << quiet << " ms\n";
    for (const auto& item : usage.machines()) {
        const auto machine = item.toMap();
        std::cout << "== " << machine.value(QStringLiteral("name")).toString().toStdString() << ' '
                  << machine.value(QStringLiteral("note")).toString().toStdString() << '\n';
        for (const auto& entry : machine.value(QStringLiteral("providers")).toList()) {
            const auto provider = entry.toMap();
            std::cout << "  " << provider.value(QStringLiteral("name")).toString().toStdString()
                      << ": tokens today "
                      << provider.value(QStringLiteral("today"))
                             .toMap()
                             .value(QStringLiteral("total"))
                             .toLongLong()
                      << ", month "
                      << provider.value(QStringLiteral("month"))
                             .toMap()
                             .value(QStringLiteral("total"))
                             .toLongLong()
                      << '\n';
            int number = 0;
            for (const auto& value : provider.value(QStringLiteral("accounts")).toList()) {
                const auto account = value.toMap();
                std::cout << "    account " << ++number << " ("
                          << account.value(QStringLiteral("source")).toString().toStdString() << ' '
                          << account.value(QStringLiteral("plan")).toString().toStdString() << ") "
                          << account.value(QStringLiteral("note")).toString().toStdString() << ':';
                for (const auto& window : account.value(QStringLiteral("windows")).toList())
                    std::cout
                        << ' '
                        << window.toMap().value(QStringLiteral("label")).toString().toStdString()
                        << ' ' << window.toMap().value(QStringLiteral("percent")).toDouble()
                        << "% resets "
                        << window.toMap()
                               .value(QStringLiteral("resets"))
                               .toDateTime()
                               .toString(QStringLiteral("ddd h:mm ap"))
                               .toStdString()
                        << ';';
                std::cout << '\n';
            }
        }
    }
    std::cout << "meter:";
    for (const auto& row : usage.meter())
        std::cout << ' ' << row.toMap().value(QStringLiteral("name")).toString().toStdString()
                  << ' ' << row.toMap().value(QStringLiteral("percent")).toDouble() << "% "
                  << row.toMap().value(QStringLiteral("label")).toString().toStdString() << ';';
    std::cout << '\n';
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (qEnvironmentVariableIsSet("LAPIS_USAGE_LIVE")) {
            show_live(qEnvironmentVariable("LAPIS_USAGE_LIVE"));
            return 0;
        }
        if (const auto home = qEnvironmentVariable("LAPIS_USAGE_MEASURE_HOME"); !home.isEmpty()) {
            measure_home(home);
            return 0;
        }
        counts_tokens_from_transcripts();
        reads_plan_limits();
        asks_every_signed_in_cli();
        tells_when_a_window_runs_out();
        counts_a_month_quickly();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "usage tests passed\n";
    return 0;
}
