#include "usage.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
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
}

QString script(const QTemporaryDir& dir, const QString& name, const QByteArray& body) {
    const auto path = dir.filePath(name);
    write(path, "#!/bin/sh\n" + body);
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
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

QVariantMap provider(const Usage& usage, const char* id) {
    for (const auto& item : usage.providers())
        if (item.toMap().value(QStringLiteral("id")).toString() == QLatin1String(id))
            return item.toMap();
    return {};
}

// Each CLI is asked in its own protocol and then stopped; the person's
// settings, hooks and transcripts are left alone; a CLI that ends without an
// answer keeps the last one and says so.
void asks_each_cli() {
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    const auto args = dir.filePath(QStringLiteral("claude-args"));
    const auto codex = script(dir, QStringLiteral("codex"), R"(
read -r line
echo '{"method":"remoteControl/status/changed","params":{}}'
echo '{"id":1,"result":{}}'
read -r line
read -r line
case "$line" in *account/rateLimits/read*) ;; *) exit 3 ;; esac
echo '{"id":2,"result":{"rateLimits":{"limitId":"codex","primary":{"usedPercent":40,"windowDurationMins":10080,"resetsAt":1790558544},"planType":"pro"}}}'
cat >/dev/null
)");
    const auto claude = script(dir, QStringLiteral("claude"),
                               R"(
for a in "$@"; do printf '[%s]\n' "$a"; done > ")" +
                                   args.toUtf8() + R"("
read -r line
case "$line" in *get_usage*) ;; *) exit 3 ;; esac
echo '{"type":"system","subtype":"init"}'
echo '{"type":"control_response","response":{"subtype":"success","request_id":"usage","response":{"subscription_type":"max","rate_limits_available":true,"rate_limits":{"five_hour":{"utilization":87,"resets_at":"2026-09-25T03:09:59+00:00"}}}}}'
cat >/dev/null
)");
    QString codex_program = codex;
    Usage usage(
        [&](const QString& id) {
            return id == QLatin1String("codex")
                       ? codex_program
                       : (id == QLatin1String("claude") ? claude : QString());
        },
        {dir.filePath(QStringLiteral("none")), dir.filePath(QStringLiteral("none"))});
    usage.refresh();
    wait_for(
        [&] {
            return !provider(usage, "codex").value(QStringLiteral("windows")).toList().isEmpty() &&
                   !provider(usage, "claude").value(QStringLiteral("windows")).toList().isEmpty() &&
                   !usage.counting();
        },
        "both CLIs answer");
    const auto week =
        provider(usage, "codex").value(QStringLiteral("windows")).toList().front().toMap();
    require(week.value(QStringLiteral("label")).toString() == QStringLiteral("Week") &&
                week.value(QStringLiteral("percent")).toDouble() == 40,
            "the Codex answer is shown");
    const auto session =
        provider(usage, "claude").value(QStringLiteral("windows")).toList().front().toMap();
    require(session.value(QStringLiteral("percent")).toDouble() == 87 &&
                provider(usage, "claude").value(QStringLiteral("plan")).toString() ==
                    QStringLiteral("max"),
            "the Claude answer is shown");
    QFile recorded(args);
    require(recorded.open(QIODevice::ReadOnly), "Claude's arguments were recorded");
    const auto words = recorded.readAll();
    require(words.contains("[--setting-sources]\n[]\n") &&
                words.contains("[--no-session-persistence]") && words.contains("[-p]"),
            "Claude runs without the person's settings and writes no transcript");

    // A CLI that ends without answering: the last answer stays, with a note.
    codex_program = script(dir, QStringLiteral("codex-broken"), "exit 1\n");
    usage.refresh();
    wait_for(
        [&] {
            return provider(usage, "codex")
                .value(QStringLiteral("note"))
                .toString()
                .contains(QStringLiteral("without an answer"));
        },
        "a failed check is reported");
    require(!provider(usage, "codex").value(QStringLiteral("windows")).toList().isEmpty(),
            "the last answer is kept");
    codex_program.clear();
    usage.refresh();
    wait_for([&] { return provider(usage, "codex").isEmpty(); },
             "an uninstalled CLI with no transcripts is not shown");
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
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (const auto home = qEnvironmentVariable("LAPIS_USAGE_MEASURE_HOME"); !home.isEmpty()) {
            measure_home(home);
            return 0;
        }
        counts_tokens_from_transcripts();
        reads_plan_limits();
        asks_each_cli();
        counts_a_month_quickly();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "usage tests passed\n";
    return 0;
}
