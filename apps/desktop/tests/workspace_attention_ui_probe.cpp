#include "platform/window_activation.hpp"
#include "terminal_surface.hpp"
#include "ui_preview.hpp"
#include "workspace.hpp"
#include "workspace_supervisor.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace lapis::desktop;
using namespace lapis::session;

constexpr int default_timeout_ms = 30000;

void require(bool condition, const char* message,
             std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error(std::string(message) + " at line " + std::to_string(where.line()));
}

void pump(int milliseconds = 10) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

template <typename Predicate>
void until(Predicate predicate, const char* message, int timeout = default_timeout_ms,
           std::source_location where = std::source_location::current()) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        if (elapsed.elapsed() >= timeout)
            require(false, message, where);
        pump(5);
    }
}

QQuickItem* visual(QQuickItem* parent, const QString& name) {
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems())
        if (auto* found = visual(child, name))
            return found;
    return nullptr;
}

QPointF actionable_center(QQuickWindow& window, QQuickItem* target) {
    require(target && target->isVisible() && target->isEnabled(), "Control is not actionable");
    const QPointF center(target->width() / 2, target->height() / 2);
    const QPointF scene = target->mapToScene(center);
    require(window.contentItem()->contains(scene), "Control is outside the window");
    return scene;
}

void click_item(QQuickWindow& window, QQuickItem* target) {
    const QPointF position = actionable_center(window, target);
    const QPointF global = window.mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QCoreApplication::sendEvent(&window, &release);
    pump(20);
}

void click(QQuickWindow& window, const QString& name) {
    click_item(window, visual(window.contentItem(), name));
}

QString preferred_label(const QVariantMap& question) {
    // Keep the fixture contract aligned with selected_question_answers in the
    // Python runner: select by question identity, never by option order.
    const auto identifier = question.value(QStringLiteral("id")).toString();
    require(identifier == QStringLiteral("color_first") ||
                identifier == QStringLiteral("color_second"),
            "Unexpected structured-input question identity");
    const auto expected = identifier == QStringLiteral("color_first") ? QStringLiteral("Blue")
                                                                      : QStringLiteral("Red");
    QString selected;
    for (const auto& value : question.value(QStringLiteral("options")).toList()) {
        const auto label = value.toMap().value(QStringLiteral("label")).toString();
        if (label == expected || label == expected + QStringLiteral(" (Recommended)")) {
            require(selected.isEmpty(), "Structured-input answer is ambiguous");
            selected = label;
        }
    }
    require(!selected.isEmpty(), "Expected structured-input answer is unavailable");
    return selected;
}

void select_combo_option(QQuickWindow& window, QObject* dialog, const QVariantMap& question,
                         const QString& selected) {
    const auto identifier = question.value(QStringLiteral("id")).toString();
    const auto options = question.value(QStringLiteral("options")).toList();
    const auto combo_name = QStringLiteral("options-") + identifier;
    click(window, combo_name);
    auto* combo = visual(window.contentItem(), combo_name);
    auto* popup = combo->property("popup").value<QObject*>();
    require(popup, "Question options popup missing");
    until([&] { return popup->property("opened").toBool(); }, "Question options did not open");
    const auto key = [&](Qt::Key code) {
        QKeyEvent down(QEvent::KeyPress, code, Qt::NoModifier);
        QKeyEvent up(QEvent::KeyRelease, code, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &down);
        QCoreApplication::sendEvent(&window, &up);
    };
    key(Qt::Key_Home);
    for (const auto& option : options) {
        if (option.toMap().value("label").toString() == selected)
            break;
        key(Qt::Key_Down);
    }
    key(Qt::Key_Return);
    until(
        [&] {
            return dialog->property("answers")
                       .toMap()
                       .value(identifier)
                       .toMap()
                       .value("answers")
                       .toList() == QVariantList{selected};
        },
        "Question control did not update answer");
}

QJsonValue read_json(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Could not open GUI fixture config");
    const auto document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "GUI fixture config is not an object");
    return document.object();
}

bool write_json(const QString& path, const QJsonObject& report) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.flush();
}

struct SourceConfig {
    QString name;
    QString role;
    QString endpoint;
    QString program;
    QStringList arguments;
    QString directory;
};

struct ProbeOptions {
    QString config_path;
    QString output_path;
    QString manifest;
    std::vector<SourceConfig> sources;
};

