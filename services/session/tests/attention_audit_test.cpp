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
            .choices = {},
            .priority = 2};
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

// Asks are journaled from source changes without a GUI client; a request
// that leaves the pending set is closed as resolved by the agent, and the
// reopened journal has no open question.
void headless_ask_resolution_and_restart() {
    const TemporaryDirectory directory{"audit"};
    const auto path = directory.path() / "service.attention";
    attention::State state{"session", "test"};
    state.connect(1, {true, true, true});
    require(state.reconcile({1, 1}, {request()}, 0) == attention::Outcome::applied);
    JournaledAsks journaled;
    {
        AttentionJournal journal(path);
        journal_attention(journal, state, journaled);
        require(journal.entries().size() == 1 &&
                journal.entries().front().kind == AttentionJournal::Kind::asked);
        require(journaled.at(std::int64_t{9}) == JournaledAskPosition{1, 1});
        // An unchanged request is not journaled twice.
        journal_attention(journal, state, journaled);
        require(journal.entries().size() == 1);
        require(state.reconcile({1, 2}, {}, 0) == attention::Outcome::applied);
        journal_attention(journal, state, journaled);
        require(journal.entries().empty() && journaled.empty());
    }
    AttentionJournal reopened(path);
    const auto& entries = reopened.entries();
    require(count_kind(entries, AttentionJournal::Kind::asked, AttentionJournal::Origin::user) ==
            1);
    require(count_kind(entries, AttentionJournal::Kind::resolved,
                       AttentionJournal::Origin::agent) == 1);
    require(open_questions(entries).empty());
}

void unavailable_journal_fails_closed() {
    const TemporaryDirectory directory{"locked"};
    const auto path = directory.path() / "locked.attention";
    AttentionJournal leased(path);
    std::unique_ptr<AttentionJournal> service_journal;
    bool threw = false;
    try {
        service_journal = std::make_unique<AttentionJournal>(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && !service_journal);
}

// A failed append leaves the file and the journaled set unchanged, so the
// next publish records the ask exactly once.
void append_failure_retries_the_ask() {
    const TemporaryDirectory directory{"failure"};
    const auto path = directory.path() / "failure.attention";
    attention::State state{"session", "test"};
    state.connect(1, {true, true, true});
    AttentionJournal journal(path);
    JournaledAsks journaled;
    journal_attention(journal, state, journaled);
    const auto boundary = std::filesystem::file_size(path);
    require(state.reconcile({1, 1}, {request()}, 0) == attention::Outcome::applied);
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
            journal_attention(journal, state, journaled);
        } catch (const std::runtime_error& error) {
            diagnostic = error.what();
        }
        require(diagnostic.find("write failed") != std::string::npos);
        require(std::filesystem::file_size(path) == boundary);
        require(journal.entries().empty() && journaled.empty());
    }
    static_cast<void>(::signal(SIGXFSZ, previous_signal));
    journal_attention(journal, state, journaled);
    require(journal.entries().size() == 1 &&
            journal.entries().front().kind == AttentionJournal::Kind::asked);
}

} // namespace

int main() {
    try {
        headless_ask_resolution_and_restart();
        unavailable_journal_fails_closed();
        append_failure_retries_the_ask();
        std::cout << "attention-audit: ok\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
