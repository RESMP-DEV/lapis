#ifndef LAPIS_DESKTOP_AGENT_SEARCH_HPP
#define LAPIS_DESKTOP_AGENT_SEARCH_HPP
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <array>
#include <vector>

namespace lapis::desktop {
class Workspace;

// What one agent can be found by.
struct AgentSearchEntry {
    QString id;
    QString title;
    QString place; // folder, as the card shows it
    QString category;
    QString harness;
    QString machine;   // ssh host, or empty
    QStringList lines; // its screen's non-empty rows
    QString harnessId; // for its mark
};

struct AgentSearchHit {
    QString id;
    int score{};
    QString snippet; // the screen line that matched, when one did
};

// Cmd+K: find an agent as you type. Every word of the query must match: the
// agent's name, folder, category, CLI or machine by its letters in order
// (word starts and runs of letters count most), or a line on its screen as
// written. The index is rebuilt when the search opens and each query only
// reads it, so typing never waits on the terminals.
class AgentSearch final : public QObject {
    Q_OBJECT
  public:
    explicit AgentSearch(Workspace* workspace = nullptr, QObject* parent = nullptr);

    // Reads every agent's name, place and current screen.
    Q_INVOKABLE void refresh();
    // Best first: {sessionId, title, place, category, harness, machine, snippet}.
    Q_INVOKABLE [[nodiscard]] QVariantList search(const QString& query, int limit = 40) const;

    void setEntries(const std::vector<AgentSearchEntry>& entries);
    [[nodiscard]] std::vector<AgentSearchHit> rank(const QString& query, int limit) const;
    [[nodiscard]] std::size_t size() const { return prepared_.size(); }

  private:
    struct Prepared {
        AgentSearchEntry entry;
        std::array<QString, 5> fields; // lowercased: title, place, category, harness, machine
        QStringList lines;             // lowercased screen rows
    };
    // One query word's best score on an agent, or -1; the matching screen
    // line's snippet goes to `found` when it is still empty.
    static int score(const Prepared& prepared, const QString& term, QString& found);
    Workspace* workspace_;
    std::vector<Prepared> prepared_;
};
} // namespace lapis::desktop
#endif
