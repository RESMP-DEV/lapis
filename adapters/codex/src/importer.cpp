#include "importer.hpp"
#include "unix_websocket.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lapis::codex {
namespace {
using ImportPhase = Importer::Phase;
using ImportItemType = Importer::ItemType;
using DetectedItem = Importer::DetectedItem;
using DetectedSession = Importer::DetectedSession;
using ItemResult = Importer::ItemResult;
using ImportedSession = Importer::ImportedSession;
using ImportFailure = Importer::ImportFailure;

constexpr qsizetype identity_limit = 256;
constexpr qsizetype description_limit = 8192;
constexpr qsizetype path_limit = 32768;
constexpr qsizetype message_limit = 8192;
constexpr qsizetype detection_limit = 128;
constexpr qsizetype sessions_limit = 1024;
constexpr qsizetype result_limit = 1024;
constexpr int rpc_timeout = 10000;
constexpr int import_timeout = 120000;

bool bounded_text(const QString& value, qsizetype limit, bool allow_empty = false) {
    return (allow_empty || !value.isEmpty()) && value.toUtf8().size() <= limit;
}

QString required_text(const QJsonObject& object, const QString& name, qsizetype limit,
                      bool allow_empty = false) {
    const auto value = object.value(name);
    if (!value.isString() || !bounded_text(value.toString(), limit, allow_empty))
        throw std::runtime_error("Codex import payload has an invalid " + name.toStdString());
    return value.toString();
}

std::optional<QString> optional_text(const QJsonObject& object, const QString& name,
                                     qsizetype limit) {
    const auto value = object.value(name);
    if (value.isUndefined() || value.isNull())
        return std::nullopt;
    if (!value.isString() || !bounded_text(value.toString(), limit, true))
        throw std::runtime_error("Codex import payload has an invalid " + name.toStdString());
    return value.toString();
}

ImportItemType item_type(const QString& value) {
    if (value == QLatin1String("AGENTS_MD"))
        return ImportItemType::agents_md;
    if (value == QLatin1String("CONFIG"))
        return ImportItemType::config;
    if (value == QLatin1String("SKILLS"))
        return ImportItemType::skills;
    if (value == QLatin1String("PLUGINS"))
        return ImportItemType::plugins;
    if (value == QLatin1String("MCP_SERVER_CONFIG"))
        return ImportItemType::mcp_server_config;
    if (value == QLatin1String("SUBAGENTS"))
        return ImportItemType::subagents;
    if (value == QLatin1String("HOOKS"))
        return ImportItemType::hooks;
    if (value == QLatin1String("COMMANDS"))
        return ImportItemType::commands;
    if (value == QLatin1String("MEMORY"))
        return ImportItemType::memory;
    if (value == QLatin1String("SESSIONS"))
        return ImportItemType::sessions;
    throw std::runtime_error("Codex import returned an unknown migration item type");
}

DetectedSession session(const QJsonObject& value, QSet<QString>& paths) {
    DetectedSession result;
    result.cwd = required_text(value, QStringLiteral("cwd"), path_limit);
    result.path = required_text(value, QStringLiteral("path"), path_limit);
    result.title = optional_text(value, QStringLiteral("title"), message_limit);
    if (!paths.contains(result.path)) {
        paths.insert(result.path);
    } else {
        throw std::runtime_error("Codex detected the same session twice");
    }
    return result;
}

DetectedItem detection_item(const QJsonObject& value, QSet<QString>& paths) {
    DetectedItem result;
    result.item_type = item_type(required_text(value, QStringLiteral("itemType"), 64));
    result.description = required_text(value, QStringLiteral("description"), description_limit);
    const auto cwd = optional_text(value, QStringLiteral("cwd"), path_limit);
    result.home_scoped = !cwd.has_value() || cwd->isEmpty();

    const auto details_value = value.value(QStringLiteral("details"));
    if (!details_value.isUndefined() && !details_value.isNull()) {
        if (!details_value.isObject())
            throw std::runtime_error("Invalid Codex migration details");
        const auto details = details_value.toObject();
        if (result.item_type == ImportItemType::sessions) {
            // A session selection cannot quietly carry executable or global
            // configuration classes. Codex serializes every known list key,
            // including empty ones; only that exact expanded shape is allowed.
            constexpr const char* allowed[] = {"plugins", "skills",    "sessions", "mcpServers",
                                               "hooks",   "subagents", "commands", "memory"};
            if (details.size() > static_cast<qsizetype>(std::size(allowed)))
                throw std::runtime_error("Codex session item has unexpected migration details");
            for (const auto& name : details.keys()) {
                const auto known =
                    std::any_of(std::begin(allowed), std::end(allowed),
                                [&](const char* wanted) { return name == QLatin1String(wanted); });
                if (!known)
                    throw std::runtime_error(
                        "Codex session item has an unknown migration details key");
                if (name == QLatin1String("sessions"))
                    continue;
                const auto other = details.value(name);
                if (!other.isArray() || !other.toArray().isEmpty())
                    throw std::runtime_error(
                        "Codex session item carries a non-session migration class");
            }
            const auto sessions_value = details.value(QStringLiteral("sessions"));
            if (!sessions_value.isArray() || sessions_value.toArray().size() > sessions_limit)
                throw std::runtime_error("Invalid Codex detected session list");
            for (const auto& entry : sessions_value.toArray()) {
                if (!entry.isObject())
                    throw std::runtime_error("Invalid Codex detected session");
                result.sessions.push_back(session(entry.toObject(), paths));
            }
        }
    }
    // Only the sessions-only first slice can be submitted. Other detected
    // classes remain visible as typed metadata, but their raw payloads are not
    // retained by Lapis before a separately reviewed opt-in implementation.
    if (result.item_type == ImportItemType::sessions)
        result.payload = value;
    return result;
}

void validate_connectors(const QJsonObject& result) {
    const auto value = result.value(QStringLiteral("connectors"));
    if (value.isUndefined())
        return;
    if (!value.isArray() || value.toArray().size() > detection_limit)
        throw std::runtime_error("Invalid Codex connector detection");
    for (const auto& entry : value.toArray()) {
        if (!entry.isObject())
            throw std::runtime_error("Invalid Codex connector candidate");
        const auto connector = entry.toObject();
        required_text(connector, QStringLiteral("name"), message_limit);
        const auto count_value = connector.value(QStringLiteral("sessionCount"));
        const auto count = count_value.toInteger(-1);
        if (!count_value.isDouble() || count < 0 || count > std::numeric_limits<quint32>::max())
            throw std::runtime_error("Invalid Codex connector session count");
        const auto source = required_text(connector, QStringLiteral("source"), 64);
        if (source != QLatin1String("remoteMcpServersConfig") &&
            source != QLatin1String("sessionToolUse"))
            throw std::runtime_error("Unknown Codex connector source");
    }
}

ImportFailure failure(const QJsonObject& value) {
    ImportFailure result;
    result.failure_stage = required_text(value, QStringLiteral("failureStage"), 256);
    result.message = required_text(value, QStringLiteral("message"), message_limit);
    result.cwd = optional_text(value, QStringLiteral("cwd"), path_limit);
    result.source = optional_text(value, QStringLiteral("source"), path_limit);
    result.error_type = optional_text(value, QStringLiteral("errorType"), 256);
    result.sub_error_type = optional_text(value, QStringLiteral("subErrorType"), 256);
    return result;
}

ImportedSession imported(const QJsonObject& value) {
    ImportedSession result;
    result.cwd = optional_text(value, QStringLiteral("cwd"), path_limit);
    result.source = optional_text(value, QStringLiteral("source"), path_limit);
    result.title = optional_text(value, QStringLiteral("title"), message_limit);
    result.target = required_text(value, QStringLiteral("target"), identity_limit);
    if (result.target.isEmpty())
        throw std::runtime_error("Imported session lacks a target identity");
    return result;
}

ItemResult item_result(const QJsonObject& value) {
    ItemResult result;
    const auto type_text = required_text(value, QStringLiteral("itemType"), 64);
    result.item_type = item_type(type_text);
    const auto failures = value.value(QStringLiteral("failures"));
    const auto successes = value.value(QStringLiteral("successes"));
    if (!failures.isArray() || !successes.isArray() ||
        failures.toArray().size() + successes.toArray().size() > result_limit)
        throw std::runtime_error("Codex import result exceeded its bounded shape");
    for (const auto& entry : successes.toArray()) {
        if (!entry.isObject())
            throw std::runtime_error("Invalid Codex import success");
        const auto success = entry.toObject();
        if (required_text(success, QStringLiteral("itemType"), 64) != type_text)
            throw std::runtime_error("Codex import result mixes migration item types");
        if (result.item_type == ImportItemType::sessions)
            result.sessions.push_back(imported(success));
    }
    for (const auto& entry : failures.toArray()) {
        if (!entry.isObject())
            throw std::runtime_error("Invalid Codex import failure");
        const auto item = entry.toObject();
        if (required_text(item, QStringLiteral("itemType"), 64) != type_text)
            throw std::runtime_error("Codex import result mixes migration item types");
        result.failures.push_back(failure(item));
    }
    return result;
}

QVector<ItemResult> item_results(const QJsonValue& value, const QSet<QString>& selected) {
    if (!value.isArray())
        throw std::runtime_error("Codex import results are missing");
    const auto entries = value.toArray();
    if (entries.size() > detection_limit)
        throw std::runtime_error("Codex import result count exceeded its limit");
    QVector<ItemResult> results;
    QSet<QString> seen;
    results.reserve(entries.size());
    for (const auto& entry : entries) {
        if (!entry.isObject())
            throw std::runtime_error("Invalid Codex import result");
        const auto result = item_result(entry.toObject());
        const auto name = required_text(entry.toObject(), QStringLiteral("itemType"), 64);
        if (seen.contains(name))
            throw std::runtime_error("Duplicate Codex import result item type");
        seen.insert(name);
        if (!selected.contains(name))
            throw std::runtime_error("Codex imported an unselected migration item type");
        results.push_back(result);
    }
    return results;
}
} // namespace

