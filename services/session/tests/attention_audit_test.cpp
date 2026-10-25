#include "attention_audit.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

namespace {
using namespace lapis::session;

void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Attention audit check failed at line " +
                                 std::to_string(where.line()));
}

attention::Request request() {
    return {.id = std::int64_t{9},
            .thread_id = "thread",
            .turn_id = "turn",
            .item_id = "item",
            .reason = "approval",
            .summary = "Run the command?",
            .choices = {"allow", "reject"},
            .priority = 2};
}

AttentionApproval approval(const std::string& choice = "allow") {
    return {.epoch = 1, .id = std::int64_t{9}, .revision = 1, .choice = choice};
}

std::size_t count_kind(const std::vector<AttentionJournal::Entry>& entries,
                       AttentionJournal::Kind kind) {
    std::size_t result = 0;
    for (const auto& entry : entries)
        result += entry.kind == kind;
    return result;
}

std::size_t count_kind(const std::vector<AttentionJournal::Entry>& entries,
                       AttentionJournal::Kind kind, AttentionJournal::Origin origin) {
    std::size_t result = 0;
    for (const auto& entry : entries)
        result += entry.kind == kind && entry.origin == origin;
    return result;
}

int next_directory = 0;

std::filesystem::path temporary_directory(const std::string& name) {
    return std::filesystem::temp_directory_path() /
           ("lapis-attention-" + name + "-" + std::to_string(::getpid()) + "-" +
            std::to_string(++next_directory));
}

class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(const std::string& name) : directory_{temporary_directory(name)} {
        std::filesystem::create_directories(directory_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return directory_; }

  private:
    std::filesystem::path directory_;
};

void headless_ask_decision_refusal_and_restart() {
    const TemporaryDirectory directory{"audit"};
    const auto path = directory.path() / "service.attention";
    attention::State state{"session", "test"};
    state.connect(1, {true, true, true});
    const auto outcome = state.reconcile({1, 1}, {request()}, 0);
    require(outcome == attention::Outcome::applied);
    JournaledAsks journaled;
    {
        AttentionJournal journal(path);

        // This hook runs from source changes and therefore does not need a GUI
        // client; the fixture deliberately creates none.
        journal_attention(journal, state, journaled);
        require(journal.entries().size() == 1 &&
                journal.entries().front().kind == AttentionJournal::Kind::asked);
        require(journaled.at(std::int64_t{9}) == JournaledAskPosition{1, 1});

        // A stale desktop revision is refused before it becomes durable evidence.
        auto stale = approval();
        stale.revision = 0;
        require(!record_intended_decision(journal, state, stale));
        const auto unsupported = approval("maybe");
        require(!record_intended_decision(journal, state, unsupported));

        require(record_intended_decision(journal, state, approval()));
        require(journal.entries().size() == 1 && !open_questions(journal.entries()).empty());

        // A real Observer marks the request responding before send; send failure is
        // the case whose false approval the compensation must erase.
        require(state.respond(1, std::int64_t{9}, 1, "allow"));
        record_refused_decision(journal, approval());
        journaled.erase(std::int64_t{9});
        journal_attention(journal, state, journaled);
        require(journal.entries().empty());
    }
    AttentionJournal reopened(path);
    const auto& entries = reopened.entries();
    require(count_kind(entries, AttentionJournal::Kind::asked) == 1);
    require(count_kind(entries, AttentionJournal::Kind::decided, AttentionJournal::Origin::user) ==
            1);
    require(count_kind(entries, AttentionJournal::Kind::resolved,
                       AttentionJournal::Origin::agent) == 1);
    require(open_questions(entries).empty());
}

void delivered_decision_confirms_intent() {
    const TemporaryDirectory directory{"delivered"};
    const auto path = directory.path() / "delivered.attention";
    attention::State state{"session", "test"};
    state.connect(1, {true, true, true});
    require(state.reconcile({1, 1}, {request()}, 0) == attention::Outcome::applied);
    AttentionJournal journal(path);
    JournaledAsks journaled;
    journal_attention(journal, state, journaled);
    require(record_intended_decision(journal, state, approval()));
    require(!open_questions(journal.entries()).empty());
    record_delivered_decision(journal, approval());
    require(journal.entries().empty());
}

void unavailable_journal_fails_closed() {
    const TemporaryDirectory directory{"locked"};
    const auto path = directory.path() / "locked.attention";
    AttentionJournal leased(path);
    {
        std::unique_ptr<AttentionJournal> service_journal;
        bool threw = false;
        try {
            service_journal = std::make_unique<AttentionJournal>(path);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw && !service_journal);
    }
    // The decision path checks this null owner before forwarding; the test pins
    // the observable setup used by that fail-closed branch.
}

void append_failure_refuses_then_accepts_retry() {
    const TemporaryDirectory directory{"failure"};
    const auto path = directory.path() / "failure.attention";
    attention::State state{"session", "test"};
    state.connect(1, {true, true, true});
    require(state.reconcile({1, 1}, {request()}, 0) == attention::Outcome::applied);
    AttentionJournal journal(path);
    JournaledAsks journaled;
    journal_attention(journal, state, journaled);
    const auto boundary = std::filesystem::file_size(path);
    const auto previous_signal = ::signal(SIGXFSZ, SIG_IGN);
    struct FileSizeLimit {
        rlim_t saved;

        ~FileSizeLimit() {
            rlimit restore{};
            ::getrlimit(RLIMIT_FSIZE, &restore);
            restore.rlim_cur = saved;
            static_cast<void>(::setrlimit(RLIMIT_FSIZE, &restore));
        }
    };
    {
        FileSizeLimit limit{[&] {
            rlimit current{};
            require(::getrlimit(RLIMIT_FSIZE, &current) == 0);
            const auto saved = current.rlim_cur;
            current.rlim_cur = boundary;
            require(::setrlimit(RLIMIT_FSIZE, &current) == 0);
            return saved;
        }()};
        std::string diagnostic;
        try {
            require(!record_intended_decision(journal, state, approval()));
        } catch (const std::runtime_error& error) {
            diagnostic = error.what();
        }
        require(diagnostic.find("write failed") != std::string::npos);
        require(std::filesystem::file_size(path) == boundary);
        require(journal.entries().size() == 1 &&
                journal.entries().front().kind == AttentionJournal::Kind::asked);
    }
    static_cast<void>(::signal(SIGXFSZ, previous_signal));

    // The durable rollback succeeded, so the caller may safely retry and the
    // retry is recorded only once it is actually durable.
    require(record_intended_decision(journal, state, approval()));
    require(!open_questions(journal.entries()).empty());
    record_delivered_decision(journal, approval());
    require(journal.entries().empty());
}

} // namespace

int main() {
    try {
        headless_ask_decision_refusal_and_restart();
        delivered_decision_confirms_intent();
        unavailable_journal_fails_closed();
        append_failure_refuses_then_accepts_retry();
        std::cout << "attention-audit: ok\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
