#define LAPIS_SESSION_SERVICE_TEST
#include "session_service.cpp"

#include <QTemporaryDir>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace lapis::session {
class AttentionServiceTestAccess final {
  public:
    static void seed_request(SessionService& service, const attention::Request& request) {
        auto* state = service.codex_state_.get();
        if (!state)
            throw std::runtime_error("Codex attention state was not constructed");
        state->connect(7, {true, true, true});
        if (state->reconcile({7, 1}, {request}, 0) != attention::Outcome::applied)
            throw std::runtime_error("Test request was not admitted");
    }

    static void decide(SessionService& service, const wire::AttentionDecision& decision) {
        service.decide_attention(wire::encode_attention_decision(decision));
    }

    static void use_client(SessionService& service, QLocalSocket* client) {
        service.client_ = client;
        wire::Attachment attachment;
        attachment.identity.session_id = QByteArray(16, 's');
        attachment.identity.epoch = QByteArray(16, 'e');
        attachment.generation = 1;
        service.attachment_ = attachment;
    }

    [[nodiscard]] static bool journal_failed(const SessionService& service) {
        return !service.attention_journal_ || service.attention_journal_failed_;
    }

    [[nodiscard]] static QString diagnostic(const SessionService& service) {
        return service.decision_error_;
    }

    [[nodiscard]] static bool request_pending(const SessionService& service,
                                              const attention::RequestId& id) {
        const auto found = service.codex_state_->pending().find(id);
        return found != service.codex_state_->pending().end() &&
               found->second.status == attention::RequestStatus::pending &&
               !found->second.submitted;
    }
};
} // namespace lapis::session

namespace {
void require(bool value, std::source_location where = std::source_location::current()) {
    if (!value)
        throw std::runtime_error("Attention service check failed at line " +
                                 std::to_string(where.line()));
}

attention::Request request() {
    return {.id = std::int64_t{19},
            .thread_id = "thread",
            .turn_id = "turn",
            .item_id = "item",
            .reason = "approval",
            .summary = "Run the fixture?",
            .choices = {"allow", "reject"},
            .priority = 2};
}

void failed_journal_refuses_decision() {
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-asvc-XXXXXX")};
    require(directory.isValid());
    require(QFile::setPermissions(directory.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const auto endpoint =
        lapis::session::posix::prepare_endpoint(directory.filePath(QStringLiteral("service.sock")));
    AttentionJournal lease{std::filesystem::path(
        QFile::encodeName(endpoint + QStringLiteral(".attention")).constData())};

    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = AgentMode::codex};
    const QByteArray session_id = QByteArray::fromHex(QByteArray(32, '1'));
    const QByteArray session_epoch = QByteArray::fromHex(QByteArray(32, '2'));
    SessionService service{endpoint, session_id, launch, session_epoch};
    QLocalSocket client;
    client.connectToServer(endpoint);
    require(client.waitForConnected(5000));
    lapis::session::AttentionServiceTestAccess::seed_request(service, request());
    lapis::session::AttentionServiceTestAccess::use_client(service, &client);

    lapis::session::AttentionServiceTestAccess::decide(service, {.source_epoch = 7,
                                                                 .request_id = std::int64_t{19},
                                                                 .revision = 1,
                                                                 .choice = QStringLiteral("allow"),
                                                                 .answers = {}});
    QCoreApplication::processEvents();

    require(lapis::session::AttentionServiceTestAccess::journal_failed(service));
    require(lapis::session::AttentionServiceTestAccess::diagnostic(service) ==
            QStringLiteral("Decision could not be recorded; try again"));
    require(lapis::session::AttentionServiceTestAccess::request_pending(service, std::int64_t{19}));
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    try {
        failed_journal_refuses_decision();
        std::cout << "attention-service: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