class Importer::Impl final : public QObject {
  public:
    explicit Impl(Importer& owner) : owner_(owner) {
        deadline_.setSingleShot(true);
        connect(&deadline_, &QTimer::timeout, this, [this] {
            fail(waiting_.isEmpty() ? QStringLiteral("Codex import completion timed out")
                                    : QStringLiteral("Codex import RPC timed out"));
        });
    }

    void start(const QString& socket) {
        close();
        clear_task();
        if (socket.isEmpty() || socket.toUtf8().size() > path_limit) {
            fail(QStringLiteral("Invalid Codex import socket"));
            return;
        }
        path_ = socket;
        phase_ = ImportPhase::connecting;
        diagnostic_ = QStringLiteral("Connecting to Codex import server");
        transport_ = new UnixWebSocket(this);
        connect(transport_, &UnixWebSocket::opened, this, [this] {
            rpc(QStringLiteral("initialize"),
                QJsonObject{{QStringLiteral("clientInfo"),
                             QJsonObject{{QStringLiteral("name"), QStringLiteral("lapis")},
                                         {QStringLiteral("version"), QStringLiteral("0.1")}}},
                            {QStringLiteral("capabilities"),
                             QJsonObject{{QStringLiteral("experimentalApi"), true}}}},
                rpc_timeout);
        });
        connect(transport_, &UnixWebSocket::message, this, [this](const QByteArray& bytes) {
            try {
                receive(bytes);
            } catch (const std::exception& error) {
                fail(QString::fromUtf8(error.what()));
            }
        });
        connect(transport_, &UnixWebSocket::failed, this,
                [this](const QString& error) { fail(error); });
        transport_->open(path_);
        emit owner_.changed();
    }