ProbeOptions parse_options() {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Opt-in two-source production workspace attention qualification"));
    const QCommandLineOption config_option(QStringList{"config"},
                                           QStringLiteral("Private fixture JSON path."),
                                           QStringLiteral("path"));
    parser.addOption(config_option);
    parser.process(QCoreApplication::arguments());
    const auto root = read_json(parser.value(config_option)).toObject();
    require(root.value(QStringLiteral("version")).toInt() == 1, "Unsupported fixture schema");
    ProbeOptions result;
    result.config_path = parser.value(config_option);
    result.manifest = root.value(QStringLiteral("manifest")).toString();
    result.output_path = root.value(QStringLiteral("output")).toString();
    require(!result.manifest.isEmpty() && !result.output_path.isEmpty(),
            "Fixture paths are missing");
    const auto sessions = root.value(QStringLiteral("sessions")).toArray();
    require(sessions.size() == 2, "Fixture must contain two sources");
    QSet<QString> roles;
    QSet<QString> endpoints;
    for (const auto& value : sessions) {
        const auto object = value.toObject();
        SourceConfig source;
        source.name = object.value(QStringLiteral("name")).toString();
        source.role = object.value(QStringLiteral("role")).toString();
        source.endpoint = object.value(QStringLiteral("endpoint")).toString();
        source.program = object.value(QStringLiteral("program")).toString();
        const auto arguments = object.value(QStringLiteral("arguments")).toArray();
        for (const auto& argument : arguments)
            source.arguments.append(argument.toString());
        source.directory = object.value(QStringLiteral("directory")).toString();
        require(source.name == source.role && (source.name == QStringLiteral("approval") ||
                                               source.name == QStringLiteral("input")),
                "Invalid source role");
        require(!source.endpoint.isEmpty() && !source.program.isEmpty() &&
                    !source.directory.isEmpty(),
                "Source endpoint, program, and directory are required");
        require(!roles.contains(source.role), "Duplicate source role");
        require(!endpoints.contains(source.endpoint), "Duplicate source endpoint");
        roles.insert(source.role);
        endpoints.insert(source.endpoint);
        result.sources.push_back(std::move(source));
    }
    return result;
}

class WorkspaceFixture {
  public:
    struct SourceRecord {
        QString id;
        QVariantList requests;
    };

    explicit WorkspaceFixture(const ProbeOptions& options) : options_(options) {
        QDir private_directory(QFileInfo(options_.manifest).absolutePath());
        require(private_directory.cdUp(), "Private fixture parent is missing");
        keymap_.setSourcePathForTesting(
            private_directory.filePath(QStringLiteral("appearance.json")));
        keymap_.load();
        std::vector<WorkspaceEntry> entries;
        for (const auto& source : options_.sources) {
            SessionPreview temporary(source.role, source.directory, {}, QColor(Qt::white), "");
            LaunchSpec launch{.program = source.program,
                              .arguments = source.arguments,
                              .directory = source.directory,
                              .agent = AgentMode::codex};
            temporary.startLive(source.endpoint, launch, wire::AttachMode::discover);
            until([&] { return temporary.inputReady() && temporary.reconnectEntry().has_value(); },
                  "Could not verify source identity before workspace adoption");
            const auto entry = temporary.reconnectEntry();
            if (!entry)
                throw std::runtime_error("Source lost its verified reconnect identity");
            const QString verified_id = temporary.sessionId();
            require(!verified_id.isEmpty(), "Verified source identity is missing");
            const auto [identity, inserted] = verified_sessions_.emplace(source.role, verified_id);
            require(inserted, "Verified source identities collided");
            entries.push_back(*entry);
        }
        {
            WorkspaceRegistry registry(options_.manifest);
            registry.write(entries);
        }
        WorkspaceOptions workspace_options;
        workspace_options.manifest = options_.manifest;
        workspace_ = std::make_unique<Workspace>(WorkspaceMode::live, workspace_options);
        until([&] { return !workspace_->loading(); }, "Workspace manifest did not load");
        preview_ = std::make_unique<UiPreview>(
            *workspace_, UiPreviewOptions{.source = QUrl(QStringLiteral("qrc:/qml/Main.qml")),
                                          .compact = true,
                                          .screen = QString(),
                                          .keymap = &keymap_});
        require(preview_->load(), "Production QML did not load");
        window().requestActivate();
        lapis::desktop::test::activate_test_window(window());
        until([&] { return window().isActive(); }, "Production window did not activate");
    }

