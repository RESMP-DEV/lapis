#include "tool_status.hpp"

#include "harness_catalog.hpp"

#include <QFileInfo>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace lapis::desktop {
namespace {

QString firstLine(QByteArray output) {
    while (!output.isEmpty()) {
        const auto end = output.indexOf('\n');
        const auto line = output.left(end < 0 ? output.size() : end).trimmed();
        if (!line.isEmpty())
            return QString::fromUtf8(line).left(ToolStatus::kLongestDetail);
        if (end < 0)
            break;
        output.remove(0, end + 1);
    }
    return {};
}

ToolStatus::Probe defaultProbe(const QString& id) {
    return {.argv = {id, QStringLiteral("--version")}, .timeoutMs = 3000, .expectedExit = 0};
}

} // namespace

ToolStatus::ToolStatus(Programs programs, QObject* parent)
    : QObject(parent), programs_(programs ? std::move(programs) : Programs{&harness_program}) {
    stale_check_.setInterval(1000);
    connect(&stale_check_, &QTimer::timeout, this, [this] { publishStaleChanges(); });
    stale_check_.start();
}

std::optional<ToolStatus::Probe> ToolStatus::probeFor(const QString& id) const {
    const auto configured = test_probes_.constFind(id);
    if (configured != test_probes_.constEnd())
        return configured->argv.isEmpty() ? std::nullopt : std::optional(configured.value());
    return defaultProbe(id);
}

void ToolStatus::setProbeForTesting(const QString& id, std::optional<Probe> probe) {
    if (probe)
        test_probes_.insert(id, std::move(*probe));
    else
        test_probes_.remove(id);
    refreshTool(id);
}

void ToolStatus::refresh() {
    for (const auto& harness : harnesses()) {
        const auto found = results_.constFind(harness.id);
        if (found == results_.constEnd() || !fresh(found.value()))
            queue(harness.id);
    }
}

void ToolStatus::refreshTool(const QString& id) {
    if (find_harness(id) != nullptr)
        queue(id);
}

void ToolStatus::queue(const QString& id) {
    if (!pending_.contains(id))
        pending_.append(id);
    startNext();
}

QDateTime ToolStatus::now() const { return now_.isValid() ? now_ : QDateTime::currentDateTime(); }

bool ToolStatus::fresh(const Result& result) const {
    return result.checked.isValid() && now() < result.checked.addMSecs(kFreshMs);
}

void ToolStatus::startNext() {
    while (running_.size() < kProbesAtOnce && !pending_.isEmpty()) {
        const auto id = pending_.takeFirst();
        const auto* harness = find_harness(id);
        if (harness == nullptr || std::any_of(running_.cbegin(), running_.cend(),
                                              [&](const Running& job) { return job.id == id; }))
            continue;

        const auto resolved = programs_(id);
        if (resolved.isEmpty()) {
            note(id, Result{.resolved = QString(),
                            .state = QStringLiteral("missing"),
                            .exitCode = 0,
                            .detail = QString(),
                            .checked = now()});
            continue;
        }
        const auto probe = probeFor(id);
        if (!probe) {
            note(id, Result{.resolved = resolved,
                            .state = QStringLiteral("present"),
                            .exitCode = 0,
                            .detail = QString(),
                            .checked = now()});
            continue;
        }

        // A probe names the catalog id, so it uses the same resolved program as
        // launch. Tests may name a fixture executable directly.
        const auto& command = probe->argv;
        const auto direct = command.first() != harness->id &&
                            command.first() != QFileInfo(harness->command).fileName();
        const auto program = direct ? command.first() : resolved;
        if (!QFileInfo(program).isExecutable()) {
            note(id, Result{.resolved = QString(),
                            .state = QStringLiteral("missing"),
                            .exitCode = 0,
                            .detail = QString(),
                            .checked = now()});
            continue;
        }

        auto& job = running_.emplace_back();
        job.id = id;
        job.process = new QProcess(this);
        job.process->setProcessChannelMode(QProcess::MergedChannels);
        job.timeout = new QTimer(job.process);
        job.timeout->setSingleShot(true);
        connect(job.timeout, &QTimer::timeout, job.process, [this, process = job.process.data()] {
            const auto found =
                std::find_if(running_.begin(), running_.end(),
                             [&](const Running& job) { return job.process == process; });
            if (found == running_.end())
                return;
            found->timedOut = true;
            process->kill();
        });
        connect(job.process, &QProcess::errorOccurred, job.process,
                [this, process = job.process.data()](QProcess::ProcessError) {
                    const auto found =
                        std::find_if(running_.begin(), running_.end(),
                                     [&](const Running& job) { return job.process == process; });
                    if (found == running_.end() || found->timedOut)
                        return;
                    note(found->id, Result{.resolved = process->program(),
                                           .state = QStringLiteral("failed"),
                                           .detail = process->errorString().left(kLongestDetail),
                                           .checked = now()});
                    process->deleteLater();
                    running_.erase(found);
                    startNext();
                });
        connect(job.process, &QProcess::finished, this,
                [this, process = job.process.data()] { finished(process); });
        connect(job.process, &QProcess::readyReadStandardOutput, job.process,
                [this, process = job.process.data()] { consumeOutput(process); });
        job.process->start(program, command.mid(1));
        job.timeout->start(probe->timeoutMs);
    }
}

