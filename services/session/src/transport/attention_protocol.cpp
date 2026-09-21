#include "attention_protocol.hpp"
#include "wire_bytes.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <set>
#include <stdexcept>
#include <utility>

namespace lapis::session::wire {
using namespace bytes;
namespace {
constexpr quint32 max_snapshot_bytes = 1024U * 1024U;
constexpr quint32 max_decision_bytes = 64U * 1024U;
constexpr quint32 max_requests = 128;
constexpr quint32 max_metadata_bytes = 1024;
constexpr quint32 max_summary_bytes = 4096;
constexpr quint32 max_diagnostic_bytes = 4096;
constexpr quint32 max_choices = 32;
constexpr quint32 max_choice_bytes = 256;
constexpr quint32 max_json_bytes = 32U * 1024U;

void check(bool valid) {
    if (!valid)
        throw std::runtime_error("Invalid attention message");
}

void append_bool(QByteArray& bytes, bool value) { bytes.append(static_cast<char>(value ? 1 : 0)); }

QByteArray valid_utf8(const QByteArray& value, quint32 limit) {
    check(value.size() <= static_cast<qsizetype>(limit));
    check(QString::fromUtf8(value).toUtf8() == value);
    return value;
}

void append_utf8(QByteArray& bytes, const QByteArray& value, quint32 limit) {
    valid_utf8(value, limit);
    append_quint32(bytes, static_cast<quint32>(value.size()));
    bytes += value;
}

void append_string(QByteArray& bytes, const QString& value, quint32 limit) {
    append_utf8(bytes, value.toUtf8(), limit);
}

void append_string(QByteArray& bytes, const std::string& value, quint32 limit) {
    check(value.size() <= static_cast<size_t>(limit));
    append_utf8(bytes, QByteArray::fromStdString(value), limit);
}

void append_json(QByteArray& bytes, const QJsonObject& value) {
    const QByteArray encoded = QJsonDocument(value).toJson(QJsonDocument::Compact);
    check(encoded.size() <= static_cast<qsizetype>(max_json_bytes));
    const auto reparsed = QJsonDocument::fromJson(encoded);
    check(reparsed.isObject() && reparsed.object() == value &&
          QJsonDocument(reparsed.object()).toJson(QJsonDocument::Compact) == encoded);
    append_quint32(bytes, static_cast<quint32>(encoded.size()));
    bytes += encoded;
}

void append_request_id(QByteArray& bytes, const attention::RequestId& id) {
    if (const auto* numeric = std::get_if<std::int64_t>(&id)) {
        bytes.append('\0');
        append_quint64(bytes, static_cast<quint64>(*numeric));
        return;
    }
    bytes.append('\1');
    append_string(bytes, std::get<std::string>(id), max_metadata_bytes);
}

bool valid_utf8_string(const std::string& value, quint32 limit) {
    return value.size() <= limit &&
           QString::fromUtf8(QByteArray::fromStdString(value)).toStdString() == value;
}

bool valid_request_id(const attention::RequestId& id) {
    const auto* text = std::get_if<std::string>(&id);
    return !text || (!text->empty() && valid_utf8_string(*text, max_metadata_bytes));
}

bool valid_request(const attention::Request& request) {
    if (!valid_request_id(request.id) ||
        !valid_utf8_string(request.thread_id, max_metadata_bytes) ||
        !valid_utf8_string(request.turn_id, max_metadata_bytes) ||
        !valid_utf8_string(request.item_id, max_metadata_bytes) ||
        !valid_utf8_string(request.reason, max_metadata_bytes) || request.reason.empty() ||
        !valid_utf8_string(request.summary, max_summary_bytes) || request.priority > 3 ||
        request.choices.size() > max_choices)
        return false;
    std::set<std::string> choices;
    for (const auto& choice : request.choices)
        if (choice.empty() || !valid_utf8_string(choice, max_choice_bytes) ||
            !choices.insert(choice).second)
            return false;
    return true;
}

bool valid_pending(const attention::Pending& pending) {
    return valid_request(pending.request) &&
           static_cast<quint8>(pending.status) <=
               static_cast<quint8>(attention::RequestStatus::stale) &&
           pending.revision != 0 && pending.source_epoch != 0;
}

bool valid_snapshot(const AttentionSnapshot& snapshot) {
    if (!valid_identity(snapshot.attachment.identity) || snapshot.attachment.generation == 0 ||
        snapshot.requests.size() > max_requests ||
        static_cast<quint8>(snapshot.activity) >
            static_cast<quint8>(attention::Activity::turn_completed) ||
        snapshot.diagnostic.toUtf8().size() > max_diagnostic_bytes)
        return false;
    if (snapshot.ready &&
        (!snapshot.available || !snapshot.connected || snapshot.source_epoch == 0))
        return false;
    if ((!snapshot.available && (snapshot.connected || !snapshot.requests.empty())) ||
        (snapshot.connected && snapshot.source_epoch == 0))
        return false;
    std::set<attention::RequestId> identities;
    for (const auto& item : snapshot.requests)
        if (!valid_pending(item.pending) || !identities.insert(item.pending.request.id).second ||
            item.pending.source_epoch > snapshot.source_epoch ||
            (snapshot.ready && item.pending.source_epoch != snapshot.source_epoch))
            return false;
    return true;
}

bool valid_decision(const AttentionDecision& decision) {
    return decision.source_epoch != 0 && decision.revision != 0 &&
           valid_request_id(decision.request_id) && !decision.choice.isEmpty() &&
           decision.choice.toUtf8().size() <= max_choice_bytes;
}

class Reader {
  public:
    explicit Reader(const QByteArray& bytes)
        : data_(reinterpret_cast<const unsigned char*>(bytes.constData())), size_(bytes.size()) {}

