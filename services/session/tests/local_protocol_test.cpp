#include "transport/local_protocol.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("Protocol expectation failed");
}
template <typename Operation> void rejects(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected);
}
} // namespace
int main() {
    using namespace lapis::session;
    try {
        Terminal terminal({20, 4});
        terminal.feed("A界é\x1b[1;31mZ\x1b[0m\x1b[?2004h");
        const auto expected = terminal.snapshot();
        const auto encoded = wire::encode_snapshot(expected);
        const auto decoded = wire::decode_snapshot(encoded);
        require(wire::encode_snapshot(decoded) == encoded);
        require(decoded.size == expected.size && decoded.bracketed_paste);
        require(decoded.graphemes == expected.graphemes);
        require(decoded.cells.at(4).style == expected.cells.at(4).style);

        const auto packet = wire::frame(wire::Kind::snapshot, encoded);
        QByteArray buffer;
        wire::Frame frame;
        for (qsizetype i = 0; i < packet.size() - 1; ++i) {
            buffer += packet[i];
            require(!wire::take_frame(buffer, frame));
        }
        buffer += packet.back();
        buffer += wire::frame(wire::Kind::text, "next");
        require(wire::take_frame(buffer, frame) && frame.kind == wire::Kind::snapshot &&
                frame.payload == encoded);
        require(wire::take_frame(buffer, frame) && frame.kind == wire::Kind::text &&
                frame.payload == "next" && buffer.isEmpty());
        rejects([&] { static_cast<void>(wire::decode_snapshot(encoded.chopped(1))); });
        rejects([&] { static_cast<void>(wire::decode_snapshot(encoded + 'x')); });
        auto invalid = expected;
        invalid.cells.front().text_offset = 100000;
        rejects([&] { static_cast<void>(wire::decode_snapshot(wire::encode_snapshot(invalid))); });
        invalid = expected;
        invalid.graphemes.front() = char32_t{0xd800};
        rejects([&] { static_cast<void>(wire::decode_snapshot(wire::encode_snapshot(invalid))); });
        for (const auto* hex : {"00000000", "00800001", "0000000100"}) {
            auto malformed = QByteArray::fromHex(hex);
            rejects([&] { static_cast<void>(wire::take_frame(malformed, frame)); });
        }
        std::cout << "Snapshot, fragmented/coalesced frames and malformed input passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
