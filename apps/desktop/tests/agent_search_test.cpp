#include "agent_search.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using lapis::desktop::AgentSearch;
using lapis::desktop::AgentSearchEntry;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QString first(const AgentSearch& search, const QString& query) {
    const auto hits = search.rank(query, 5);
    return hits.empty() ? QString() : hits.front().id;
}

// Names, folders, categories, CLIs and machines match by their letters in
// order; screens match as written, with the line shown; every word counts.
void finds_agents_by_what_people_remember() {
    AgentSearch search;
    search.setEntries({
        {"a",
         "lapis",
         "~/dev/lapis",
         "Build",
         "Codex",
         "",
         {"› run the tests", "12 passed"},
         "codex"},
        {"b",
         "infinity",
         "devbox:~/dev/infinity",
         "Later",
         "Claude",
         "devbox",
         {"error: undefined symbol _ZN5lapis4mainEv", "linker command failed"},
         "claude"},
        {"c", "notes", "~/notes", "Build", "Grok", "", {"shopping list"}, "grok"},
    });
    require(first(search, QStringLiteral("lap")) == QStringLiteral("a"), "a name prefix");
    require(first(search, QStringLiteral("dvinf")) == QStringLiteral("b"),
            "folder letters in order");
    require(first(search, QStringLiteral("devbox")) == QStringLiteral("b"), "the machine");
    require(first(search, QStringLiteral("grok")) == QStringLiteral("c"), "the CLI");
    const auto screen = search.rank(QStringLiteral("undefined symbol"), 5);
    require(screen.size() == 1 && screen.front().id == QStringLiteral("b") &&
                screen.front().snippet.contains(QStringLiteral("undefined symbol")),
            "a line on the screen, shown with the result");
    require(search.rank(QStringLiteral("codex build"), 5).size() == 1, "every word must match");
    require(search.rank(QStringLiteral("zzzq"), 5).empty(), "nothing matches nothing");
    require(search.rank(QString(), 5).size() == 3, "an empty query lists every agent");
    require(search.rank(QStringLiteral("udsm"), 5).empty(),
            "screens do not match scattered letters, which would match almost anything");
}

// Timing on a large workspace: 128 agents with full screens.
void stays_quick_with_many_agents() {
    std::vector<AgentSearchEntry> entries;
    std::uint32_t seed = 7;
    const auto next = [&seed] {
        seed = seed * 1664525U + 1013904223U;
        return seed;
    };
    const QStringList words{
        QStringLiteral("build"),  QStringLiteral("error"),  QStringLiteral("tests"),
        QStringLiteral("kernel"), QStringLiteral("cuda"),   QStringLiteral("merge"),
        QStringLiteral("agent"),  QStringLiteral("review"), QStringLiteral("deploy"),
        QStringLiteral("render"), QStringLiteral("socket"), QStringLiteral("queue")};
    for (int agent = 0; agent < 128; ++agent) {
        AgentSearchEntry entry{QString::number(agent),
                               QStringLiteral("project-%1").arg(agent),
                               QStringLiteral("~/dev/project-%1/src").arg(agent),
                               QStringLiteral("Category %1").arg(agent % 8),
                               agent % 2 ? QStringLiteral("Codex") : QStringLiteral("Claude"),
                               agent % 5 ? QString() : QStringLiteral("devbox"),
                               {},
                               agent % 2 ? QStringLiteral("codex") : QStringLiteral("claude")};
        for (int line = 0; line < 60; ++line) {
            QString text;
            while (text.size() < 110)
                text += words[static_cast<qsizetype>(next() % 12)] + QLatin1Char(' ');
            entry.lines << text;
        }
        entries.push_back(std::move(entry));
    }
    AgentSearch search;
    QElapsedTimer clock;
    clock.start();
    search.setEntries(entries);
    const auto indexed = clock.nsecsElapsed();
    const QStringList queries{QStringLiteral("p"),
                              QStringLiteral("pro"),
                              QStringLiteral("project-12"),
                              QStringLiteral("dev"),
                              QStringLiteral("cat 3"),
                              QStringLiteral("codex"),
                              QStringLiteral("kernel error"),
                              QStringLiteral("socket queue"),
                              QStringLiteral("devbox"),
                              QStringLiteral("zzz"),
                              QStringLiteral("pj12src"),
                              QStringLiteral("review deploy build")};
    std::vector<std::int64_t> times;
    for (int round = 0; round < 2000; ++round) {
        const auto& query = queries[round % queries.size()];
        clock.restart();
        const auto hits = search.rank(query, 40);
        times.push_back(clock.nsecsElapsed());
        static_cast<void>(hits);
    }
    std::sort(times.begin(), times.end());
    const auto at = [&times](double share) {
        return static_cast<double>(
                   times[static_cast<std::size_t>(share * static_cast<double>(times.size() - 1))]) /
               1e6;
    };
    std::cout << "agent search: 128 agents x 60 lines, index " << static_cast<double>(indexed) / 1e6
              << " ms; query p50 " << at(0.5) << " ms, p95 " << at(0.95) << " ms, p99 " << at(0.99)
              << " ms\n";
    require(at(0.99) < 250.0, "no query takes a noticeable pause");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        finds_agents_by_what_people_remember();
        stays_quick_with_many_agents();
    } catch (const std::exception& error) {
        std::cerr << "agent_search_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "agent search cases passed\n";
    return 0;
}
