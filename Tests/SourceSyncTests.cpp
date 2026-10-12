// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#include "SourceSyncProtocol.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using Receiver = pocket::SourceSyncReceiver;
using Result = Receiver::Result;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Sender
{
    Receiver receiver;
    Receiver::Context context;
    std::uint32_t phase = 0, lastWord = 0;
    Sender()
    {
        context.word = 21u | (1u << 8) | (1u << 12) | (11u << 13);
        context.mapping.fill(-1); context.mapping[2] = 14; context.mapping[7] = 22;
        context.validRoutes = (1u << 2) | (1u << 7);
    }
    Result send(Receiver::Opcode opcode, std::uint32_t payload)
    {
        phase ^= Receiver::phaseMask;
        lastWord = phase | (opcode << 20) | payload;
        return receiver.accept(lastWord, context);
    }
    int nonce() const { return static_cast<int>((context.word >> 13) & 127u); }
    std::vector<std::uint8_t> bytes(int source, const std::string& name)
    {
        std::vector<std::uint8_t> content { static_cast<std::uint8_t>(source) };
        content.insert(content.end(), name.begin(), name.end()); return content;
    }
    Result begin(int lane, std::size_t size)
    { return send(Receiver::begin, (static_cast<unsigned>(lane) << 16) | (static_cast<unsigned>(size) << 7) | static_cast<unsigned>(nonce())); }
    void data(const std::vector<std::uint8_t>& content)
    {
        for (std::size_t i = 0; i < content.size(); i += 2)
        {
            const auto second = i + 1 < content.size() ? content[i + 1] : 0;
            require(send(Receiver::data, static_cast<std::uint32_t>(((i / 2) & 15u) << 16)
                | (static_cast<std::uint32_t>(content[i]) << 8) | static_cast<std::uint32_t>(second)) == Result::accepted,
                "In-order content packets are accepted");
        }
    }
    Result end(int lane, const std::vector<std::uint8_t>& content, unsigned damage = 0)
    {
        const auto crc = Receiver::frameCrc(lane, content.size(), nonce(), context.word, content.data());
        return send(Receiver::end, ((static_cast<std::uint32_t>(crc) ^ damage) << 4) | (static_cast<unsigned>(nonce()) & 15u));
    }
    Result frame(int lane, int source, const std::string& name)
    {
        const auto content = bytes(source, name);
        const auto result = begin(lane, content.size());
        if (result != Result::accepted) return result;
        data(content); return end(lane, content);
    }
    std::string name() const
    { return { receiver.completedName(), static_cast<std::size_t>(receiver.completedNameBytes()) }; }
};

void testWireFormat()
{
    std::uint16_t check = 0xffff;
    for (auto byte : std::string("123456789")) check = Receiver::crcByte(check, static_cast<std::uint8_t>(byte));
    require(check == 0x29b1, "CRC matches the CCITT-FALSE reference vector");
    Sender sender;
    require(sender.context.word == 0x017115, "Context matches the Python companion fixture");
    const auto fixture = sender.bytes(14, "Hat");
    require(Receiver::frameCrc(2, fixture.size(), 11, sender.context.word, fixture.data()) == 0xe271,
            "CRC matches the Python companion fixture");
    require(sender.send(Receiver::status, sender.context.validRoutes) == Result::routeStatus, "STATUS carries route validity");
    require(sender.frame(2, 14, "Hat") == Result::nameReady, "Short ASCII name is accepted");
    require(sender.receiver.completedLane() == 2 && sender.name() == "Hat", "Completed frame preserves lane and exact name");
    require(sender.receiver.accept(sender.lastWord, sender.context) == Result::ignored, "Retransmission is idempotent");
    require(sender.frame(7, 22, "  \xc3\xa9 \xe9\x9f\xb3 \xf0\x9f\xa5\x81  ") == Result::nameReady,
            "Unicode and edge whitespace are preserved");
    require(sender.name() == "  \xc3\xa9 \xe9\x9f\xb3 \xf0\x9f\xa5\x81  ", "Names remain byte-exact");
    require(sender.frame(2, 14, std::string(256, 'x')) == Result::nameReady && sender.name().size() == 256,
            "Longest content frame survives chunk-index wrap and odd padding");
    require(sender.frame(2, 14, std::string(257, 'x')) == Result::rejected, "Oversized names are rejected");
    require(sender.frame(2, 14, "") == Result::rejected, "Empty names are rejected");
}

