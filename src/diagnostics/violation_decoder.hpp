#pragma once

#include "diagnostics/types.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace dobby {

inline constexpr std::ptrdiff_t kViolationTypeOffset = 0x30;
inline constexpr std::ptrdiff_t kViolationSeverityOffset = 0x34;
inline constexpr std::ptrdiff_t kViolationPacketIdOffset = 0x38;
inline constexpr std::ptrdiff_t kViolationContextOffset = 0x40;
inline constexpr std::size_t kMaximumContextLength = 1024 * 1024;

std::optional<ViolationRecord> decodeViolation(const void* packet);
std::optional<ViolationRecord> decodeViolationArguments(
        std::int32_t response, std::int32_t packetId, const void* context);
std::optional<ViolationRecord> decodeBadPacketDisconnect(
        std::int32_t packetId, std::uint64_t packetSize,
        const void* messageFromServer, const void* messageBodyOverride);
std::string violationObjectLayout(const ViolationRecord& record);

} // namespace dobby