void ToolStatus::finished(QProcess* process) {
    const auto found = std::find_if(running_.begin(), running_.end(),
                                    [&](const Running& job) { return job.process == process; });
    if (found == running_.end())
        return;
    const auto id = found->id;
    const auto timed_out = found->timedOut;
    const auto exit_code = process->exitCode();
    consumeOutput(process);
    auto output = found->output;
    if (output.size() > kLongestOutput)
        output.truncate(kLongestOutput);
    Result result{.resolved = process->program(),
                  .state = QString(),
                  .exitCode = exit_code,
                  .detail = QString(),
                  .checked = now()};
    if (timed_out)
        result.state = QStringLiteral("timeout");
    else if (process->exitStatus() != QProcess::NormalExit)
        result.state = QStringLiteral("failed");
    else {
        const auto probe = probeFor(id);
        const auto expected = probe ? probe->expectedExit : 0;
        result.state = exit_code == expected ? QStringLiteral("ok") : QStringLiteral("failed");
    }
    result.detail = firstLine(std::move(output));
    process->deleteLater();
    running_.erase(found);
    note(id, std::move(result));
    startNext();
}

void ToolStatus::consumeOutput(QProcess* process) {
    const auto found = std::find_if(running_.begin(), running_.end(),
                                    [&](const Running& job) { return job.process == process; });
    if (found == running_.end())
        return;
    auto bytes = process->readAllStandardOutput();
    if (found->output.size() >= kLongestOutput) {
        const auto room = kLongestOutput - found->output.size();
        if (room > 0)
            found->output.append(bytes.left(room));
    } else {
        found->output.append(bytes);
    }
    if (found->output.size() > kLongestOutput)
        found->output.truncate(kLongestOutput);
}

void ToolStatus::note(const QString& id, Result result) {
    fresh_noted_.insert(id, fresh(result));
    results_.insert(id, std::move(result));
    emit changed();
}

void ToolStatus::publishStaleChanges() {
    bool changed = false;
    for (auto entry = results_.cbegin(); entry != results_.cend(); ++entry) {
        if (fresh_noted_.value(entry.key()) && !fresh(entry.value())) {
            fresh_noted_.insert(entry.key(), false);
            changed = true;
        }
    }
    if (changed)
        emit this->changed();
}

void ToolStatus::publishForTesting() {
    publishStaleChanges();
    emit changed();
}

qsizetype ToolStatus::bufferedOutputForTesting(const QString& id) const {
    const auto found = std::find_if(running_.cbegin(), running_.cend(),
                                    [&](const Running& job) { return job.id == id; });
    return found == running_.cend() ? qsizetype{0} : found->output.size();
}

QVariantList ToolStatus::rows() const {
    QVariantList rows;
    for (const auto& harness : harnesses()) {
        const auto found = results_.constFind(harness.id);
        const auto& result = found == results_.constEnd() ? Result{} : found.value();
        rows.append(QVariantMap{
            {QStringLiteral("id"), harness.id},
            {QStringLiteral("label"), harness.label},
            {QStringLiteral("command"), harness.command},
            {QStringLiteral("adapter"),
             harness.adapter == HarnessAdapter::codex    ? QStringLiteral("codex")
             : harness.adapter == HarnessAdapter::claude ? QStringLiteral("claude")
                                                         : QStringLiteral("terminal")},
            {QStringLiteral("update"), harness.updateCommand},
            {QStringLiteral("resumeOption"), harness.resumeOption},
            {QStringLiteral("offered"), harness.offered},
            {QStringLiteral("resolved"), result.resolved},
            {QStringLiteral("state"), result.state},
            {QStringLiteral("exitCode"), result.exitCode},
            {QStringLiteral("detail"), result.detail},
            {QStringLiteral("checked"),
             result.checked.isValid() ? result.checked.toString(Qt::ISODateWithMs) : QString()},
            {QStringLiteral("fresh"), found != results_.constEnd() && fresh(result)},
            {QStringLiteral("probed"), probeFor(harness.id).has_value()}});
    }
    return rows;
}

} // namespace lapis::desktop