    ~WorkspaceFixture() {
        preview_.reset();
        workspace_.reset();
        QThreadPool::globalInstance()->waitForDone();
        // Python owns the services and verifies continuation before stopping them.
    }

    Workspace& workspace() { return *workspace_; }
    UiPreview& preview() { return *preview_; }
    QQuickWindow& window() { return *preview_->window(); }

    void adopt_sources() {
        until(
            [&] {
                return workspace().sessions().size() ==
                       static_cast<qsizetype>(options_.sources.size());
            },
            "Workspace did not restore both sources");
        for (std::size_t index = 0; index < options_.sources.size(); ++index) {
            const auto& source = options_.sources[index];
            const auto verified = verified_sessions_.find(source.role);
            require(verified != verified_sessions_.end(),
                    "Verified source identity was not retained");
            auto* session = workspace().session(verified->second);
            require(session, "Workspace restored the wrong source identity");
            until(
                [&] {
                    return session->inputReady() && session->attentionReady() &&
                           session->attentionCount() == 1;
                },
                "Adopted source request did not arrive");
            records_.push_back({session->sessionId(), session->attentionRequests()});
        }
        require(records_[0].id != records_[1].id, "Neighbor identities collided");
    }

    [[nodiscard]] const std::vector<SourceRecord>& records() const { return records_; }
    SessionPreview* session_for(const QString& role) {
        const auto verified = verified_sessions_.find(role);
        require(verified != verified_sessions_.end(), "Verified source identity was not retained");
        return workspace().session(verified->second);
    }

  private:
    ProbeOptions options_;
    KeyMap keymap_;
    std::unique_ptr<Workspace> workspace_;
    std::unique_ptr<UiPreview> preview_;
    std::vector<SourceRecord> records_;
    std::map<QString, QString> verified_sessions_;
};

QVariantMap queue_row(const WorkspaceSupervisor& supervisor, const QString& session_id) {
    const auto queue = supervisor.attentionQueue();
    for (const auto& value : queue) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("sessionId")).toString() == session_id)
            return row;
    }
    return {};
}

QVariantMap request_with_token(const QVariantList& requests, const QString& token) {
    for (const auto& value : requests) {
        const auto request = value.toMap();
        if (request.value(QStringLiteral("token")).toString() == token)
            return request;
    }
    return {};
}

void review_request(QQuickWindow& window, QObject* queue, QObject* dialog,
                    const WorkspaceSupervisor& supervisor, SessionPreview& session,
                    const QString& token) {
    if (!queue->property("visible").toBool()) {
        click(window, QStringLiteral("workspaceRequests"));
        until([&] { return queue->property("opened").toBool(); }, "Workspace queue did not reopen");
    }
    const auto rows = supervisor.attentionQueue();
    qsizetype row_index = 0;
    while (row_index < rows.size() &&
           rows[row_index].toMap().value("sessionId").toString() != session.sessionId())
        ++row_index;
    require(row_index < rows.size(), "Source disappeared from queue");
    click(window, QStringLiteral("workspaceRequest-") + QString::number(row_index));
    until([&] { return dialog->property("opened").toBool(); },
          "Source-bound attention dialog did not open");
    require(dialog->property("session").value<SessionPreview*>() == &session,
            "Dialog selected the wrong source");
    require(dialog->property("draft").toMap().value(QStringLiteral("token")).toString() == token,
            "Dialog selected the wrong request token");
    require(!token.isEmpty(), "Aggregate queue token is empty");
}

std::pair<const SourceConfig&, const WorkspaceFixture::SourceRecord&>
source_by_role(const ProbeOptions& options, const WorkspaceFixture& fixture, const QString& role) {
    for (std::size_t index = 0; index < options.sources.size(); ++index) {
        if (options.sources[index].role == role)
            return {options.sources[index], fixture.records()[index]};
    }
    throw std::runtime_error("Required source role is missing");
}

