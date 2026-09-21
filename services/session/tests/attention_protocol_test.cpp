#include "transport/attention_protocol.hpp"

#include <QJsonDocument>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using lapis::session::attention::Activity;
using lapis::session::attention::Pending;
using lapis::session::attention::Request;
using lapis::session::attention::RequestId;
using lapis::session::attention::RequestStatus;
using lapis::session::wire::AttentionDecision;
using lapis::session::wire::AttentionItem;
using lapis::session::wire::AttentionSnapshot;

constexpr quint32 max_metadata = 1024;
constexpr quint32 max_summary = 4096;
constexpr quint32 max_choices = 32;
constexpr quint32 max_choice = 256;
constexpr quint32 max_json = 32U * 1024U;
constexpr quint32 max_requests = 128;

void require(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("Attention protocol expectation failed at line " +
                                 std::to_string(where.line()));
}

template <typename Operation>
void rejects(Operation operation, std::source_location where = std::source_location::current()) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, where);
}

void put32(QByteArray& bytes, qsizetype offset, quint32 value) {
    bytes.replace(offset, 4,
                  QByteArray()
                      .append(static_cast<char>(value >> 24U))
                      .append(static_cast<char>(value >> 16U))
                      .append(static_cast<char>(value >> 8U))
                      .append(static_cast<char>(value)));
}

void put64(QByteArray& bytes, qsizetype offset, quint64 value) {
    put32(bytes, offset, static_cast<quint32>(value >> 32U));
    put32(bytes, offset + 4, static_cast<quint32>(value));
}

void put8(QByteArray& bytes, qsizetype offset, quint8 value) {
    bytes.replace(offset, 1, QByteArray(1, static_cast<char>(value)));
}

qsizetype diagnostic_end(const QByteArray& encoded) {
    const qsizetype offset = 56;
    const quint32 length = quint32{static_cast<quint8>(encoded[offset])} << 24U |
                           quint32{static_cast<quint8>(encoded[offset + 1])} << 16U |
                           quint32{static_cast<quint8>(encoded[offset + 2])} << 8U |
                           static_cast<quint8>(encoded[offset + 3]);
    return offset + 4 + static_cast<qsizetype>(length);
}

AttentionItem request(const RequestId& id, quint64 epoch) {
    AttentionItem result;
    result.pending.request = Request{id,
                                     std::string("thread"),
                                     std::string("turn"),
                                     std::string("item"),
                                     std::string("approval"),
                                     std::string("summary"),
                                     {std::string("yes"), std::string("no")},
                                     quint8{2}};
    result.details = QJsonObject{{QString::fromLatin1("risk"), 4}};
    result.pending = Pending{result.pending.request,
                             RequestStatus::responding,
                             quint64{17},
                             quint64{100},
                             quint64{200},
                             epoch,
                             true};
    return result;
}

AttentionSnapshot snapshot(std::vector<AttentionItem> requests = {}, bool ready = true) {
    AttentionSnapshot result;
    result.attachment = {{QByteArray(16, 1), QByteArray(16, 2)}, quint64{9}};
    result.available = true;
    result.connected = ready;
    result.ready = ready;
    result.source_epoch = ready ? quint64{20} : quint64{21};
    result.activity = Activity::working;
    result.diagnostic = QStringLiteral("synchronized");
    result.requests = std::move(requests);
    return result;
}

AttentionDecision decision(const RequestId& id) {
    return {quint64{20}, id, quint64{17}, QStringLiteral("yes"),
            QJsonObject{{QString::fromLatin1("answer"), QStringLiteral("ok")}}};
}