    void stop() {
        close();
        clear_task();
        phase_ = ImportPhase::disconnected;
        diagnostic_ = QStringLiteral("Codex importer stopped");
        emit owner_.changed();
    }

    bool detect() { return detect(Importer::DetectOptions{}); }

    bool detect(const Importer::DetectOptions& options) {
        if (phase_ != ImportPhase::ready || !transport_)
            return false;
        QJsonObject parameters{{QStringLiteral("includeHome"), options.include_home},
                               {QStringLiteral("migrationSource"), QStringLiteral("claude")}};
        if (!options.cwds.isEmpty()) {
            QSet<QString> seen;
            QJsonArray cwds;
            for (const auto& cwd : options.cwds) {
                if (!bounded_text(cwd, path_limit) || seen.contains(cwd))
                    return false;
                seen.insert(cwd);
                cwds.append(cwd);
            }
            parameters.insert(QStringLiteral("cwds"), cwds);
        }
        if (options.maximum_session_age_days.has_value())
            parameters.insert(QStringLiteral("maxSessionAgeDays"),
                              static_cast<qint64>(*options.maximum_session_age_days));
        if (options.maximum_sessions.has_value())
            parameters.insert(QStringLiteral("maxSessions"),
                              static_cast<qint64>(*options.maximum_sessions));
        phase_ = ImportPhase::detecting;
        diagnostic_ = QStringLiteral("Detecting Claude sessions");
        rpc(QStringLiteral("externalAgentConfig/detect"), parameters, rpc_timeout);
        emit owner_.changed();
        return true;
    }

