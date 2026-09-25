#include "observer.hpp"
#include "unix_websocket.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace lapis::codex {
namespace {
namespace attention = session::attention;
using attention::Activity;
using attention::RequestId;
constexpr qsizetype message_limit = qsizetype{64} * 1024;
constexpr qsizetype details_limit = qsizetype{16} * 1024;
constexpr qsizetype aggregate_details_limit = qsizetype{512} * 1024;
constexpr int rpc_timeout = 10000;
constexpr auto waiting_for_history = QLatin1String("Waiting for Codex thread history");
quint64 now() {
    return static_cast<quint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}
bool text(const QString& value, qsizetype limit) {
    return !value.isEmpty() && value.toUtf8().size() <= limit;
}
std::optional<RequestId> request_id(const QJsonValue& value) {
    if (value.isString() && text(value.toString(), 256))
        return value.toString().toStdString();
    if (!value.isDouble())
        return std::nullopt;
    constexpr auto sentinel = std::numeric_limits<qint64>::max();
    const auto integer = value.toInteger(sentinel);
    // A second fallback distinguishes a valid INT64_MAX from conversion failure.
    // JSON numeric equality can round INT64_MAX to the out-of-range double 2^63.
    if (integer == sentinel && value.toInteger(std::numeric_limits<qint64>::min()) != sentinel)
        return std::nullopt;
    return integer;
}
QJsonValue json_id(const RequestId& id) {
    if (const auto* number = std::get_if<std::int64_t>(&id))
        return QJsonValue(static_cast<qint64>(*number));
    return QString::fromStdString(std::get<std::string>(id));
}
QByteArray json(const QJsonObject& value) {
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}
struct Request {
    attention::Request core;
    QJsonObject details;
    QJsonArray questions;
    QString method;
};
void parse_approval(Request& result, const QJsonObject& params) {
    result.core.reason = "Command approval";
    result.details.insert("command", params.value("command"));
    result.details.insert("cwd", params.value("cwd"));
    result.details.insert("reason", params.value("reason"));
    // Other decision variants (for example accepting for a session) remain
    // observable but are deliberately not exposed by this first adapter.
    const auto available = params.value("availableDecisions");
    if (!available.isArray())
        throw std::runtime_error("Approval lacks qualified decision list");
    for (const auto& choice : available.toArray()) {
        if (!choice.isString())
            continue;
        const auto value = choice.toString().toStdString();
        if ((value == "accept" || value == "decline" || value == "cancel") &&
            std::find(result.core.choices.begin(), result.core.choices.end(), value) ==
                result.core.choices.end())
            result.core.choices.push_back(value);
    }
}
void parse_questions(Request& result, const QJsonObject& params) {
    result.core.reason = "User input";
    const auto questions = params.value("questions");
    if (!questions.isArray() || questions.toArray().isEmpty() || questions.toArray().size() > 16)
        throw std::runtime_error("Invalid question set");
    std::set<QString> ids;
    for (const auto& entry : questions.toArray()) {
        const auto question = entry.toObject();
        const auto id_text = question.value("id").toString();
        const auto options = question.value("options");
        if (!text(id_text, 256) || !ids.insert(id_text).second ||
            !text(question.value("question").toString(), 8192) ||
            !(options.isNull() || options.isArray()))
            throw std::runtime_error("Invalid question payload");
        if (options.toArray().size() > 32)
            throw std::runtime_error("Too many question options");
        std::set<QString> labels;
        for (const auto& option : options.toArray()) {
            const auto label = option.toObject().value("label").toString();
            if (!text(label, 8192) || !labels.insert(label).second)
                throw std::runtime_error("Invalid question option");
        }
        result.questions.append(question);
    }
    result.details.insert("questions", result.questions);
    result.core.choices = {"submit"};
}
Request parse_request(const QJsonObject& message) {
    Request result;
    const auto id = request_id(message.value("id"));
    const auto params = message.value("params").toObject();
    const auto thread = params.value("threadId").toString();
    if (!id || !text(thread, 256))
        throw std::runtime_error("Source request has invalid identity");
    result.core.id = *id;
    result.core.thread_id = thread.toStdString();
    result.method = message.value("method").toString();
    for (const auto* name : {"turnId", "itemId"}) {
        const auto value = params.value(name);
        if (!value.isUndefined() && (!value.isString() || !text(value.toString(), 256)))
            throw std::runtime_error("Source request has invalid context identity");
    }
    result.core.turn_id = params.value("turnId").toString().toStdString();
    result.core.item_id = params.value("itemId").toString().toStdString();
    result.details.insert("method", result.method);
    if (result.method == "item/commandExecution/requestApproval") {
        parse_approval(result, params);
    } else if (result.method == "item/tool/requestUserInput") {
        parse_questions(result, params);
    } else {
        result.core.reason = "Respond in terminal";
        // Unqualified request types are observed only, never answered here.
    }
    if (json(result.details).size() > details_limit)
        throw std::runtime_error("Attention details exceeded limit");
    result.core.summary = result.core.reason;
    return result;
}
qsizetype details_cost(const Request& request) { return json(request.details).size(); }
bool valid_answers(const Request& request, const QJsonObject& answers) {
    if (answers.size() != request.questions.size())
        return false;
    for (const auto& value : request.questions) {
        const auto question = value.toObject();
        const auto submitted = answers.value(question.value("id").toString()).toObject();
        const auto values = submitted.value("answers");
        if (submitted.size() != 1 || !values.isArray() || values.toArray().size() != 1)
            return false;
        const auto answer = values.toArray().first();
        if (!answer.isString() || !text(answer.toString(), 8192))
            return false;
        const auto options = question.value("options").toArray();
        const bool free_text = options.isEmpty() || question.value("isOther").toBool();
        if (!free_text && std::none_of(options.begin(), options.end(), [&](const auto& option) {
                return option.toObject().value("label") == answer;
            }))
            return false;
    }
    return true;
}
Activity activity(const QJsonObject& status) {
    const auto type = status.value("type").toString();
    if (type == "active")
        return Activity::working;
    if (type == "idle")
        return Activity::idle;
    return Activity::unknown;
}
} // namespace