void round_trip_and_recovery() {
    AttentionSnapshot original =
        snapshot({request(std::int64_t{0}, 20), request(std::int64_t{-1}, 20),
                  request(std::string("id-é"), 20)});
    original.requests[2].pending.status = RequestStatus::stale;
    const QByteArray encoded = lapis::session::wire::encode_attention_snapshot(original);
    const AttentionSnapshot decoded = lapis::session::wire::decode_attention_snapshot(encoded);
    require(decoded.attachment == original.attachment && decoded.ready && decoded.available &&
            decoded.connected && decoded.source_epoch == 20 && decoded.requests.size() == 3);
    require(std::holds_alternative<std::int64_t>(decoded.requests[0].pending.request.id) &&
            std::get<std::int64_t>(decoded.requests[0].pending.request.id) == 0);
    require(std::holds_alternative<std::int64_t>(decoded.requests[1].pending.request.id) &&
            std::get<std::int64_t>(decoded.requests[1].pending.request.id) == -1);
    const auto* text = std::get_if<std::string>(&decoded.requests[2].pending.request.id);
    require(text && *text == "id-é" && decoded.requests[2].pending.status == RequestStatus::stale);
    require(lapis::session::wire::encode_attention_snapshot(decoded) == encoded);

    AttentionSnapshot recovery = snapshot({request(std::int64_t{0}, 20)}, false);
    recovery.connected = true;
    const QByteArray recovery_encoded = lapis::session::wire::encode_attention_snapshot(recovery);
    const AttentionSnapshot restored =
        lapis::session::wire::decode_attention_snapshot(recovery_encoded);
    require(restored.available && restored.connected && !restored.ready &&
            restored.source_epoch == 21);
    require(restored.requests.front().pending.source_epoch == 20 &&
            restored.requests.front().pending.status == RequestStatus::responding);
    require(lapis::session::wire::encode_attention_snapshot(restored) == recovery_encoded);

    const AttentionDecision original_decision = decision(std::numeric_limits<std::int64_t>::min());
    const QByteArray decision_encoded =
        lapis::session::wire::encode_attention_decision(original_decision);
    const AttentionDecision restored_decision =
        lapis::session::wire::decode_attention_decision(decision_encoded);
    require(restored_decision.request_id == original_decision.request_id &&
            restored_decision.source_epoch == 20 && restored_decision.revision == 17 &&
            restored_decision.choice == QStringLiteral("yes") &&
            restored_decision.answers == original_decision.answers);
    require(lapis::session::wire::encode_attention_decision(restored_decision) == decision_encoded);
}

void boundaries() {
    AttentionItem maximum = request(std::string(max_metadata, 'i'), 20);
    maximum.pending.request.thread_id = std::string(max_metadata, 't');
    maximum.pending.request.turn_id = std::string(max_metadata, 'r');
    maximum.pending.request.item_id = std::string(max_metadata, 'm');
    maximum.pending.request.reason = std::string(max_metadata, 'a');
    maximum.pending.request.summary = std::string(max_summary, 's');
    maximum.pending.request.choices.clear();
    for (quint32 index = 0; index < max_choices; ++index)
        maximum.pending.request.choices.emplace_back(max_choice, static_cast<char>('A' + index));
    const AttentionSnapshot one = snapshot({maximum});
    require(lapis::session::wire::decode_attention_snapshot(
                lapis::session::wire::encode_attention_snapshot(one))
                .requests.front()
                .pending.request == maximum.pending.request);

    AttentionSnapshot batch = snapshot();
    for (quint32 index = 0; index < max_requests; ++index)
        batch.requests.push_back(request(std::int64_t{index}, 20));
    const QByteArray batch_encoded = lapis::session::wire::encode_attention_snapshot(batch);
    require(batch_encoded.size() <= qsizetype{1024} * 1024 &&
            lapis::session::wire::decode_attention_snapshot(batch_encoded).requests.size() ==
                max_requests);
    batch.requests.push_back(request(std::string("overflow"), 20));
    rejects([&] { static_cast<void>(lapis::session::wire::encode_attention_snapshot(batch)); });

    AttentionSnapshot oversized = snapshot();
    for (quint32 index = 0; index < max_requests; ++index) {
        auto item = maximum;
        item.pending.request.id = std::int64_t{index};
        item.details = QJsonObject{{"text", QString(max_json - 20, 'x')}};
        oversized.requests.push_back(std::move(item));
    }
    rejects([&] { static_cast<void>(lapis::session::wire::encode_attention_snapshot(oversized)); });

    const QString json_key(max_json - 6, 'k');
    AttentionDecision bounded = decision(std::numeric_limits<std::int64_t>::max());
    bounded.choice = QString(max_choice, 'c');
    bounded.answers = QJsonObject{{json_key, 1}};
    const QByteArray bounded_encoded = lapis::session::wire::encode_attention_decision(bounded);
    const auto restored = lapis::session::wire::decode_attention_decision(bounded_encoded);
    require(restored.choice.size() == max_choice && restored.answers == bounded.answers);

    auto exceeded = bounded;
    exceeded.answers = QJsonObject{{QString(max_json - 5, 'k'), 1}};
    rejects([&] { static_cast<void>(lapis::session::wire::encode_attention_decision(exceeded)); });
}

