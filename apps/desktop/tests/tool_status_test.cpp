// The tool status layer reads the canonical harness catalog, resolves through
// the same program lookup as launch, and reports only bounded probe answers or
// explicit resolution. Fixture executables keep CLI release behavior out of the
// tests.
#include "tool_status.hpp"

#include "harness_catalog.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QThread>
#include <QVariantMap>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using lapis::desktop::ToolStatus;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool settle(const std::function<bool()>& done, int milliseconds = 8000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

QVariantMap rowFor(const ToolStatus& status, const QString& id);

QVariantMap rowFor(const std::unique_ptr<ToolStatus>& status, const QString& id) {
    return rowFor(*status, id);
}

QVariantMap rowFor(const ToolStatus& status, const QString& id) {
    for (const auto& value : status.rows()) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("id")).toString() == id)
            return row;
    }
    return {};
}

QString fixture(const QString& id) {
    static const QHash<QString, QString> programs{
        {QStringLiteral("claude"), QStringLiteral("/usr/bin/true")},
        {QStringLiteral("codex"), QStringLiteral("/usr/bin/false")},
        {QStringLiteral("opencode"), QStringLiteral("/usr/bin/printf")},
        {QStringLiteral("grok"), QStringLiteral("/bin/sleep")},
        {QStringLiteral("omp"), QStringLiteral("/usr/bin/true")},
        {QStringLiteral("agy"), QString()},
        {QStringLiteral("kimi"), QStringLiteral("/usr/bin/true")},
        {QStringLiteral("gemini"), QStringLiteral("/usr/bin/true")},
    };
    return programs.value(id);
}

std::unique_ptr<ToolStatus> fixtureStatus() {
    auto status = std::make_unique<ToolStatus>(&fixture);
    status->setProbeForTesting(
        QStringLiteral("claude"),
        ToolStatus::Probe{.argv = {QStringLiteral("/usr/bin/true")}, .timeoutMs = 1000});
    status->setProbeForTesting(QStringLiteral("codex"),
                               ToolStatus::Probe{.argv = {QStringLiteral("/usr/bin/false")},
                                                 .timeoutMs = 1000,
                                                 .expectedExit = 1});
    status->setProbeForTesting(
        QStringLiteral("opencode"),
        ToolStatus::Probe{.argv = {QStringLiteral("/usr/bin/printf"), QStringLiteral("%s\n%s\n"),
                                   QString(), QStringLiteral("OpenCode answer")},
                          .timeoutMs = 1000});
    status->setProbeForTesting(
        QStringLiteral("grok"),
        ToolStatus::Probe{.argv = {QStringLiteral("/bin/sleep"), QStringLiteral("30")},
                          .timeoutMs = 150});
    status->setProbeForTesting(QStringLiteral("omp"), ToolStatus::Probe{});
    return status;
}

void catalog_is_the_only_row_source() {
    ToolStatus status(&fixture);
    const auto rows = status.rows();
    const auto& catalog = lapis::desktop::harnesses();
    require(rows.size() == static_cast<qsizetype>(catalog.size()),
            "status has exactly one row per catalog harness");
    for (qsizetype index = 0; index < rows.size(); ++index) {
        const auto row = rows.at(index).toMap();
        const auto& harness = catalog.at(static_cast<std::size_t>(index));
        require(row.value(QStringLiteral("id")).toString() == harness.id &&
                    row.value(QStringLiteral("label")).toString() == harness.label &&
                    row.value(QStringLiteral("command")).toString() == harness.command &&
                    row.value(QStringLiteral("offered")).toBool() == harness.offered,
                "row identity and launch metadata come from the catalog");
        require(row.value(QStringLiteral("state")).toString() == QStringLiteral("unknown") &&
                    row.value(QStringLiteral("checked")).toString().isEmpty() &&
                    !row.value(QStringLiteral("fresh")).toBool(),
                "an unchecked row says unknown and has no timestamp");
    }
    require(rowFor(status, QStringLiteral("not-a-tool")).isEmpty(),
            "unknown ids have no status row");
}

