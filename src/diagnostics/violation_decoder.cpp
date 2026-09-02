#include "diagnostics/violation_decoder.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace dobby {
namespace {

template <class T>
T readUnaligned(const std::byte* address) {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

struct AndroidStringView {
    std::string_view value;
    const char* storage;
};

std::optional<AndroidStringView> readAndroidString(const std::byte* object) {
    const auto tag = readUnaligned<std::uint8_t>(object);
    if ((tag & 1U) == 0U) {
        const auto length = static_cast<std::size_t>(tag >> 1U);
        return AndroidStringView{
                std::string_view(reinterpret_cast<const char*>(object + 1), length), "short"};
    }

    const auto length = readUnaligned<std::size_t>(object + 8);
    const auto data = readUnaligned<const char*>(object + 16);
    if (data == nullptr || length > kMaximumContextLength)
        return std::nullopt;
    return AndroidStringView{std::string_view(data, length), "long"};
}

} // namespace

std::optional<ViolationRecord> decodeViolation(const void* packet) {
    if (packet == nullptr)
        return std::nullopt;

    const auto* bytes = static_cast<const std::byte*>(packet);
    const auto context = readAndroidString(bytes + kViolationContextOffset);
    if (!context)
        return std::nullopt;

    ViolationRecord result;
    result.type = readUnaligned<std::int32_t>(bytes + kViolationTypeOffset);
    result.severity = readUnaligned<std::int32_t>(bytes + kViolationSeverityOffset);
    result.packetId = readUnaligned<std::int32_t>(bytes + kViolationPacketIdOffset);
    result.context.assign(context->value);
    result.contextStorage = context->storage;
    return result;
}

std::optional<ViolationRecord> decodeViolationArguments(
        std::int32_t response, std::int32_t packetId, const void* context) {
    if (context == nullptr)
        return std::nullopt;

    const auto decodedContext = readAndroidString(
            static_cast<const std::byte*>(context));
    if (!decodedContext)
        return std::nullopt;

    ViolationRecord result;
    // handlePacketViolation receives the response but not the serialized
    // PacketViolationType. Preserve that absence instead of inferring a type.
    result.type = -1;
    result.severity = response >= 1 && response <= 3 ? response - 1 : -1;
    result.packetId = packetId;
    result.context.assign(decodedContext->value);
    result.contextStorage = decodedContext->storage;
    return result;
}

std::optional<ViolationRecord> decodeBadPacketDisconnect(
        std::int32_t packetId, std::uint64_t packetSize,
        const void* messageFromServer, const void* messageBodyOverride) {
    if (messageFromServer == nullptr || messageBodyOverride == nullptr)
        return std::nullopt;

    const auto serverMessage = readAndroidString(
            static_cast<const std::byte*>(messageFromServer));
    const auto bodyOverride = readAndroidString(
            static_cast<const std::byte*>(messageBodyOverride));
    if (!serverMessage || !bodyOverride)
        return std::nullopt;

    ViolationRecord result;
    // The disconnect callback exposes neither a serialized violation type nor
    // a PacketViolationResponse. Reason 90 is itself a terminating BadPacket
    // disconnect, so preserve the absent type and the observed severity.
    result.type = -1;
    result.severity = 2;
    result.packetId = packetId;
    result.context = "DisconnectFailReason::BadPacket (90)";
    if (packetId >= 0) {
        result.context += "\nallowIncomingPacketId packet_size=" +
                          std::to_string(packetSize);
    } else {
        result.context += "\ninbound_packet_correlation=unavailable";
    }
    if (!serverMessage->value.empty()) {
        result.context += "\nmessage_from_server=";
        result.context.append(serverMessage->value);
    }
    if (!bodyOverride->value.empty()) {
        result.context += "\nmessage_body_override=";
        result.context.append(bodyOverride->value);
    }
    result.contextStorage = "disconnect_callback";
    return result;
}

std::string violationObjectLayout(const ViolationRecord& record) {
    return
            "PacketViolationWarningPacket payload @ object + 0x30\n"
            "  +0x30  int32 violation_type     = " + std::to_string(record.type) + "\n"
            "  +0x34  int32 violation_severity = " + std::to_string(record.severity) + "\n"
            "  +0x38  int32 violating_packet   = " + std::to_string(record.packetId) + "\n"
            "  +0x40  libc++ string context    = " + record.contextStorage +
            " / " + std::to_string(record.context.size()) + " bytes\n";
}

} // namespace dobby