void set_answers(QQuickWindow& window, QObject* dialog, const QVariantMap& request) {
    const auto details = request.value(QStringLiteral("details")).toMap();
    const auto questions = details.value(QStringLiteral("questions")).toList();
    for (const auto& value : questions) {
        const auto question = value.toMap();
        const QString selected = preferred_label(question);
        require(!selected.isEmpty(), "Expected structured-input answer is unavailable");
        select_combo_option(window, dialog, question, selected);
    }
    pump(10);
}

void set_first_answer(QQuickWindow& window, QObject* dialog, const QVariantMap& request) {
    const auto details = request.value(QStringLiteral("details")).toMap();
    const auto questions = details.value(QStringLiteral("questions")).toList();
    require(!questions.isEmpty(), "Structured-input request has no questions");
    const auto question = questions.first().toMap();
    const auto selected = preferred_label(question);
    select_combo_option(window, dialog, question, selected);
}

void require_first_answer_restored(QQuickWindow& window, QObject* dialog,
                                   const QVariantMap& request) {
    const auto questions = request.value(QStringLiteral("details"))
                               .toMap()
                               .value(QStringLiteral("questions"))
                               .toList();
    require(!questions.isEmpty(), "Structured-input request lost its questions");
    const auto identifier = questions.first().toMap().value(QStringLiteral("id")).toString();
    const auto answers = dialog->property("answers").toMap();
    require(answers.value(identifier).toMap().value(QStringLiteral("answers")).toList() ==
                QVariantList{preferred_label(questions.first().toMap())},
            "Structured-input draft was not restored");
    auto* combo = visual(window.contentItem(), QStringLiteral("options-") + identifier);
    require(combo && combo->property("currentText").toString() ==
                         preferred_label(questions.first().toMap()),
            "Restored answer is not displayed in its production control");
}

void require_second_answer_untouched(QObject* dialog, const QVariantMap& request) {
    const auto questions = request.value(QStringLiteral("details"))
                               .toMap()
                               .value(QStringLiteral("questions"))
                               .toList();
    require(questions.size() == 2, "Structured-input fixture must have two questions");
    const auto identifier = questions.last().toMap().value(QStringLiteral("id")).toString();
    const auto answer = dialog->property("answers").toMap().value(identifier);
    require(!answer.isValid() || answer.toMap().value(QStringLiteral("answers")).toList().empty(),
            "Untouched structured-input question was populated");
}

void append_response(QJsonObject& report, const SourceConfig& source, SessionPreview& session,
                     const QVariantMap& request, const QString& choice) {
    auto responses = report.value(QStringLiteral("responses")).toArray();
    responses.append(QJsonObject{{"sourceId", source.role},
                                 {"sessionId", session.sessionId()},
                                 {"token", request.value(QStringLiteral("token")).toString()},
                                 {"choice", choice}});
    report.insert(QStringLiteral("responses"), responses);
}

void respond(QQuickWindow& window, QObject* dialog, const QString& choice) {
    click(window, QStringLiteral("respond-") + choice);
    until([&] { return !dialog->property("canRespond").toBool(); },
          "Response was not queued by production controls");
    require(QMetaObject::invokeMethod(dialog, "close"), "Could not close dialog");
    until([&] { return !dialog->property("visible").toBool(); }, "Response dialog did not close");
}

QJsonObject initial_report(const ProbeOptions& options) {
    return QJsonObject{{"scope", QStringLiteral("two-real-Codex workspace attention")},
                       {"config", options.config_path},
                       {"passed", false},
                       {"responses", QJsonArray{}},
                       {"checks", QJsonArray{}}};
}

