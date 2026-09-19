#pragma once

#include "diagnostics/types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace dobby {

std::string_view disconnectReasonName(std::int32_t reason);
std::string_view disconnectCodeword(std::int32_t reason);

std::optional<DisconnectEvidence> decodeDisconnectArguments(
        std::int32_t reason, std::int32_t stage,
        const void* messageFromServer, const void* messageBodyOverride,
        bool skipMessage, bool sourcePresent, bool telemetryOverridePresent);

} // namespace dobby