void testIsolationAndCorruption()
{
    Sender sender;
    const auto content = sender.bytes(14, "Hat");
    require(sender.begin(2, content.size()) == Result::accepted, "Begin accepts mapped active lane");
    sender.data(content);
    require(sender.end(2, content, 1) == Result::rejected, "CRC corruption never completes a name");
    require(sender.receiver.accept(sender.lastWord, sender.context) == Result::rejected,
            "Retrying a rejected packet cannot receive a success acknowledgement");
    require(sender.frame(2, 22, "Hat") == Result::rejected, "Source prefix must match explicit lane mapping");
    require(sender.frame(0, 14, "Hat") == Result::rejected, "Unmapped gaps remain unmapped");
    sender.context.validRoutes &= ~(1u << 2);
    require(sender.frame(2, 14, "Hat") == Result::rejected, "Inactive route cannot change a name");
    sender.context.validRoutes |= 1u << 2;
    require(sender.begin(2, content.size()) == Result::accepted, "New frame starts after rejection");
    require(sender.send(Receiver::data, (1u << 16) | (14u << 8) | 'H') == Result::rejected,
            "Out-of-order chunks cancel a frame");
    require(sender.end(2, content) == Result::rejected, "Cancelled frame cannot finish");
    require(sender.begin(2, content.size()) == Result::accepted, "Begin starts context test");
    sender.context.word += 1u << 13;
    require(sender.send(Receiver::data, (14u << 8) | 'H') == Result::rejected, "Context change cancels in-flight data");
    require(sender.frame(2, 14, "Hat") == Result::nameReady, "Fresh frame works with new context");
    require(sender.begin(2, content.size()) == Result::accepted, "Begin starts map change test");
    sender.context.mapping[2] = 15;
    require(sender.send(Receiver::data, (14u << 8) | 'H') == Result::rejected, "Changed source mapping cancels in-flight data");
    sender.context.mapping[2] = 14;
    require(sender.begin(2, content.size()) == Result::accepted, "Begin starts RESET test");
    require(sender.send(Receiver::reset, 0) == Result::accepted, "RESET is accepted");
    require(sender.end(2, content) == Result::rejected, "RESET discards partial content");
    require(sender.begin(2, content.size()) == Result::accepted, "Begin starts STATUS test");
    require(sender.send(Receiver::status, sender.context.validRoutes) == Result::routeStatus, "STATUS cancels prior frame");
    require(sender.end(2, content) == Result::rejected, "Route status interrupts a stale frame");
    require(sender.send(Receiver::nop, 0) == Result::accepted, "Heartbeat carries no content");
    require(sender.send(Receiver::nop, 1) == Result::rejected, "Unknown heartbeat content is rejected");
    require(sender.send(Receiver::status, 1u << 16) == Result::rejected, "Unknown STATUS bits are rejected");
    Sender other;
    require(other.frame(2, 14, "Other") == Result::nameReady, "Second receiver independently accepts names");
    require(sender.frame(2, 14, "First") == Result::nameReady && other.name() == "Other",
            "Instances cannot leak assembled names");
}

void testInvalidUtf8()
{
    const std::string invalid[] { std::string("x\0y", 3), "\x80", "\xc0\x80", "\xc2", "\xe0\x80\x80",
        "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe2(" };
    for (const auto& name : invalid)
    {
        Sender sender;
        require(sender.frame(2, 14, name) == Result::rejected, "Malformed UTF-8 and embedded NUL are rejected");
    }
}
}
int main()
{
    testWireFormat(); testIsolationAndCorruption(); testInvalidUtf8();
    std::cout << "PASS: source-name wire format, CRC, bounded UTF-8, mapping, context, retry and instance isolation\n";
}
