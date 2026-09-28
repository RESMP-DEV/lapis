#ifndef LAPIS_DESKTOP_HARNESS_CATALOG_HPP
#define LAPIS_DESKTOP_HARNESS_CATALOG_HPP

#include <QString>
#include <QStringList>

#include <vector>

namespace lapis::desktop {

// How lapis observes a CLI. This is the launch-transport choice, not evidence
// that the observer has produced activity or approval capability.
enum class HarnessAdapter : unsigned char { codex, claude, terminal };

// The argument shape accepted by the CLIs that expose a model choice.
enum class HarnessModelOption : unsigned char { unsupported, shortOption, longOption, assignment };

// One CLI lapis knows. The catalog order is the new-agent picker order; a
// retired CLI stays known so saved agents still restore.
struct HarnessDescriptor {
    QString id;
    QString label;
    QString command;
    // The CLI's own non-interactive update command, if lapis runs it. Codex
    // has none: its observer accepts only qualified binaries, so lapis keeps
    // the qualified build and turns off Codex's update prompt instead.
    QString updateCommand;
    bool offered{};
    HarnessAdapter adapter{};
    HarnessModelOption modelOption{};
    QString resumeOption;

    // Arguments lapis always gives a new agent of this CLI, before the
    // person's own arguments.
    [[nodiscard]] QStringList defaultArguments() const;
    // The CLI's model flag with a model name; empty for a default model or a
    // CLI that has no model option in this catalog.
    [[nodiscard]] QStringList modelArguments(const QString& model) const;
};

// Every known CLI in the pickers' order, including retired CLIs.
[[nodiscard]] const std::vector<HarnessDescriptor>& harnesses();
// A known descriptor, or null for an unknown external id.
[[nodiscard]] const HarnessDescriptor* find_harness(const QString& id);
// The installed program of a known CLI, or empty when it is not found on this
// Mac or the id is unknown.
[[nodiscard]] QString harness_program(const QString& id);
// A known CLI's name as people call it ("Claude", "Codex"), or an unknown id
// itself.
[[nodiscard]] QString harness_label(const QString& id);

} // namespace lapis::desktop

#endif
