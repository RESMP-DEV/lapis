#include "harness_catalog.hpp"

#include <QDir>
#include <QStandardPaths>

#include <algorithm>

namespace lapis::desktop {
namespace {

QString harnessExecutable(const HarnessDescriptor& harness) {
    auto paths = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const auto* suffix :
         {"/.local/bin", "/.bun/bin", "/.grok/bin", "/.kimi-code/bin", "/.opencode/bin", "/bin"})
        paths.append(QDir::homePath() + QLatin1String(suffix));
    paths << QStringLiteral("/opt/homebrew/bin") << QStringLiteral("/usr/local/bin");
    return QStandardPaths::findExecutable(harness.command, paths);
}
} // namespace

QStringList HarnessDescriptor::defaultArguments() const {
    if (id == QLatin1String("codex"))
        return {QStringLiteral("-c"), QStringLiteral("check_for_update_on_startup=false")};
    return {};
}

QStringList HarnessDescriptor::modelArguments(const QString& model) const {
    if (model.isEmpty() || modelOption == HarnessModelOption::unsupported)
        return {};
    switch (modelOption) {
    case HarnessModelOption::shortOption:
        return {QStringLiteral("-m"), model};
    case HarnessModelOption::longOption:
        return {QStringLiteral("--model"), model};
    case HarnessModelOption::assignment:
        return {QStringLiteral("--model=") + model};
    case HarnessModelOption::unsupported:
        break;
    }
    return {};
}

const std::vector<HarnessDescriptor>& harnesses() {
    // In the order the pickers offer them.
    static const std::vector<HarnessDescriptor> catalog{
        {.id = QStringLiteral("claude"),
         .label = QStringLiteral("Claude"),
         .command = QStringLiteral("claude"),
         .updateCommand = QStringLiteral("update"),
         .offered = true,
         .adapter = HarnessAdapter::claude,
         .modelOption = HarnessModelOption::longOption,
         .resumeOption = QStringLiteral("--resume")},
        {.id = QStringLiteral("codex"),
         .label = QStringLiteral("Codex"),
         .command = QStringLiteral("codex"),
         .updateCommand = {},
         .offered = true,
         .adapter = HarnessAdapter::codex,
         .modelOption = HarnessModelOption::shortOption,
         .resumeOption = QStringLiteral("resume")},
        {.id = QStringLiteral("opencode"),
         .label = QStringLiteral("OpenCode"),
         .command = QStringLiteral("opencode"),
         .updateCommand = QStringLiteral("upgrade"),
         .offered = true,
         .adapter = HarnessAdapter::terminal,
         .modelOption = HarnessModelOption::shortOption,
         .resumeOption = QStringLiteral("--session")},
        {.id = QStringLiteral("grok"),
         .label = QStringLiteral("Grok"),
         .command = QStringLiteral("grok"),
         .updateCommand = QStringLiteral("update"),
         .offered = true,
         .adapter = HarnessAdapter::terminal,
         .modelOption = HarnessModelOption::shortOption,
         .resumeOption = QStringLiteral("-r")},
        {.id = QStringLiteral("omp"),
         .label = QStringLiteral("OMP"),
         .command = QStringLiteral("omp"),
         .updateCommand = QStringLiteral("update"),
         .offered = true,
         .adapter = HarnessAdapter::terminal,
         .modelOption = HarnessModelOption::assignment,
         .resumeOption = QStringLiteral("--resume")},
        {.id = QStringLiteral("agy"),
         .label = QStringLiteral("Antigravity"),
         .command = QStringLiteral("agy"),
         .updateCommand = QStringLiteral("update"),
         .offered = true,
         .adapter = HarnessAdapter::terminal,
         .modelOption = HarnessModelOption::longOption,
         .resumeOption = QStringLiteral("--conversation")},
        {.id = QStringLiteral("kimi"),
         .label = QStringLiteral("Kimi"),
         .command = QStringLiteral("kimi"),
         .updateCommand = QStringLiteral("upgrade"),
         .offered = true,
         .adapter = HarnessAdapter::terminal,
         .modelOption = HarnessModelOption::shortOption,
         .resumeOption = QStringLiteral("--session")},
        {.id = QStringLiteral("gemini"),
         .label = QStringLiteral("Gemini"),
         .command = QStringLiteral("gemini"),
         .updateCommand = {},
         .offered = false,
         .adapter = HarnessAdapter::terminal,
         .modelOption = HarnessModelOption::unsupported,
         .resumeOption = QString()},
    };

    return catalog;
}

const HarnessDescriptor* find_harness(const QString& id) {
    const auto& harness_catalog = harnesses();
    const auto found =
        std::find_if(harness_catalog.begin(), harness_catalog.end(),
                     [&](const HarnessDescriptor& harness) { return harness.id == id; });
    return found == harness_catalog.end() ? nullptr : &*found;
}

QString harness_program(const QString& id) {
    const auto* harness = find_harness(id);
    return harness ? harnessExecutable(*harness) : QString();
}

QString harness_label(const QString& id) {
    const auto* harness = find_harness(id);
    return harness ? harness->label : id;
}
} // namespace lapis::desktop