    [[nodiscard]] bool done() const { return offset_ == size_; }
    [[nodiscard]] qsizetype remaining() const { return size_ - offset_; }

    const unsigned char* take(qsizetype size) {
        check(size >= 0 && remaining() >= size);
        const unsigned char* result = data_ + offset_;
        offset_ += size;
        return result;
    }

    quint8 quint8_value() {
        const auto* cursor = take(1);
        return static_cast<quint8>(*cursor);
    }

    quint32 quint32_value() {
        const auto* cursor = take(4);
        return read_quint32(cursor);
    }

    quint64 quint64_value() {
        const auto* cursor = take(8);
        return read_quint64(cursor);
    }

    QByteArray bytes(quint32 limit) {
        const quint32 length = quint32_value();
        check(length <= limit && length <= static_cast<quint32>(remaining()));
        return QByteArray(reinterpret_cast<const char*>(take(length)),
                          static_cast<qsizetype>(length));
    }

    QString string(quint32 limit) { return QString::fromUtf8(valid_utf8(bytes(limit), limit)); }

    std::string std_string(quint32 limit) {
        return QString::fromUtf8(valid_utf8(bytes(limit), limit)).toStdString();
    }

    bool bool_value() {
        const quint8 value = quint8_value();
        check(value <= 1);
        return value != 0;
    }

    Attachment attachment() {
        const auto* cursor = take(40);
        return decode_attachment(cursor);
    }

    QJsonObject json_object() {
        const QByteArray value = bytes(max_json_bytes);
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(value, &error);
        check(error.error == QJsonParseError::NoError && document.isObject() &&
              QJsonDocument(document.object()).toJson(QJsonDocument::Compact) == value);
        return document.object();
    }