void exercise(const ProbeOptions& options, QJsonObject& report) {
    WorkspaceFixture fixture(options);
    fixture.adopt_sources();
    auto* supervisor = fixture.preview().supervisor();
    require(supervisor, "Production supervisor is unavailable");
    until([&] { return supervisor->pendingCount() == 2; },
          "Aggregate workspace queue did not contain two live requests");
    require(!supervisor->attentionQueue().isEmpty(), "Aggregate queue is empty");
    auto* queue = fixture.window().findChild<QObject*>(QStringLiteral("workspaceAttentionQueue"));
    require(queue, "Workspace attention queue is missing");
    click(fixture.window(), QStringLiteral("workspaceRequests"));
    until([&] { return queue->property("opened").toBool(); }, "Workspace queue did not open");
    auto* dialog = fixture.window().findChild<QObject*>(QStringLiteral("attentionDialog"));
    require(dialog, "Production attention dialog is missing");
    const auto initial_focus = fixture.workspace().focusedSession()->sessionId();
    require(fixture.window().grabWindow().save(
                QFileInfo(options.output_path).dir().filePath("workspace-queue.png")),
            "Workspace queue capture failed");
    const auto input = source_by_role(options, fixture, QStringLiteral("input"));
    const auto approval = source_by_role(options, fixture, QStringLiteral("approval"));
    auto* input_session = fixture.session_for(QStringLiteral("input"));
    auto* approval_session = fixture.session_for(QStringLiteral("approval"));
    require(input_session && approval_session, "Verified source session was lost");

    const QString input_token = queue_row(*supervisor, input_session->sessionId())
                                    .value(QStringLiteral("token"))
                                    .toString();
    const auto input_request = request_with_token(input.second.requests, input_token);
    require(!input_request.isEmpty(), "Input queue token is not source-bound");
    review_request(fixture.window(), queue, dialog, *supervisor, *input_session, input_token);
    require(fixture.workspace().focusedSession()->sessionId() == initial_focus,
            "Reviewing input moved terminal ownership");
    set_first_answer(fixture.window(), dialog, input_request);
    require_second_answer_untouched(dialog, input_request);
    require(QMetaObject::invokeMethod(dialog, "close"), "Could not close input dialog");
    until([&] { return !dialog->property("visible").toBool(); },
          "Partial input dialog did not close");

    const QString approval_token = queue_row(*supervisor, approval_session->sessionId())
                                       .value(QStringLiteral("token"))
                                       .toString();
    const auto approval_request = request_with_token(approval.second.requests, approval_token);
    require(!approval_request.isEmpty(), "Approval queue token is not source-bound");
    review_request(fixture.window(), queue, dialog, *supervisor, *approval_session, approval_token);
    require(fixture.workspace().focusedSession()->sessionId() == initial_focus,
            "Reviewing approval moved terminal ownership");
    append_response(report, approval.first, *approval_session, approval_request,
                    QStringLiteral("accept"));
    respond(fixture.window(), dialog, QStringLiteral("accept"));

    review_request(fixture.window(), queue, dialog, *supervisor, *input_session, input_token);
    require_first_answer_restored(fixture.window(), dialog, input_request);
    require_second_answer_untouched(dialog, input_request);
    set_answers(fixture.window(), dialog, input_request);
    append_response(report, input.first, *input_session, input_request, QStringLiteral("submit"));
    respond(fixture.window(), dialog, QStringLiteral("submit"));
    until([&] { return supervisor->pendingCount() == 0; },
          "Resolved requests remained in aggregate queue");
    for (const auto& record : fixture.records()) {
        auto* session = fixture.workspace().session(record.id);
        require(session && session->inputReady() && session->hasAttentionSource(),
                "Neighbor identity or readiness was lost");
    }
}

int run_probe(const ProbeOptions& options) {
    QJsonObject report = initial_report(options);
    try {
        exercise(options, report);
        report.insert(QStringLiteral("passed"), true);
        auto checks = report.value(QStringLiteral("checks")).toArray();
        checks.append(QStringLiteral("two source-bound dialogs retained neighbor identity"));
        checks.append(QStringLiteral("structured-input draft survived approval review"));
        checks.append(QStringLiteral("aggregate queue resolved only matching requests"));
        report.insert(QStringLiteral("checks"), checks);
    } catch (const std::exception& error) {
        report.insert(QStringLiteral("error"), QString::fromUtf8(error.what()));
        std::cerr << error.what() << '\n';
    }
    if (!write_json(options.output_path, report))
        std::cerr << "Could not write GUI report\n";
    return report.value(QStringLiteral("passed")).toBool() ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    qputenv("QT_MTL_NO_TRANSACTION", "1");
    qputenv("QT_VULKAN_LIB", LAPIS_VULKAN_LIBRARY);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    qmlRegisterUncreatableType<lapis::desktop::SessionPreview>("Lapis", 1, 0, "SessionPreview",
                                                               "Owned by workspace");
    qmlRegisterType<lapis::desktop::TerminalSurface>("Lapis", 1, 0, "TerminalSurface");
    try {
        return run_probe(parse_options());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
