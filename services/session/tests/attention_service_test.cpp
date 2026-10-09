#define LAPIS_SESSION_SERVICE_TEST
// White-box fixture: the test TU must compile the service translation unit so
// tests can reach the private attention state through the access class below.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "session_service.cpp"

#include <QTemporaryDir>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <utility>

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
        service.ready_ = true;
        service.snapshot_in_flight_ = false;
        service.dirty_ = true;
        wire::Attachment attachment;
        attachment.identity.session_id = QByteArray(16, 's');
        attachment.identity.epoch = QByteArray(16, 'e');
        attachment.generation = 1;
        service.attachment_ = std::move(attachment);
    }

    [[nodiscard]] static bool journal_failed(const SessionService& service) {
        return !service.attention_journal_ || service.attention_journal_failed_;
    }

    [[nodiscard]] static const QString& diagnostic(const SessionService& service) {
        return service.decision_error_;
    }

    static void configure_notify_only(SessionService& service) {
        service.notify_state_ = std::make_unique<attention::State>(
            service.identity_.session_id.toHex().toStdString(), "codex-notify");
        service.notify_turns_ = std::make_unique<NotifyTurns>(*service.notify_state_);
    }

    static void receive_filtered_only(SessionService& service, const QByteArray& output) {
        service.terminal_hooks_.emplace();
        service.receive_output(output);
    }

    [[nodiscard]] static quint64 pty_read_ns(const SessionService& service) {
        return service.timing_.pty_read_ns;
    }

    [[nodiscard]] static qsizetype sync_limit(const QByteArray& output) {
        return SessionService::synchronized_output_limit(output);
    }

    static void feed_output(SessionService& service, const QByteArray& output) {
        service.pending_output_ = output;
        service.process_output();
    }

    [[nodiscard]] static bool in_sync(const SessionService& service) { return service.in_sync_; }

    static void defer_next_snapshot(SessionService& service) {
        service.snapshot_pace_.published(service.snapshot_clock_.elapsed());
    }

    [[nodiscard]] static bool publish_timer_active(const SessionService& service) {
        return service.timer_.isActive();
    }

    [[nodiscard]] static const QByteArray& pending_output(const SessionService& service) {
        return service.pending_output_;
    }

    [[nodiscard]] static quint64 snapshot_sequence(const SessionService& service) {
        return service.snapshot_sequence_;
    }

    static void publish_paced_output(SessionService& service) { service.publish_paced_output(); }

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