  private:
    const unsigned char* data_;
    qsizetype size_;
    qsizetype offset_{};
};

attention::RequestId request_id(Reader& reader) {
    const quint8 type = reader.quint8_value();
    check(type <= 1);
    if (type == 0)
        return static_cast<std::int64_t>(reader.quint64_value());
    return reader.std_string(max_metadata_bytes);
}

void append_request(QByteArray& bytes, const AttentionItem& item) {
    check(valid_pending(item.pending));
    const auto& request = item.pending.request;
    append_request_id(bytes, request.id);
    append_string(bytes, request.thread_id, max_metadata_bytes);
    append_string(bytes, request.turn_id, max_metadata_bytes);
    append_string(bytes, request.item_id, max_metadata_bytes);
    append_string(bytes, request.reason, max_metadata_bytes);
    append_string(bytes, request.summary, max_summary_bytes);
    append_quint32(bytes, static_cast<quint32>(request.choices.size()));
    for (const auto& choice : request.choices)
        append_string(bytes, choice, max_choice_bytes);
    bytes.append(static_cast<char>(request.priority));
    append_json(bytes, item.details);
    bytes.append(static_cast<char>(item.pending.status));
    append_quint64(bytes, item.pending.revision);
    append_quint64(bytes, item.pending.arrived);
    append_quint64(bytes, item.pending.not_before);
    append_quint64(bytes, item.pending.source_epoch);
    append_bool(bytes, item.pending.submitted);
}

AttentionItem request(Reader& reader) {
    AttentionItem item;
    auto& pending = item.pending;
    pending.request.id = request_id(reader);
    pending.request.thread_id = reader.std_string(max_metadata_bytes);
    pending.request.turn_id = reader.std_string(max_metadata_bytes);
    pending.request.item_id = reader.std_string(max_metadata_bytes);
    pending.request.reason = reader.std_string(max_metadata_bytes);
    pending.request.summary = reader.std_string(max_summary_bytes);
    const quint32 choice_count = reader.quint32_value();
    check(choice_count <= max_choices);
    pending.request.choices.reserve(choice_count);
    for (quint32 index = 0; index < choice_count; ++index)
        pending.request.choices.push_back(reader.std_string(max_choice_bytes));
    pending.request.priority = reader.quint8_value();
    item.details = reader.json_object();
    pending.status = static_cast<attention::RequestStatus>(reader.quint8_value());
    pending.revision = reader.quint64_value();
    pending.arrived = reader.quint64_value();
    pending.not_before = reader.quint64_value();
    pending.source_epoch = reader.quint64_value();
    pending.submitted = reader.bool_value();
    return item;
}
} // namespace

QByteArray encode_attention_snapshot(const AttentionSnapshot& snapshot) {
    check(valid_snapshot(snapshot));
    QByteArray bytes;
    append_quint32(bytes, version);
    bytes += encode_attachment(snapshot.attachment);
    append_bool(bytes, snapshot.available);
    append_bool(bytes, snapshot.connected);
    append_bool(bytes, snapshot.ready);
    bytes.append(static_cast<char>(snapshot.activity));
    append_quint64(bytes, snapshot.source_epoch);
    append_string(bytes, snapshot.diagnostic, max_diagnostic_bytes);
    append_quint32(bytes, static_cast<quint32>(snapshot.requests.size()));
    for (const auto& item : snapshot.requests) {
        // Bound intermediate allocation too: individual valid records must not
        // accumulate into a multi-megabyte snapshot before rejection.
        QByteArray record;
        append_request(record, item);
        check(record.size() <= static_cast<qsizetype>(max_snapshot_bytes) - bytes.size());
        bytes += record;
    }
    check(bytes.size() <= static_cast<qsizetype>(max_snapshot_bytes));
    return bytes;
}

AttentionSnapshot decode_attention_snapshot(const QByteArray& payload) {
    check(payload.size() <= static_cast<qsizetype>(max_snapshot_bytes));
    Reader reader(payload);
    check(reader.quint32_value() == version);
    AttentionSnapshot result;
    result.attachment = reader.attachment();
    result.available = reader.bool_value();
    result.connected = reader.bool_value();
    result.ready = reader.bool_value();
    const auto activity = reader.quint8_value();
    check(activity <= static_cast<quint8>(attention::Activity::turn_completed));
    result.activity = static_cast<attention::Activity>(activity);
    result.source_epoch = reader.quint64_value();
    result.diagnostic = reader.string(max_diagnostic_bytes);
    const quint32 request_count = reader.quint32_value();
    check(request_count <= max_requests);
    result.requests.reserve(request_count);
    for (quint32 index = 0; index < request_count; ++index)
        result.requests.push_back(request(reader));
    check(reader.done() && valid_snapshot(result));
    return result;
}

QByteArray encode_attention_decision(const AttentionDecision& decision) {
    check(valid_decision(decision));
    QByteArray bytes;
    append_quint32(bytes, version);
    append_quint64(bytes, decision.source_epoch);
    append_request_id(bytes, decision.request_id);
    append_quint64(bytes, decision.revision);
    append_string(bytes, decision.choice, max_choice_bytes);
    append_json(bytes, decision.answers);
    check(bytes.size() <= static_cast<qsizetype>(max_decision_bytes));
    return bytes;
}

AttentionDecision decode_attention_decision(const QByteArray& payload) {
    check(payload.size() <= static_cast<qsizetype>(max_decision_bytes));
    Reader reader(payload);
    check(reader.quint32_value() == version);
    AttentionDecision result;
    result.source_epoch = reader.quint64_value();
    result.request_id = request_id(reader);
    result.revision = reader.quint64_value();
    result.choice = reader.string(max_choice_bytes);
    result.answers = reader.json_object();
    check(reader.done() && valid_decision(result));
    return result;
}
} // namespace lapis::session::wire
