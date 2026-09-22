#ifndef LAPIS_SESSION_TRANSPORT_WIRE_BYTES_HPP
#define LAPIS_SESSION_TRANSPORT_WIRE_BYTES_HPP

#include "local_protocol.hpp"
#include <stdexcept>

// Internal big-endian primitives. Callers must check the available byte count
// before reading: fixed-size local frames and the attention Reader do so.
namespace lapis::session::wire::bytes {
inline void append_quint32(QByteArray& bytes, quint32 value) {
    bytes.append(static_cast<char>(value >> 24U));
    bytes.append(static_cast<char>(value >> 16U));
    bytes.append(static_cast<char>(value >> 8U));
    bytes.append(static_cast<char>(value));
}
inline void append_quint64(QByteArray& bytes, quint64 value) {
    append_quint32(bytes, static_cast<quint32>(value >> 32U));
    append_quint32(bytes, static_cast<quint32>(value));
}
inline QByteArray raw_bytes(const unsigned char*& cursor, qsizetype size) {
    QByteArray result(reinterpret_cast<const char*>(cursor), size);
    cursor += size;
    return result;
}
inline quint32 read_quint32(const unsigned char*& cursor) {
    const quint32 value = (quint32{cursor[0]} << 24U) | (quint32{cursor[1]} << 16U) |
                          (quint32{cursor[2]} << 8U) | quint32{cursor[3]};
    cursor += 4;
    return value;
}
inline quint64 read_quint64(const unsigned char*& cursor) {
    const quint64 high = read_quint32(cursor);
    return (high << 32U) | read_quint32(cursor);
}
inline QByteArray encode_attachment(const Attachment& attachment) {
    if (!valid_identity(attachment.identity) || attachment.generation == 0)
        throw std::runtime_error("Invalid attachment identity");
    QByteArray result;
    result += attachment.identity.session_id;
    result += attachment.identity.epoch;
    append_quint64(result, attachment.generation);
    return result;
}
inline Attachment decode_attachment(const unsigned char*& cursor) {
    Attachment result;
    result.identity.session_id = raw_bytes(cursor, 16);
    result.identity.epoch = raw_bytes(cursor, 16);
    result.generation = read_quint64(cursor);
    if (!valid_identity(result.identity) || result.generation == 0)
        throw std::runtime_error("Invalid attachment identity");
    return result;
}
} // namespace lapis::session::wire::bytes

#endif