void notify_only_rejection_names_codex() {
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-asvc-XXXXXX")};
    require(directory.isValid());
    require(QFile::setPermissions(directory.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const auto endpoint =
        lapis::session::posix::prepare_endpoint(directory.filePath(QStringLiteral("service.sock")));
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
    lapis::session::AttentionServiceTestAccess::configure_notify_only(service);
    lapis::session::AttentionServiceTestAccess::use_client(service, &client);

    lapis::session::AttentionServiceTestAccess::decide(service, {.source_epoch = 1,
                                                                 .request_id = std::int64_t{19},
                                                                 .revision = 1,
                                                                 .choice = QStringLiteral("allow"),
                                                                 .answers = {}});
    QCoreApplication::processEvents();

    require(lapis::session::AttentionServiceTestAccess::diagnostic(service) ==
            QStringLiteral("Answer Codex requests in the terminal"));
}

void filtered_output_still_records_pty_timing() {
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-asvc-XXXXXX")};
    require(directory.isValid());
    require(QFile::setPermissions(directory.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const auto endpoint =
        lapis::session::posix::prepare_endpoint(directory.filePath(QStringLiteral("service.sock")));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = AgentMode::codex};
    const QByteArray session_id = QByteArray::fromHex(QByteArray(32, '1'));
    const QByteArray session_epoch = QByteArray::fromHex(QByteArray(32, '2'));
    SessionService service{endpoint, session_id, launch, session_epoch};

    lapis::session::AttentionServiceTestAccess::receive_filtered_only(
        service, "\x1b]7717;lapis-init;codex;not-bound\x07");
    require(lapis::session::AttentionServiceTestAccess::pty_read_ns(service) != 0);
}

void parser_carries_a_split_synchronized_marker() {
    using Access = lapis::session::AttentionServiceTestAccess;
    const QByteArray begin = QByteArrayLiteral("\x1b[?2026h");
    const QByteArray end = QByteArrayLiteral("\x1b[?2026l");
    require(Access::sync_limit(QByteArrayLiteral("plain output")) == 12);
    require(Access::sync_limit(begin + QByteArrayLiteral("frame")) == begin.size());
    require(Access::sync_limit(begin + QByteArrayLiteral("frame") + end +
                               QByteArrayLiteral("next")) == begin.size());
    require(Access::sync_limit(begin.left(4)) == -begin.size());
    require(Access::sync_limit(QByteArrayLiteral("x") + begin.left(4)) == -begin.size());
}

void adjacent_updates_do_not_hide_a_finished_frame() {
    using Access = lapis::session::AttentionServiceTestAccess;
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-sync-XXXXXX")};
    require(directory.isValid());
    const auto endpoint =
        lapis::session::posix::prepare_endpoint(directory.filePath(QStringLiteral("service.sock")));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = AgentMode::terminal};
    SessionService service{endpoint, QByteArray(32, '1'), launch, QByteArray(32, '2')};
    QLocalSocket client;
    client.connectToServer(endpoint);
    require(client.waitForConnected(5000));
    Access::use_client(service, &client);

    const QByteArray begin = QByteArrayLiteral("\x1b[?2026h");
    const QByteArray end = QByteArrayLiteral("\x1b[?2026l");
    Access::feed_output(service,
                        begin + QByteArrayLiteral("ONE") + end + begin + QByteArrayLiteral("TWO"));
    require(client.bytesToWrite() > 0);
    require(!Access::in_sync(service));
    QCoreApplication::processEvents();
    require(Access::in_sync(service));
}

void a_marker_crossing_the_chunk_boundary_feeds_as_one_unit() {
    using Access = lapis::session::AttentionServiceTestAccess;
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-sync-XXXXXX")};
    require(directory.isValid());
    const auto endpoint =
        lapis::session::posix::prepare_endpoint(directory.filePath(QStringLiteral("service.sock")));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = AgentMode::terminal};
    SessionService service{endpoint, QByteArray(32, '1'), launch, QByteArray(32, '2')};
    QLocalSocket client;
    client.connectToServer(endpoint);
    require(client.waitForConnected(5000));
    Access::use_client(service, &client);
    const QByteArray padding(100, 'x');
    Access::feed_output(service, padding + QByteArrayLiteral("\x1b[?2026hWHOLE\x1b[?2026lNEXT"));
    require(Access::pending_output(service) == QByteArrayLiteral("NEXT"));
    require(client.bytesToWrite() > 0);
}

void a_completed_frame_waits_for_the_paced_publication() {
    using Access = lapis::session::AttentionServiceTestAccess;
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-sync-XXXXXX")};
    require(directory.isValid());
    const auto endpoint =
        lapis::session::posix::prepare_endpoint(directory.filePath(QStringLiteral("service.sock")));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = AgentMode::terminal};
    SessionService service{endpoint, QByteArray(32, '1'), launch, QByteArray(32, '2')};
    QLocalSocket client;
    client.connectToServer(endpoint);
    require(client.waitForConnected(5000));
    Access::use_client(service, &client);
    const quint64 before_snapshot = Access::snapshot_sequence(service);
    Access::defer_next_snapshot(service);

    Access::feed_output(service, QByteArrayLiteral("\x1b[?2026hWHOLE\x1b[?2026l"));
    require(Access::publish_timer_active(service));
    require(client.bytesToWrite() == 0);
    Access::publish_paced_output(service);
    require(Access::snapshot_sequence(service) > before_snapshot);
    require(!Access::publish_timer_active(service));
}

void a_split_marker_waits_when_the_client_snapshot_is_in_flight() {
    using Access = lapis::session::AttentionServiceTestAccess;
    QTemporaryDir directory{QStringLiteral("/private/tmp/lapis-sync-XXXXXX")};
    require(directory.isValid());
    const auto endpoint =
        lapis::session::posix::prepare_endpoint(directory.filePath(QStringLiteral("service.sock")));
    const LaunchSpec launch{.program = QStringLiteral("/bin/cat"),
                            .arguments = {},
                            .directory = directory.path(),
                            .size = {80, 24},
                            .agent = AgentMode::terminal};
    SessionService service{endpoint, QByteArray(32, '1'), launch, QByteArray(32, '2')};
    QLocalSocket client;
    client.connectToServer(endpoint);
    require(client.waitForConnected(5000));
    Access::use_client(service, &client);
    const QByteArray large(static_cast<qsizetype>(1024 * 1024), '0');
    require(client.write(large) > 0);
    require(client.bytesToWrite() > 0);

    const QByteArray output = QByteArrayLiteral("x\x1b[?202");
    Access::feed_output(service, output);
    require(Access::pending_output(service) == QByteArrayLiteral("\x1b[?202"));
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application{argc, argv};
    try {
        failed_journal_refuses_decision();
        notify_only_rejection_names_codex();
        filtered_output_still_records_pty_timing();
        parser_carries_a_split_synchronized_marker();
        adjacent_updates_do_not_hide_a_finished_frame();
        a_marker_crossing_the_chunk_boundary_feeds_as_one_unit();
        a_completed_frame_waits_for_the_paced_publication();
        a_split_marker_waits_when_the_client_snapshot_is_in_flight();
        std::cout << "attention-service: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
