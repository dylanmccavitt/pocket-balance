// SPDX-License-Identifier: AGPL-3.0-only
// Copyright 2026 Pocket Balance contributors
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace pocket
{
// Version 1 wire format. Every word is phase:1 | opcode:3 | payload:20.
// BEGIN: lane:4 | content byte count:9 | revision nonce:7.
// DATA: chunk index modulo 16:4 | first byte:8 | second byte:8.
// END: CRC16-CCITT-FALSE:16 | nonce low bits:4.
// Content is source Mixer track:8 followed by 1..256 UTF-8 name bytes.
// CRC covers [1, lane, count high, count low, nonce, context high, middle, low]
// followed by all content bytes, without the final DATA padding byte.
class SourceSyncReceiver
{
public:
    enum Opcode : std::uint32_t { nop = 0, begin = 1, data = 2, end = 3, reset = 4, status = 5 };
    enum class Result { ignored, accepted, nameReady, routeStatus, rejected };
    struct Context
    {
        std::uint32_t word = 0;
        std::array<int, 16> mapping {};
        std::uint16_t validRoutes = 0;
    };

    static constexpr std::size_t maxNameBytes = 256;
    static constexpr std::size_t maxContentBytes = maxNameBytes + 1;
    static constexpr std::uint32_t phaseMask = 1u << 23;

    static std::uint16_t crcByte(std::uint16_t crc, std::uint8_t byte) noexcept
    {
        crc ^= static_cast<std::uint16_t>(static_cast<unsigned>(byte) << 8);
        for (int i = 0; i < 8; ++i)
            crc = static_cast<std::uint16_t>((static_cast<unsigned>(crc) << 1)
                  ^ ((crc & 0x8000u) != 0 ? 0x1021u : 0u));
        return crc;
    }

    static std::uint16_t frameCrc(int lane, std::size_t count, int nonce,
                                 std::uint32_t context, const std::uint8_t* bytes) noexcept
    {
        const std::uint8_t header[] { 1, static_cast<std::uint8_t>(lane),
            static_cast<std::uint8_t>(count >> 8), static_cast<std::uint8_t>(count),
            static_cast<std::uint8_t>(nonce), static_cast<std::uint8_t>(context >> 16),
            static_cast<std::uint8_t>(context >> 8), static_cast<std::uint8_t>(context) };
        std::uint16_t crc = 0xffff;
        for (auto byte : header) crc = crcByte(crc, byte);
        for (std::size_t i = 0; i < count; ++i) crc = crcByte(crc, bytes[i]);
        return crc;
    }

    static bool validUtf8(const std::uint8_t* bytes, std::size_t count) noexcept
    {
        if (count == 0 || count > maxNameBytes) return false;
        for (std::size_t i = 0; i < count;)
        {
            const auto first = bytes[i++];
            if (first == 0) return false;
            if (first < 0x80) continue;
            int extra = 0;
            std::uint32_t codepoint = 0, minimum = 0;
            if (first >= 0xc2 && first <= 0xdf) { extra = 1; codepoint = first & 0x1fu; minimum = 0x80; }
            else if (first >= 0xe0 && first <= 0xef) { extra = 2; codepoint = first & 0x0fu; minimum = 0x800; }
            else if (first >= 0xf0 && first <= 0xf4) { extra = 3; codepoint = first & 0x07u; minimum = 0x10000; }
            else return false;
            if (i + static_cast<std::size_t>(extra) > count) return false;
            for (int n = 0; n < extra; ++n)
            {
                const auto next = bytes[i++];
                if ((next & 0xc0u) != 0x80u) return false;
                codepoint = (codepoint << 6) | (next & 0x3fu);
            }
            if (codepoint < minimum || codepoint > 0x10ffffu
                || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
        }
        return true;
    }

    void clear() noexcept { active = false; received = 0; haveLastWord = false; }
    void cancelFrame() noexcept { active = false; received = 0; }

    Result accept(std::uint32_t word, const Context& context) noexcept
    {
        if (word >= (1u << 24)) return Result::rejected;
        if (haveLastWord && word == lastWord) return lastAccepted ? Result::ignored : Result::rejected;
        const auto opcode = (word >> 20) & 7u;
        if (haveLastWord && (word & phaseMask) == (lastWord & phaseMask) && opcode != reset)
        { cancelFrame(); return Result::rejected; }
        lastWord = word; haveLastWord = true;
        const auto result = acceptNew(word, context);
        lastAccepted = result != Result::rejected;
        return result;
    }

    int completedLane() const noexcept { return lane; }
    const char* completedName() const noexcept { return reinterpret_cast<const char*>(content.data() + 1); }
    int completedNameBytes() const noexcept { return static_cast<int>(length - 1); }

private:
    Result acceptNew(std::uint32_t word, const Context& context) noexcept
    {
        const auto opcode = (word >> 20) & 7u;
        const auto payload = word & 0xfffffu;
        if (opcode == reset) { cancelFrame(); return Result::accepted; }
        if (opcode == nop) return payload == 0 ? Result::accepted : Result::rejected;
        if (opcode == status)
        {
            cancelFrame();
            return (payload & 0xf0000u) == 0 ? Result::routeStatus : Result::rejected;
        }
        if ((context.word & (1u << 12)) == 0) { cancelFrame(); return Result::rejected; }
        if (opcode == begin)
        {
            cancelFrame();
            lane = static_cast<int>((payload >> 16) & 15u);
            length = static_cast<std::size_t>((payload >> 7) & 511u);
            nonce = static_cast<int>(payload & 127u);
            if (length < 2 || length > maxContentBytes
                || nonce != static_cast<int>((context.word >> 13) & 127u)
                || context.mapping[static_cast<std::size_t>(lane)] < 0
                || (context.validRoutes & (1u << lane)) == 0) return Result::rejected;
            active = true; frameContext = context.word;
            source = context.mapping[static_cast<std::size_t>(lane)];
            return Result::accepted;
        }
        if (!active || context.word != frameContext
            || context.mapping[static_cast<std::size_t>(lane)] != source
            || (context.validRoutes & (1u << lane)) == 0)
        { cancelFrame(); return Result::rejected; }
        if (opcode == data)
        {
            if (received >= length || ((payload >> 16) & 15u) != ((received / 2) & 15u))
            { cancelFrame(); return Result::rejected; }
            content[received++] = static_cast<std::uint8_t>(payload >> 8);
            if (received < length) content[received++] = static_cast<std::uint8_t>(payload);
            else if ((payload & 255u) != 0) { cancelFrame(); return Result::rejected; }
            return Result::accepted;
        }
        if (opcode == end)
        {
            active = false;
            if (received != length || (payload & 15u) != (static_cast<unsigned>(nonce) & 15u)
                || (payload >> 4) != frameCrc(lane, length, nonce, frameContext, content.data())
                || content[0] != source || !validUtf8(content.data() + 1, length - 1)) return Result::rejected;
            return Result::nameReady;
        }
        cancelFrame(); return Result::rejected;
    }

    std::array<std::uint8_t, maxContentBytes> content {};
    std::uint32_t frameContext = 0, lastWord = 0;
    std::size_t length = 0, received = 0;
    int lane = 0, source = -1, nonce = 0;
    bool active = false, haveLastWord = false, lastAccepted = false;
};
}