    bool import_sessions(const QVector<qsizetype>& positions) {
        if (phase_ != ImportPhase::ready || !transport_)
            return false;
        if (positions.isEmpty() || positions.size() > detection_.items.size())
            return false;
        QSet<qsizetype> unique;
        QJsonArray migration_items;
        for (const auto position : positions) {
            if (position < 0 || position >= detection_.items.size() || unique.contains(position))
                return false;
            unique.insert(position);
            const auto& item = detection_.items[static_cast<qsizetype>(position)];
            if (item.item_type != ImportItemType::sessions || item.sessions.isEmpty())
                return false;
            migration_items.append(item.payload);
        }
        selected_types_ = {QStringLiteral("SESSIONS")};
        import_id_.clear();
        progress_.clear();
        completion_.reset();
        phase_ = ImportPhase::importing;
        diagnostic_ = QStringLiteral("Waiting for Codex import acceptance");
        rpc(QStringLiteral("externalAgentConfig/import"),
            QJsonObject{{QStringLiteral("migrationItems"), migration_items},
                        {QStringLiteral("source"), QStringLiteral("lapis")},
                        {QStringLiteral("providerId"), QStringLiteral("claude")},
                        {QStringLiteral("migrationSource"), QStringLiteral("claude")}},
            rpc_timeout);
        emit owner_.changed();
        return true;
    }

    ImportPhase phase() const { return phase_; }
    const QString& diagnostic() const { return diagnostic_; }
    const Importer::Detection& detection() const { return detection_; }
    const QVector<ItemResult>& progress() const { return progress_; }
    const Importer::Completion& completion() const {
        static const Importer::Completion empty;
        return completion_.has_value() ? *completion_ : empty;
    }
    QString import_id() const { return import_id_; }
    QVector<ImportedSession> imported_sessions() const {
        if (!completion_.has_value())
            return {};
        QVector<ImportedSession> sessions;
        for (const auto& result : completion_->results)
            sessions += result.sessions;
        return sessions;
    }

  private:
    void close() {
        deadline_.stop();
        if (transport_) {
            transport_->disconnect(this);
            transport_->close();
            transport_->deleteLater();
            transport_ = nullptr;
        }
        waiting_.clear();
        initialized_ = false;
    }

    void clear_task() {
        detection_.items.clear();
        selected_types_.clear();
        import_id_.clear();
        progress_.clear();
        completion_.reset();
    }

    void fail(const QString& reason) {
        close();
        phase_ = ImportPhase::failed;
        diagnostic_ = reason;
        emit owner_.failed(reason);
        emit owner_.changed();
    }

