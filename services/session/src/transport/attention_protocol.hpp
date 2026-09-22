#ifndef LAPIS_SESSION_ATTENTION_PROTOCOL_HPP
#define LAPIS_SESSION_ATTENTION_PROTOCOL_HPP

#include "local_protocol.hpp"
#include <QJsonObject>
#include <lapis/session/attention.hpp>
#include <vector>

namespace lapis::session::wire {
struct AttentionItem {
    attention::Pending pending;
    QJsonObject details;
};
struct AttentionSnapshot {
    Attachment attachment;
    bool available{};
    bool connected{};
    bool ready{};
    quint64 source_epoch{};
    attention::Activity activity{attention::Activity::unknown};
    QString diagnostic;
    std::vector<AttentionItem> requests;
};
// Carried inside the existing attachment-bound control envelope.
struct AttentionDecision {
    quint64 source_epoch{};
    attention::RequestId request_id;
    quint64 revision{};
    QString choice;
    QJsonObject answers;
};
[[nodiscard]] QByteArray encode_attention_snapshot(const AttentionSnapshot& snapshot);
[[nodiscard]] AttentionSnapshot decode_attention_snapshot(const QByteArray& payload);
[[nodiscard]] QByteArray encode_attention_decision(const AttentionDecision& decision);
[[nodiscard]] AttentionDecision decode_attention_decision(const QByteArray& payload);
} // namespace lapis::session::wire
#endif