void canonical_json_and_utf8() {
    AttentionSnapshot original = snapshot({request(std::string("id-é"), 20)});
    original.requests.front().details =
        QJsonObject{{QString::fromLatin1("aa"), QStringLiteral("é")}};
    const QByteArray encoded = lapis::session::wire::encode_attention_snapshot(original);
    require(encoded.contains(
        QJsonDocument(original.requests.front().details).toJson(QJsonDocument::Compact)));
    require(lapis::session::wire::decode_attention_snapshot(encoded).requests.front().details ==
            original.requests.front().details);

    AttentionDecision original_decision = {20, std::int64_t{0}, 17, QStringLiteral("oui-é"),
                                           QJsonObject{{QString::fromLatin1("aa"), 1}}};
    const QByteArray compact =
        QJsonDocument(original_decision.answers).toJson(QJsonDocument::Compact);
    const QByteArray decision_encoded =
        lapis::session::wire::encode_attention_decision(original_decision);
    QByteArray spaced = decision_encoded;
    spaced.replace(spaced.lastIndexOf(compact), compact.size(), QByteArrayLiteral("{\"a\" :1}"));
    require(spaced.size() == decision_encoded.size());
    rejects([&] { static_cast<void>(lapis::session::wire::decode_attention_decision(spaced)); });

    auto invalid_text = snapshot({request(std::string(1, '\xff'), 20)});
    rejects(
        [&] { static_cast<void>(lapis::session::wire::encode_attention_snapshot(invalid_text)); });
    invalid_text.requests.front().pending.request.id = std::int64_t{1};
    invalid_text.requests.front().pending.request.reason = std::string(1, '\xff');
    rejects(
        [&] { static_cast<void>(lapis::session::wire::encode_attention_snapshot(invalid_text)); });
    invalid_text.requests.front().pending.request.reason = "reason";
    invalid_text.requests.front().pending.request.summary = std::string(1, '\xff');
    rejects(
        [&] { static_cast<void>(lapis::session::wire::encode_attention_snapshot(invalid_text)); });
}

void golden_wire_bytes() {
    AttentionSnapshot minimal;
    minimal.attachment = {{QByteArray(16, 1), QByteArray(16, 2)}, quint64{1}};
    minimal.available = true;
    minimal.connected = true;
    minimal.ready = true;
    minimal.source_epoch = 1;
    minimal.activity = Activity::idle;
    require(lapis::session::wire::encode_attention_snapshot(minimal) ==
            QByteArray::fromHex("00000006"                         // wire version
                                "01010101010101010101010101010101" // session ID
                                "02020202020202020202020202020202" // service epoch
                                "0000000000000001"                 // attachment generation
                                "01010102"         // available, connected, ready, idle
                                "0000000000000001" // source epoch
                                "00000000"         // empty diagnostic
                                "00000000"));      // no requests

    AttentionDecision minimal_decision{quint64{1}, std::int64_t{0}, quint64{1}, QStringLiteral("y"),
                                       QJsonObject{}};
    require(lapis::session::wire::encode_attention_decision(minimal_decision) ==
            QByteArray::fromHex("00000006"         // wire version
                                "0000000000000001" // source epoch
                                "00"               // numeric ID tag
                                "0000000000000000" // request ID
                                "0000000000000001" // revision
                                "0000000179"       // choice: y
                                "000000027b7d"));  // answers: {}
}