void probes_report_their_own_answers() {
    const auto status = fixtureStatus();
    status->refresh();
    const auto answered = [&status] {
        for (const auto& value : status->rows())
            if (value.toMap().value(QStringLiteral("state")).toString() ==
                QStringLiteral("unknown"))
                return false;
        return true;
    };
    require(settle(answered), "every catalog harness answered");

    const auto ok = rowFor(status, QStringLiteral("claude"));
    require(ok.value(QStringLiteral("state")).toString() == QStringLiteral("ok") &&
                ok.value(QStringLiteral("resolved")).toString() ==
                    QStringLiteral("/usr/bin/true") &&
                ok.value(QStringLiteral("probed")).toBool() &&
                ok.value(QStringLiteral("fresh")).toBool(),
            "an expected zero exit reads ok");
    const auto accepted = rowFor(status, QStringLiteral("codex"));
    require(accepted.value(QStringLiteral("state")).toString() == QStringLiteral("ok") &&
                accepted.value(QStringLiteral("exitCode")).toInt() == 1,
            "the configured expected nonzero exit reads ok");
    const auto talker = rowFor(status, QStringLiteral("opencode"));
    require(talker.value(QStringLiteral("state")).toString() == QStringLiteral("ok") &&
                talker.value(QStringLiteral("detail")).toString() ==
                    QStringLiteral("OpenCode answer"),
            "the first nonblank output line is the detail");
    const auto slow = rowFor(status, QStringLiteral("grok"));
    require(slow.value(QStringLiteral("state")).toString() == QStringLiteral("timeout"),
            "a probe past its deadline reads timeout");
    const auto unprobed = rowFor(status, QStringLiteral("omp"));
    require(unprobed.value(QStringLiteral("state")).toString() == QStringLiteral("present") &&
                !unprobed.value(QStringLiteral("probed")).toBool() &&
                unprobed.value(QStringLiteral("resolved")).toString() ==
                    QStringLiteral("/usr/bin/true"),
            "an unprobed installed harness says present");
    const auto missing = rowFor(status, QStringLiteral("agy"));
    require(missing.value(QStringLiteral("state")).toString() == QStringLiteral("missing"),
            "an unresolved harness says missing without running a probe");
}

void answers_expire_to_stale_without_rewording() {
    const auto status = fixtureStatus();
    status->refresh();
    require(settle([&status] {
                return rowFor(status, QStringLiteral("claude"))
                           .value(QStringLiteral("state"))
                           .toString() == QStringLiteral("ok");
            }),
            "the probe answered");
    const auto before = rowFor(status, QStringLiteral("claude"));
    const auto checked = QDateTime::fromString(before.value(QStringLiteral("checked")).toString(),
                                               Qt::ISODateWithMs);
    require(checked.isValid(), "a fresh answer has a valid timestamp");
    status->setNowForTesting(checked.addMSecs(ToolStatus::kFreshMs + 1000));
    require(settle([&status] {
                return !rowFor(status, QStringLiteral("claude"))
                            .value(QStringLiteral("fresh"))
                            .toBool();
            }),
            "the production stale scan republishes currency changes");
    const auto after = rowFor(status, QStringLiteral("claude"));
    require(!after.value(QStringLiteral("fresh")).toBool() &&
                after.value(QStringLiteral("state")).toString() == QStringLiteral("ok") &&
                after.value(QStringLiteral("checked")) == before.value(QStringLiteral("checked")),
            "age changes only currency, not the recorded answer");
}

void noisy_probe_output_is_bounded_while_running() {
    const auto status = fixtureStatus();
    const auto noisy =
        QStringLiteral("printf '%%1s' ''; sleep 0.15").arg(ToolStatus::kLongestOutput + 1);
    status->setProbeForTesting(QStringLiteral("claude"),
                               ToolStatus::Probe{
                                   .argv = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), noisy},
                                   .timeoutMs = 2000,
                               });
    status->refreshTool(QStringLiteral("claude"));
    require(settle([&status] {
                return status->bufferedOutputForTesting(QStringLiteral("claude")) ==
                       ToolStatus::kLongestOutput;
            }),
            "probe output is drained and truncated before exit");
    require(settle([&status] {
                return rowFor(status, QStringLiteral("claude"))
                           .value(QStringLiteral("state"))
                           .toString() == QStringLiteral("ok");
            }),
            "the bounded probe still records its result");
}

void one_harness_can_be_asked_again() {
    const auto status = fixtureStatus();
    status->refresh();
    require(settle([&status] {
                return rowFor(status, QStringLiteral("kimi"))
                           .value(QStringLiteral("checked"))
                           .toString()
                           .isEmpty() == false;
            }),
            "the single fixture answered");
    status->setProbeForTesting(
        QStringLiteral("kimi"),
        ToolStatus::Probe{.argv = {QStringLiteral("/bin/sleep"), QStringLiteral("0.02")},
                          .timeoutMs = 1000});
    const auto before = rowFor(status, QStringLiteral("kimi"));
    status->refreshTool(QStringLiteral("kimi"));
    require(settle([&status, before] {
                return rowFor(status, QStringLiteral("kimi")).value(QStringLiteral("checked")) !=
                       before.value(QStringLiteral("checked"));
            }),
            "the rerun recorded a new answer time");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        catalog_is_the_only_row_source();
        probes_report_their_own_answers();
        answers_expire_to_stale_without_rewording();
        noisy_probe_output_is_bounded_while_running();
        one_harness_can_be_asked_again();
    } catch (const std::exception& error) {
        std::cerr << "tool_status_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "tool_status_test: all cases passed\n";
    return 0;
}
