#pragma once

#include "core/runtime_state.hpp"
#include "diagnostics/types.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace dobby {

const char* severityName(std::int32_t severity);
const char* violationTypeName(std::int32_t type);
std::string packetIdHex(std::int32_t packetId);
Diagnostic buildDiagnostic(
        const ViolationRecord& record, std::optional<StreamFailure> streamFailure,
        std::string intercept,
        std::optional<ValidationEvidence> validation = std::nullopt,
        std::optional<DisconnectEvidence> disconnect = std::nullopt);
Diagnostic buildDisconnectDiagnostic(
        DisconnectEvidence evidence, std::string intercept);
std::string buildDeveloperStatus(const RuntimeSnapshot& snapshot);
std::string rawPacketHex(const Diagnostic& diagnostic);
std::string streamFailureSummary(const Diagnostic& diagnostic);

} // namespace dobby