    void rpc(const QString& method, const QJsonObject& parameters, int timeout) {
        if (!transport_ || !waiting_.isEmpty() || rpc_id_ == std::numeric_limits<qint64>::max()) {
            fail(QStringLiteral("Invalid Codex import RPC state"));
            return;
        }
        waiting_ = method;
        ++rpc_id_;
        deadline_.start(timeout);
        const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("id"), rpc_id_},
                                                     {QStringLiteral("method"), method},
                                                     {QStringLiteral("params"), parameters}})
                               .toJson(QJsonDocument::Compact);
        if (!transport_->send(bytes))
            fail(QStringLiteral("Codex import RPC write failed"));
    }

    void finish_detection(const QJsonObject& result) {
        const auto value = result.value(QStringLiteral("items"));
        if (!value.isArray() || value.toArray().size() > detection_limit)
            throw std::runtime_error("Invalid Codex migration detection list");
        validate_connectors(result);
        Importer::Detection replacement;
        QSet<QString> paths;
        replacement.items.reserve(value.toArray().size());
        for (const auto& entry : value.toArray()) {
            if (!entry.isObject())
                throw std::runtime_error("Invalid Codex migration item");
            replacement.items.push_back(detection_item(entry.toObject(), paths));
        }
        detection_ = std::move(replacement);
        phase_ = ImportPhase::ready;
        diagnostic_ = QStringLiteral("Claude detection ready");
        emit owner_.detected();
        emit owner_.changed();
    }

    void merge_progress(QVector<ItemResult> replacement) {
        for (const auto& result : replacement) {
            const auto name = itemTypeString(result.item_type);
            const auto existing =
                std::find_if(progress_.begin(), progress_.end(), [&](const ItemResult& value) {
                    return value.item_type == result.item_type;
                });
            if (existing != progress_.end())
                *existing = result;
            else
                progress_.push_back(result);
        }
        if (progress_.size() > selected_types_.size())
            throw std::runtime_error("Codex progress exceeded selected item types");
    }

    static QString itemTypeString(ImportItemType type) {
        switch (type) {
        case ImportItemType::agents_md:
            return QStringLiteral("AGENTS_MD");
        case ImportItemType::config:
            return QStringLiteral("CONFIG");
        case ImportItemType::skills:
            return QStringLiteral("SKILLS");
        case ImportItemType::plugins:
            return QStringLiteral("PLUGINS");
        case ImportItemType::mcp_server_config:
            return QStringLiteral("MCP_SERVER_CONFIG");
        case ImportItemType::subagents:
            return QStringLiteral("SUBAGENTS");
        case ImportItemType::hooks:
            return QStringLiteral("HOOKS");
        case ImportItemType::commands:
            return QStringLiteral("COMMANDS");
        case ImportItemType::memory:
            return QStringLiteral("MEMORY");
        case ImportItemType::sessions:
            return QStringLiteral("SESSIONS");
        }
        throw std::runtime_error("Unknown migration item type");
    }

    void receive(const QByteArray& bytes) {
        const auto document = QJsonDocument::fromJson(bytes);
        if (!document.isObject())
            throw std::runtime_error("Invalid Codex import JSON object");
        const auto message = document.object();
        if (message.contains(QStringLiteral("method")))
            receive_notification(message);
        else
            receive_reply(message);
    }

    void receive_notification(const QJsonObject& message) {
        const auto method = message.value(QStringLiteral("method")).toString();
        if (method != QLatin1String("externalAgentConfig/import/progress") &&
            method != QLatin1String("externalAgentConfig/import/completed")) {
            if (method == QLatin1String("initialized") && !initialized_ &&
                !message.contains(QStringLiteral("params")))
                return;
            return;
        }
        if (message.contains(QStringLiteral("id")))
            throw std::runtime_error("Codex import notification unexpectedly requested a reply");
        const auto parameters = message.value(QStringLiteral("params")).toObject();
        if (!message.value(QStringLiteral("params")).isObject())
            throw std::runtime_error("Codex import notification lacks parameters");
        const auto identifier =
            required_text(parameters, QStringLiteral("importId"), identity_limit);
        if (identifier != import_id_)
            return; // A shared server may broadcast another client's completion.
        if (phase_ != ImportPhase::importing)
            throw std::runtime_error("Unexpected Codex import notification after completion");
        const auto replacement =
            item_results(parameters.value(QStringLiteral("itemTypeResults")), selected_types_);
        const bool completed = method == QLatin1String("externalAgentConfig/import/completed");
        if (completed) {
            const auto selected =
                std::any_of(replacement.begin(), replacement.end(), [](const ItemResult& value) {
                    return value.item_type == ImportItemType::sessions;
                });
            if (!selected)
                throw std::runtime_error("Codex completion omitted the selected session import");
            for (const auto& result : replacement) {
                if (result.sessions.isEmpty() && result.failures.isEmpty())
                    throw std::runtime_error(
                        "Codex session result has neither success nor failure");
            }
            Importer::Completion finished;
            finished.import_id = identifier;
            finished.results = replacement;
            completion_ = std::move(finished);
            progress_ = replacement;
            const auto failures =
                std::any_of(replacement.begin(), replacement.end(),
                            [](const ItemResult& value) { return !value.failures.isEmpty(); });
            phase_ = failures ? ImportPhase::completed_with_failures : ImportPhase::completed;
            diagnostic_ = failures ? QStringLiteral("Codex import completed with failures")
                                   : QStringLiteral("Codex import completed");
            close();
            emit owner_.importCompleted();
        } else {
            merge_progress(std::move(replacement));
            diagnostic_ = QStringLiteral("Codex import is running");
            emit owner_.importProgress();
        }
        emit owner_.changed();
    }

    void receive_reply(const QJsonObject& message) {
        const auto identifier = message.value(QStringLiteral("id"));
        if (!identifier.isDouble() || identifier.toInteger(-1) != rpc_id_ || waiting_.isEmpty())
            throw std::runtime_error("Unexpected Codex import RPC reply");
        const auto method = std::exchange(waiting_, {});
        deadline_.stop();
        if (message.contains(QStringLiteral("error")))
            throw std::runtime_error("Codex import RPC failed");
        const auto result = message.value(QStringLiteral("result"));
        if (!result.isObject())
            throw std::runtime_error("Invalid Codex import RPC result");
        if (method == QLatin1String("initialize")) {
            required_text(result.toObject(), QStringLiteral("userAgent"), identity_limit);
            initialized_ = true;
            phase_ = ImportPhase::ready;
            diagnostic_ = QStringLiteral("Codex importer ready");
            emit owner_.connected();
        } else if (method == QLatin1String("externalAgentConfig/detect")) {
            finish_detection(result.toObject());
        } else if (method == QLatin1String("externalAgentConfig/import")) {
            import_id_ =
                required_text(result.toObject(), QStringLiteral("importId"), identity_limit);
            if (import_id_.isEmpty())
                throw std::runtime_error("Codex returned an empty import identity");
            diagnostic_ = QStringLiteral("Codex accepted the import");
            deadline_.start(import_timeout);
            emit owner_.importAccepted(import_id_);
        } else {
            throw std::runtime_error("Unexpected Codex import RPC result");
        }
        emit owner_.changed();
    }

    Importer& owner_;
    UnixWebSocket* transport_{}; // QObject-owned and retired with deleteLater.
    QTimer deadline_;
    QString path_;
    QString diagnostic_ = QStringLiteral("Codex importer disconnected");
    QString import_id_;
    Importer::Detection detection_;
    QVector<ItemResult> progress_;
    std::optional<Importer::Completion> completion_;
    QSet<QString> selected_types_;
    QString waiting_;
    qint64 rpc_id_{};
    ImportPhase phase_ = ImportPhase::disconnected;
    bool initialized_{};
};

Importer::Importer(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(*this)) {}
Importer::~Importer() = default;
void Importer::start(const QString& socket) { impl_->start(socket); }
void Importer::stop() { impl_->stop(); }
bool Importer::detect() { return impl_->detect(); }
bool Importer::detect(const DetectOptions& options) { return impl_->detect(options); }
bool Importer::importSessions(const QVector<qsizetype>& item_positions) {
    return impl_->import_sessions(item_positions);
}
Importer::Phase Importer::phase() const { return impl_->phase(); }
const QString& Importer::diagnostic() const { return impl_->diagnostic(); }
const Importer::Detection& Importer::detection() const { return impl_->detection(); }
const QVector<Importer::ItemResult>& Importer::progress() const { return impl_->progress(); }
const Importer::Completion& Importer::completion() const { return impl_->completion(); }
QString Importer::importId() const { return impl_->import_id(); }
QVector<Importer::ImportedSession> Importer::importedSessions() const {
    return impl_->imported_sessions();
}
} // namespace lapis::codex