class Observer::Impl final : public QObject {
  public:
    Impl(attention::State& state, Observer& owner) : state_(state), owner_(owner) {
        deadline_.setSingleShot(true);
        retry_.setSingleShot(true);
        connect(&deadline_, &QTimer::timeout, this, [this] { fail("Codex RPC timed out"); });
        connect(&retry_, &QTimer::timeout, this, [this] { reconcile(); });
    }
    void start(const QString& socket, QStringView hash) {
        stop();
        thread_.clear();
        requests_.clear();
        retired_.clear();
        pending_details_bytes_ = 0;
        if (!Observer::qualifiedBinarySha256s().contains(hash)) {
            unsupported_source_ = true;
            diagnostic_ = "Unsupported Codex binary hash";
            emit owner_.changed();
            return;
        }
        unsupported_source_ = false;
        path_ = socket;
        reconnect();
    }
    void stop() {
        close();
        unsupported_source_ = false;
        path_.clear();
        diagnostic_ = "Codex observer stopped";
        emit owner_.changed();
    }
    void reconnect() {
        close();
        if (unsupported_source_) {
            emit owner_.changed();
            return;
        }
        if (path_.isEmpty() || state_.epoch() == std::numeric_limits<quint64>::max()) {
            fail("Codex source cannot reconnect");
            return;
        }
        state_.connect(state_.epoch() + 1, {true, true, true});
        sequence_ = 0;
        retired_.clear();
        initialized_ = false;
        discovering_ = true;
        discovery_.clear();
        background_.clear();
        diagnostic_ = "Connecting to Codex";
        transport_ = new UnixWebSocket(this);
        connect(transport_, &UnixWebSocket::opened, this, [this] {
            rpc("initialize", {{"clientInfo", QJsonObject{{"name", "lapis"}, {"version", "0.1"}}},
                               {"capabilities", QJsonObject{{"experimentalApi", true}}}});
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
    [[nodiscard]] const QString& diagnostic() const { return diagnostic_; }
    [[nodiscard]] const QString& thread_id() const { return thread_; }
    [[nodiscard]] QJsonObject details(const RequestId& id) const {
        const auto entry = requests_.find(id);
        return entry == requests_.end() ? QJsonObject{} : entry->second.details;
    }
    bool decide(quint64 epoch, const RequestId& id, quint64 revision, const QString& choice,
                const QJsonObject& answers) {
        const auto entry = requests_.find(id);
        if (!state_.ready() || !transport_ || entry == requests_.end() || epoch != state_.epoch())
            return false;
        const auto& request = entry->second;
        if (std::find(request.core.choices.begin(), request.core.choices.end(),
                      choice.toStdString()) == request.core.choices.end())
            return false;
        QJsonObject result;
        if (request.method == "item/tool/requestUserInput") {
            if (!valid_answers(request, answers))
                return false;
            result.insert("answers", answers);
        } else {
            if (!answers.isEmpty())
                return false;
            result.insert("decision", choice);
        }
        const auto bytes = json({{"id", json_id(id)}, {"result", result}});
        if (bytes.size() > message_limit)
            return false;
        try {
            if (!state_.respond(epoch, id, revision, choice.toStdString()))
                return false;
        } catch (const std::exception& error) {
            fail(QString::fromUtf8(error.what()));
            return false;
        }
        if (!transport_->send(bytes)) {
            fail("Codex response delivery is uncertain; reconnect before deciding again");
            return false;
        }
        emit owner_.changed();
        return true;
    }

  private:
    void close() {
        deadline_.stop();
        retry_.stop();
        if (transport_) {
            transport_->disconnect(this);
            transport_->close();
            transport_->deleteLater();
            transport_ = nullptr;
        }
        state_.disconnect();
        waiting_.clear();
        replay_.clear();
        replay_bytes_ = 0;
        unclassified_.clear();
        unclassified_total_ = {};
        recovering_ = false;
    }
    void fail(const QString& reason) {
        close();
        diagnostic_ = reason;
        emit owner_.changed();
    }
    attention::Position next() {
        if (sequence_ == std::numeric_limits<quint64>::max())
            throw std::runtime_error("Codex source sequence exhausted");
        return {state_.epoch(), ++sequence_};
    }
    void check(attention::Outcome outcome) {
        if (outcome != attention::Outcome::applied && outcome != attention::Outcome::duplicate)
            throw std::runtime_error("Codex attention lost synchronization");
    }
    bool bind(const QString& thread) {
        if (!text(thread, 256) || (!thread_.isEmpty() && thread_ != thread)) {
            fail("Codex observer requires one stable thread");
            return false;
        }
        thread_ = thread;
        return true;
    }
    void rpc(const QString& method, const QJsonObject& params) {
        if (!transport_ || !waiting_.isEmpty() || rpc_id_ == std::numeric_limits<qint64>::max()) {
            fail("Invalid Codex RPC state");
            return;
        }
        waiting_ = method;
        ++rpc_id_;
        deadline_.start(rpc_timeout);
        if (!transport_->send(json({{"id", rpc_id_}, {"method", method}, {"params", params}})))
            fail("Codex RPC write failed");
    }
    void reconcile() {
        if (!initialized_ || !transport_ || !waiting_.isEmpty())
            return;
        state_.overflow();
        recovering_ = true;
        // Discovery events are not the authoritative replay. Start fresh after
        // binding the persistent thread, before issuing resume/read.
        replay_.clear();
        replay_bytes_ = 0;
        // Retries for a thread without a rollout (no first turn yet) keep that
        // diagnostic instead of alternating with this one every second.
        if (diagnostic_ != waiting_for_history)
            diagnostic_ = "Reconciling Codex requests";
        rpc("thread/resume", {{"threadId", thread_}, {"excludeTurns", true}});
        emit owner_.changed();
    }
    void queue(const QJsonObject& message, qsizetype size) {
        if (replay_.size() >= 1024 || replay_bytes_ + size > UnixWebSocket::maximum_message_bytes)
            throw std::runtime_error("Codex replay exceeded limit");
        replay_.push_back(message);
        replay_bytes_ += size;
    }
    void resolved(const RequestId& id) {
        if (!retired_.contains(id) && retired_.size() >= 1024)
            throw std::runtime_error("Codex retired request limit reached");
        retired_.insert(id);
        const auto request = requests_.find(id);
        if (request != requests_.end())
            pending_details_bytes_ -= details_cost(request->second);
        requests_.erase(id);
        check(state_.resolve(next(), id));
    }
    void apply_event(const QJsonObject& message) {
        const auto method = message.value("method").toString();
        const auto params = message.value("params").toObject();
        if (message.contains("id")) {
            const auto request = parse_request(message);
            if (retired_.contains(request.core.id))
                return;
            const auto old = requests_.find(request.core.id);
            if (old != requests_.end() && old->second.details != request.details)
                throw std::runtime_error("Conflicting Codex request replay");
            const auto details_bytes = details_cost(request);
            if (old == requests_.end() &&
                pending_details_bytes_ + details_bytes > aggregate_details_limit)
                throw std::runtime_error("Codex pending attention details exceeded limit");
            check(state_.request(next(), request.core, now()));
            if (old == requests_.end())
                pending_details_bytes_ += details_bytes;
            requests_[request.core.id] = request;
        } else if (method == "serverRequest/resolved") {
            const auto id = request_id(params.value("requestId"));
            if (!id)
                throw std::runtime_error("Invalid resolved request identity");
            resolved(*id);
        } else if (method == "turn/started") {
            check(state_.activity(next(), Activity::working));
        } else if (method == "turn/completed") {
            const auto status = params.value("turn").toObject().value("status").toString();
            check(state_.activity(next(), status == "completed" ? Activity::turn_completed
                                                                : Activity::idle));
        } else if (method == "thread/status/changed") {
            check(state_.activity(next(), activity(params.value("status").toObject())));
        }
    }
    void finish(const QJsonObject& thread) {
        if (thread.value("id").toString() != thread_)
            throw std::runtime_error("Reconciliation returned another Codex thread");
        std::map<RequestId, Request> replacement;
        auto resolved_ids = retired_;
        for (const auto& message : replay_) {
            if (message.value("method") == "serverRequest/resolved") {
                const auto id = request_id(message.value("params").toObject().value("requestId"));
                if (!id)
                    throw std::runtime_error("Invalid replay resolution");
                resolved_ids.insert(*id);
            } else if (message.contains("id")) {
                const auto request = parse_request(message);
                const auto previous = replacement.find(request.core.id);
                if (previous != replacement.end() && (previous->second.core != request.core ||
                                                      previous->second.details != request.details))
                    throw std::runtime_error("Conflicting replay request");
                replacement[request.core.id] = request;
            }
        }
        // The core also retires requests omitted by a same-epoch snapshot.
        // Mirror that boundary so late replays cannot restore adapter details.
        for (const auto& [id, pending] : state_.pending())
            if (pending.source_epoch == state_.epoch() && !replacement.contains(id))
                resolved_ids.insert(id);
        if (resolved_ids.size() > 1024)
            throw std::runtime_error("Codex retired request limit reached");
        for (const auto& id : resolved_ids)
            replacement.erase(id);
        qsizetype replacement_details_bytes = 0;
        for (const auto& [id, request] : replacement) {
            static_cast<void>(id);
            replacement_details_bytes += details_cost(request);
            if (replacement_details_bytes > aggregate_details_limit)
                throw std::runtime_error("Codex pending attention details exceeded limit");
        }
        std::vector<attention::Request> pending;
        pending.reserve(replacement.size());
        for (const auto& [id, request] : replacement)
            pending.push_back(request.core);
        check(state_.reconcile(next(), pending, now()));
        requests_ = std::move(replacement);
        pending_details_bytes_ = replacement_details_bytes;
        retired_ = std::move(resolved_ids);
        check(state_.activity(next(), activity(thread.value("status").toObject())));
        // Record resolutions in the core after the authoritative replacement,
        // preventing subsequent late replays from recreating retired requests.
        for (const auto& id : retired_)
            check(state_.resolve(next(), id));
        recovering_ = false;
        replay_.clear();
        replay_bytes_ = 0;
        diagnostic_ = "Codex synchronized";
        emit owner_.changed();
    }
    void receive(const QByteArray& bytes) {
        const auto document = QJsonDocument::fromJson(bytes);
        if (!document.isObject())
            throw std::runtime_error("Invalid Codex JSON object");
        const auto message = document.object();
        if (message.contains("method"))
            receive_event(message, bytes.size());
        else
            receive_reply(message);
    }
    struct DeferredTraffic {
        std::size_t events{};
        qsizetype bytes{};
    };
    void defer_unclassified(const QString& id, qsizetype size) {
        if (unclassified_total_.events >= 1024 ||
            unclassified_total_.bytes + size > UnixWebSocket::maximum_message_bytes)
            throw std::runtime_error("Codex unclassified traffic exceeded limit");
        // An already-bound observer can never adopt this sender. Retain only
        // bounded accounting until metadata classifies it, not actionable payloads.
        auto& deferred = unclassified_[id];
        ++deferred.events;
        deferred.bytes += size;
        ++unclassified_total_.events;
        unclassified_total_.bytes += size;
    }
    void classified(const QString& id) {
        const auto found = unclassified_.find(id);
        if (found == unclassified_.end())
            return;
        unclassified_total_.events -= found->second.events;
        unclassified_total_.bytes -= found->second.bytes;
        unclassified_.erase(found);
    }
    bool classify_thread(const QJsonObject& thread) {
        const auto id = thread.value("id").toString();
        if (!text(id, 256) || !thread.value("ephemeral").isBool())
            throw std::runtime_error("Invalid Codex thread metadata");
        classified(id);
        if (!thread.value("ephemeral").toBool())
            return bind(id);
        if (id == thread_ || (!background_.contains(id) && background_.size() >= 128))
            throw std::runtime_error("Invalid Codex temporary thread set");
        background_.insert(id);
        return true;
    }
    void discover_next() {
        if (discovery_.isEmpty()) {
            discovering_ = false;
            if (!thread_.isEmpty())
                reconcile();
            else {
                diagnostic_ = "Waiting for a persistent Codex thread";
                emit owner_.changed();
            }
            return;
        }
        discovery_id_ = discovery_.takeFirst();
        rpc("thread/read", {{"threadId", discovery_id_}, {"includeTurns", false}});
    }
    void receive_event(const QJsonObject& message, qsizetype size) {
        const auto method = message.value("method").toString();
        const auto params = message.value("params").toObject();
        if (method == "thread/started") {
            if (classify_thread(params.value("thread").toObject()) && initialized_ &&
                !discovering_ && !params.value("thread").toObject().value("ephemeral").toBool())
                reconcile();
            return;
        }
        const auto source_thread = params.value("threadId");
        if (source_thread.isUndefined() && !message.contains("id"))
            return;
        if (!source_thread.isString() || !text(source_thread.toString(), 256)) {
            fail("Codex event lacks thread identity: " + method);
            return;
        }
        if (background_.contains(source_thread.toString())) {
            if (method == "thread/closed")
                background_.erase(source_thread.toString());
            return;
        }
        if (source_thread.toString() == thread_ &&
            (method == "thread/closed" || method == "thread/archived")) {
            fail("Codex thread closed; restore the source before reconnecting");
            return;
        }
        if (!thread_.isEmpty() && source_thread.toString() != thread_) {
            defer_unclassified(source_thread.toString(), size);
            return;
        }
        // Only classified persistent metadata can establish the initial binding.
        // Early events remain bounded and non-actionable until resume/read
        // provides the authoritative replay for that thread.
        if (discovering_ || thread_.isEmpty()) {
            queue(message, size);
            return;
        }
        if (!bind(source_thread.toString()))
            return;
        if (recovering_ || !state_.ready()) {
            queue(message, size);
            return;
        }
        apply_event(message);
        emit owner_.changed();
        return;
    }
    void receive_reply(const QJsonObject& message) {
        const auto id = request_id(message.value("id"));
        if (!id || *id != RequestId{rpc_id_} || waiting_.isEmpty())
            throw std::runtime_error("Unexpected Codex RPC reply");
        const auto method = std::exchange(waiting_, {});
        deadline_.stop();
        if (message.contains("error")) {
            const auto error = message.value("error").toObject();
            // This exact error is part of the qualified binary contract; a new
            // binary must requalify it rather than broadening retry eligibility.
            if (method == "thread/resume" && error.value("code").toInteger() == -32600 &&
                error.value("message").toString() == "no rollout found for thread id " + thread_) {
                recovering_ = false;
                replay_.clear();
                replay_bytes_ = 0;
                diagnostic_ = waiting_for_history;
                retry_.start(1000);
                emit owner_.changed();
                return;
            }
            throw std::runtime_error("Codex reconciliation RPC failed");
        }
        const auto result = message.value("result");
        if (!result.isObject())
            throw std::runtime_error("Invalid Codex RPC result");
        receive_result(method, result.toObject());
    }
    void loaded_threads(const QJsonObject& result) {
        const auto data = result.value("data");
        if (!data.isArray() || data.toArray().size() > 128 ||
            !result.value("nextCursor").toString().isEmpty())
            throw std::runtime_error("Codex thread discovery exceeded limit");
        for (const auto& value : data.toArray()) {
            if (!value.isString() || !text(value.toString(), 256))
                throw std::runtime_error("Invalid loaded Codex thread identity");
            discovery_.append(value.toString());
        }
        discover_next();
    }
    void receive_result(const QString& method, const QJsonObject& result) {
        if (method == "initialize") {
            if (!result.value("userAgent").isString())
                throw std::runtime_error("Invalid Codex initialization");
            if (!transport_->send(json({{"method", "initialized"}})))
                throw std::runtime_error("Codex initialization write failed");
            initialized_ = true;
            QPointer<Impl> alive(this);
            QPointer<UnixWebSocket> current = transport_;
            emit owner_.initialized();
            if (alive && current && transport_ == current)
                rpc("thread/loaded/list", {{"limit", 128}});
        } else if (method == "thread/loaded/list") {
            loaded_threads(result);
        } else if (method == "thread/resume") {
            if (result.value("thread").toObject().value("id").toString() != thread_)
                throw std::runtime_error("Codex resume returned another thread");
            rpc("thread/read", {{"threadId", thread_}, {"includeTurns", true}});
        } else if (method == "thread/read") {
            const auto thread = result.value("thread").toObject();
            if (discovering_) {
                if (thread.value("id").toString() != discovery_id_)
                    throw std::runtime_error("Discovery returned another Codex thread");
                if (classify_thread(thread))
                    discover_next();
            } else
                finish(thread);
        }
    }
    attention::State& state_;
    Observer& owner_;
    QPointer<UnixWebSocket> transport_;
    QTimer deadline_;
    QTimer retry_;
    QString path_;
    QString thread_;
    QString diagnostic_;
    QString waiting_;
    bool unsupported_source_{};
    qint64 rpc_id_{};
    quint64 sequence_{};
    bool initialized_{};
    bool discovering_{};
    QStringList discovery_;
    QString discovery_id_;
    std::set<QString> background_;
    std::map<QString, DeferredTraffic> unclassified_;
    DeferredTraffic unclassified_total_;
    bool recovering_{};
    qsizetype replay_bytes_{};
    std::vector<QJsonObject> replay_;
    std::map<RequestId, Request> requests_;
    qsizetype pending_details_bytes_{};
    std::set<RequestId> retired_;
};
Observer::Observer(attention::State& state, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(state, *this)) {}
Observer::~Observer() = default;
// Updating this pin requires the live requalification procedure in
// adapters/codex/README.md; a version string alone is insufficient.
QStringList Observer::qualifiedBinarySha256s() {
    // Codex 0.155.1 standalone builds, each qualified live on the machine that ran it.
    return {QStringLiteral("81f1d50b0153837534552c7033f203c99a34d2e1fb3fdadc4ec6002fd834180c"),
            QStringLiteral("8eaf1ad12fe6bf89b1710330f58900014322c7c5af677e43be116d8ac5fc0a9e")};
}
QString Observer::qualifiedBinarySha256() { return qualifiedBinarySha256s().front(); }
void Observer::start(const QString& socket, const QString& hash) { impl_->start(socket, hash); }
void Observer::reconnect() { impl_->reconnect(); }
void Observer::stop() { impl_->stop(); }
QString Observer::diagnostic() const { return impl_->diagnostic(); }
QString Observer::threadId() const { return impl_->thread_id(); }
QJsonObject Observer::details(const RequestId& id) const { return impl_->details(id); }
bool Observer::decide(quint64 epoch, const RequestId& id, quint64 revision, const QString& choice,
                      const QJsonObject& answers) {
    return impl_->decide(epoch, id, revision, choice, answers);
}
} // namespace lapis::codex