void malformed_snapshots() {
    const QByteArray encoded =
        lapis::session::wire::encode_attention_snapshot(snapshot({request(0, 20)}));
    rejects([&] {
        static_cast<void>(lapis::session::wire::decode_attention_snapshot(encoded.chopped(1)));
    });
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_snapshot(encoded + 'x')); });

    auto bad_version = encoded;
    put32(bad_version, 0, 4);
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_snapshot(bad_version)); });

    auto bad_activity = encoded;
    put8(bad_activity, 47, 4);
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_snapshot(bad_activity)); });

    auto bad_ready = encoded;
    put8(bad_ready, 45, 0);
    put8(bad_ready, 46, 1);
    rejects([&] { static_cast<void>(lapis::session::wire::decode_attention_snapshot(bad_ready)); });

    auto ready_wrong_epoch = encoded;
    put64(ready_wrong_epoch, 48, 22);
    rejects([&] {
        static_cast<void>(lapis::session::wire::decode_attention_snapshot(ready_wrong_epoch));
    });

    auto bad_status = encoded;
    put8(bad_status, encoded.size() - 34, 3);
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_snapshot(bad_status)); });

    auto bad_bool = encoded;
    put8(bad_bool, encoded.size() - 1, 2);
    rejects([&] { static_cast<void>(lapis::session::wire::decode_attention_snapshot(bad_bool)); });

    QByteArray empty = lapis::session::wire::encode_attention_snapshot(snapshot());
    put32(empty, diagnostic_end(empty), 1);
    rejects([&] { static_cast<void>(lapis::session::wire::decode_attention_snapshot(empty)); });

    auto duplicate = snapshot({request(0, 20), request(1, 20)});
    duplicate.requests[1].pending.request.id = std::int64_t{0};
    rejects([&] { static_cast<void>(lapis::session::wire::encode_attention_snapshot(duplicate)); });

    auto zero_revision = snapshot({request(0, 20)});
    zero_revision.requests.front().pending.revision = 0;
    rejects(
        [&] { static_cast<void>(lapis::session::wire::encode_attention_snapshot(zero_revision)); });

    auto zero_pending_epoch = zero_revision;
    zero_pending_epoch.requests.front().pending.revision = 1;
    zero_pending_epoch.requests.front().pending.source_epoch = 0;
    rejects([&] {
        static_cast<void>(lapis::session::wire::encode_attention_snapshot(zero_pending_epoch));
    });

    auto invalid_status = snapshot({request(0, 20)});
    invalid_status.requests.front().pending.status =
        static_cast<RequestStatus>(static_cast<quint8>(RequestStatus::stale) + 1);
    rejects([&] {
        static_cast<void>(lapis::session::wire::encode_attention_snapshot(invalid_status));
    });

    auto invalid_activity = snapshot();
    invalid_activity.activity = static_cast<Activity>(4);
    rejects([&] {
        static_cast<void>(lapis::session::wire::encode_attention_snapshot(invalid_activity));
    });
}

void malformed_decisions() {
    const QByteArray encoded =
        lapis::session::wire::encode_attention_decision(decision(std::int64_t{0}));
    rejects([&] {
        static_cast<void>(lapis::session::wire::decode_attention_decision(encoded.chopped(1)));
    });
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_decision(encoded + 'x')); });

    auto bad_version = encoded;
    put32(bad_version, 0, 4);
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_decision(bad_version)); });

    auto zero_epoch = encoded;
    put64(zero_epoch, 4, 0);
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_decision(zero_epoch)); });

    auto zero_revision = encoded;
    put64(zero_revision, 21, 0);
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_decision(zero_revision)); });

    auto bad_id_type = encoded;
    put8(bad_id_type, 12, 2);
    rejects(
        [&] { static_cast<void>(lapis::session::wire::decode_attention_decision(bad_id_type)); });

    auto oversized_choice = encoded;
    put32(oversized_choice, 29, max_choice + 1);
    rejects([&] {
        static_cast<void>(lapis::session::wire::decode_attention_decision(oversized_choice));
    });

    auto zero_choice = decision(std::int64_t{0});
    zero_choice.choice.clear();
    rejects(
        [&] { static_cast<void>(lapis::session::wire::encode_attention_decision(zero_choice)); });
}
} // namespace

int main() {
    try {
        round_trip_and_recovery();
        boundaries();
        canonical_json_and_utf8();
        golden_wire_bytes();
        malformed_snapshots();
        malformed_decisions();
        std::cout << "Attention v6 codec boundary and recovery checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
