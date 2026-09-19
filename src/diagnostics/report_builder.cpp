#include "diagnostics/report_builder.hpp"

#include "core/config.hpp"
#include "core/constants.hpp"
#include "diagnostics/violation_decoder.hpp"
#include "network/packet_names.hpp"
#include "platform/files.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>

namespace dobby {
namespace {

std::string packetNameString(std::int32_t packetId) {
    return std::string(packetName(packetId));
}

std::string buildReadTrace(const StreamFailure& failure) {
    std::ostringstream output;
    output << "Reads: ";
    if (failure.attempts.empty()) {
        output << "none\n";
        return output.str();
    }

    std::size_t index = 0;
    while (index < failure.attempts.size()) {
        const auto& first = failure.attempts[index];
        std::size_t count = 1;
        std::size_t end = first.offset + first.requested;
        while (index + count < failure.attempts.size()) {
            const auto& next = failure.attempts[index + count];
            if (first.overflow || next.overflow || next.requested != first.requested ||
                next.offset != end) {
                break;
            }
            end += next.requested;
            ++count;
        }
        if (index != 0)
            output << ", ";
        if (first.overflow) {
            output << "FAIL@" << first.offset << "+" << first.requested;
        } else {
            output << first.offset << "->" << end << " (" << count << "x"
                   << first.requested << "B";
            if (first.requested == 0)
                output << "; zero-length read, not confirmed failure";
            output << ')';
        }
        index += count;
    }
    output << '\n';
    return output.str();
}

std::string lastClientField(const StreamFailure& failure) {
    return failure.attempts.empty() ? std::string{} : failure.attempts.back().clientField;
}

std::string buildClientFieldLine(const StreamFailure& failure) {
    const auto field = lastClientField(failure);
    return field.empty() ? std::string{} : "Client field: " + field + "\n";
}

std::string buildHexDump(const StreamFailure& failure) {
    std::ostringstream output;
    constexpr std::size_t width = 16;
    for (std::size_t start = 0; start < failure.rawBytes.size(); start += width) {
        const bool failureLine = failure.failureOffset >= start && failure.failureOffset < start + width;
        std::array<char, 24> offset{};
        std::snprintf(offset.data(), offset.size(), "%06zx", start);
        output << (failureLine ? "> " : "  ") << offset.data() << "  ";
        const auto count = std::min(width, failure.rawBytes.size() - start);
        output << hexBytes(std::span<const std::uint8_t>(failure.rawBytes.data() + start, count)) << '\n';
    }
    if (failure.rawBytesTruncated)
        output << "  ... capture truncated; full stream size=" << failure.viewSize << " bytes\n";
    return output.str();
}

template <class T>
T readLittleEndian(std::span<const std::uint8_t> bytes) {
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

std::string floatingPointText(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

std::span<const std::uint8_t> attemptBytes(
        const StreamFailure& failure, const StreamReadAttempt& attempt) {
    if (attempt.offset >= failure.rawBytes.size())
        return {};
    const auto size = std::min(attempt.requested, failure.rawBytes.size() - attempt.offset);
    return {failure.rawBytes.data() + attempt.offset, size};
}

std::string primitiveInterpretationsJson(std::span<const std::uint8_t> bytes) {
    if (bytes.size() == 1) {
        const auto value = bytes[0];
        return std::string("{\"u8\":") + std::to_string(value) +
               ",\"i8\":" + std::to_string(static_cast<std::int8_t>(value)) +
               ",\"bool_candidate\":" + (value == 0 ? "false" : "true") + "}";
    }
    if (bytes.size() == 2) {
        return std::string("{\"u16_le\":") +
               std::to_string(readLittleEndian<std::uint16_t>(bytes)) +
               ",\"i16_le\":" +
               std::to_string(readLittleEndian<std::int16_t>(bytes)) + "}";
    }
    if (bytes.size() == 4) {
        return std::string("{\"u32_le\":") +
               std::to_string(readLittleEndian<std::uint32_t>(bytes)) +
               ",\"i32_le\":" +
               std::to_string(readLittleEndian<std::int32_t>(bytes)) +
               ",\"f32_le\":\"" +
               floatingPointText(readLittleEndian<float>(bytes)) + "\"}";
    }
    if (bytes.size() == 8) {
        return std::string("{\"u64_le\":\"") +
               std::to_string(readLittleEndian<std::uint64_t>(bytes)) +
               "\",\"i64_le\":\"" +
               std::to_string(readLittleEndian<std::int64_t>(bytes)) +
               "\",\"f64_le\":\"" +
               floatingPointText(readLittleEndian<double>(bytes)) + "\"}";
    }
    return "null";
}

std::string primitiveInterpretationsText(std::span<const std::uint8_t> bytes) {
    if (bytes.size() == 1) {
        const auto value = bytes[0];
        return "u8=" + std::to_string(value) +
               " i8=" + std::to_string(static_cast<std::int8_t>(value)) +
               " bool=" + (value == 0 ? "false" : "true");
    }
    if (bytes.size() == 2) {
        return "u16le=" + std::to_string(readLittleEndian<std::uint16_t>(bytes)) +
               " i16le=" + std::to_string(readLittleEndian<std::int16_t>(bytes));
    }
    if (bytes.size() == 4) {
        return "u32le=" + std::to_string(readLittleEndian<std::uint32_t>(bytes)) +
               " i32le=" + std::to_string(readLittleEndian<std::int32_t>(bytes)) +
               " f32le=" + floatingPointText(readLittleEndian<float>(bytes));
    }
    if (bytes.size() == 8) {
        return "u64le=" + std::to_string(readLittleEndian<std::uint64_t>(bytes)) +
               " i64le=" + std::to_string(readLittleEndian<std::int64_t>(bytes)) +
               " f64le=" + floatingPointText(readLittleEndian<double>(bytes));
    }
    return {};
}

std::string buildFieldEvidence(const StreamFailure& failure) {
    std::ostringstream output;
    bool wroteHeader = false;
    for (const auto& attempt : failure.attempts) {
        if (attempt.clientField.empty())
            continue;
        if (!wroteHeader) {
            output << "Field evidence (raw bytes; little-endian candidates, types not inferred):\n";
            wroteHeader = true;
        }
        const auto bytes = attemptBytes(failure, attempt);
        output << "- " << attempt.clientField << " @" << attempt.offset << "+"
               << attempt.requested << " raw " << hexBytes(bytes);
        if (bytes.size() != attempt.requested)
            output << " [captured " << bytes.size() << "/" << attempt.requested << "]";
        const auto candidates = primitiveInterpretationsText(bytes);
        if (!candidates.empty())
            output << " | " << candidates;
        if (attempt.overflow)
            output << " | overflow";
        output << '\n';
    }
    return output.str();
}

std::string imageOffsetHex(std::uint64_t offset) {
    std::ostringstream output;
    output << "0x" << std::hex << offset;
    return output.str();
}

std::string buildValidationReport(const ValidationEvidence& evidence) {
    std::ostringstream output;
    output << "Validation result: " << (evidence.resultSuccess ? "success" : "failure")
           << " | response " << evidence.response
           << " | new_or_updated " << (evidence.newOrUpdated ? "yes" : "no") << '\n';
    output << "Validation error: "
           << (evidence.errorCategory.empty() ? "unknown" : evidence.errorCategory)
           << ':' << evidence.errorValue;
    if (!evidence.errorMessage.empty())
        output << " | " << evidence.errorMessage;
    output << '\n';
    if (!evidence.sourceFrames.empty()) {
        output << "Bedrock error provenance (inner -> outer):\n";
        for (const auto& frame : evidence.sourceFrames) {
            output << "- " << (frame.filename.empty() ? "<filename unavailable>" : frame.filename)
                   << ':' << frame.line << " | hash " << imageOffsetHex(frame.filenameHash);
            if (!frame.context.empty())
                output << " | " << frame.context;
            output << '\n';
        }
    }
    if (!evidence.nestedErrors.empty()) {
        output << "Nested validation errors:\n";
        for (const auto& nested : evidence.nestedErrors) {
            output << "- depth " << nested.depth << " | "
                   << (nested.errorCategory.empty() ? "unknown" : nested.errorCategory)
                   << ':' << nested.errorValue;
            if (!nested.errorMessage.empty())
                output << " | " << nested.errorMessage;
            output << '\n';
            for (const auto& frame : nested.sourceFrames) {
                output << "  - "
                       << (frame.filename.empty() ? "<filename unavailable>" : frame.filename)
                       << ':' << frame.line;
                if (!frame.context.empty())
                    output << " | " << frame.context;
                output << '\n';
            }
        }
    }
    if (evidence.provenanceTruncated)
        output << "Bedrock error provenance: truncated or unreadable\n";
    if (!evidence.nativeStackImageOffsets.empty()) {
        output << "Native stack (libminecraftpe image offsets):\n";
        for (const auto offset : evidence.nativeStackImageOffsets)
            output << "- libminecraftpe+" << imageOffsetHex(offset) << '\n';
    }
    if (!evidence.recentPackets.empty()) {
        output << "Recent inbound packets (oldest -> newest):\n";
        for (const auto& packet : evidence.recentPackets) {
            output << "- " << packetNameString(packet.packetId) << " (" << packet.packetId
                   << ") size " << packet.packetSize << " age "
                   << packet.ageMilliseconds << "ms\n";
        }
    }
    output << '\n';
    return output.str();
}

std::string buildDisconnectJson(const Diagnostic& diagnostic) {
    const auto& evidence = *diagnostic.disconnect;
    std::string json =
            std::string("{\"tool\":\"dobby\",\"tool_version\":\"") + kDobbyVersion +
            "\",\"event\":\"client_disconnect\",\"captured_at\":\"" +
            jsonEscape(diagnostic.capturedAt) +
            "\",\"direction\":\"server_to_client\",\"intercept\":\"" +
            jsonEscape(diagnostic.intercept) +
            "\",\"disconnect_reason\":" + std::to_string(evidence.reason) +
            ",\"disconnect_reason_name\":\"" + jsonEscape(evidence.reasonName) +
            "\",\"codeword\":\"" + jsonEscape(evidence.codeword) +
            "\",\"disconnect_stage\":" + std::to_string(evidence.stage) +
            ",\"skip_message\":" + (evidence.skipMessage ? "true" : "false") +
            ",\"source_present\":" + (evidence.sourcePresent ? "true" : "false") +
            ",\"telemetry_override_present\":" +
            (evidence.telemetryOverridePresent ? "true" : "false") +
            ",\"message_from_server\":\"" + jsonEscape(evidence.messageFromServer) +
            "\",\"message_body_override\":\"" +
            jsonEscape(evidence.messageBodyOverride) +
            "\",\"message_from_server_storage\":\"" +
            jsonEscape(evidence.messageFromServerStorage) +
            "\",\"message_body_override_storage\":\"" +
            jsonEscape(evidence.messageBodyOverrideStorage) +
            "\",\"latest_packet_id\":" + std::to_string(diagnostic.packetId) +
            ",\"latest_packet_name\":\"" +
            jsonEscape(packetNameString(diagnostic.packetId)) +
            "\",\"native_stack\":[";
    for (std::size_t index = 0; index < evidence.nativeStackImageOffsets.size(); ++index) {
        if (index != 0)
            json += ',';
        json += "{\"image_offset\":\"" +
                imageOffsetHex(evidence.nativeStackImageOffsets[index]) + "\"}";
    }
    json += "],\"recent_packets\":[";
    for (std::size_t index = 0; index < evidence.recentPackets.size(); ++index) {
        if (index != 0)
            json += ',';
        const auto& packet = evidence.recentPackets[index];
        json += std::string("{\"packet_id\":") + std::to_string(packet.packetId) +
                ",\"packet_name\":\"" + jsonEscape(packetNameString(packet.packetId)) +
                "\",\"packet_size\":" + std::to_string(packet.packetSize) +
                ",\"age_ms\":" + std::to_string(packet.ageMilliseconds) + "}";
    }
    json += std::string("],\"minecraft_version\":\"") + kMinecraftVersion +
            "\",\"libminecraftpe_build_id\":\"" + kMinecraftBuildId + "\"}";
    return json;
}

std::string buildDisconnectReport(const Diagnostic& diagnostic) {
    const auto& evidence = *diagnostic.disconnect;
    std::ostringstream output;
    output << "DOBBY DISCONNECT DIAGNOSTIC\n"
           << diagnostic.capturedAt << " | client disconnect callback\n\n"
           << evidence.reasonName << " (" << evidence.reason << ") / "
           << evidence.codeword << '\n'
           << "Disconnect stage: " << evidence.stage << '\n'
           << "Skip message: " << (evidence.skipMessage ? "yes" : "no") << '\n'
           << "Source present: " << (evidence.sourcePresent ? "yes" : "no") << '\n'
           << "Telemetry override present: "
           << (evidence.telemetryOverridePresent ? "yes" : "no") << '\n';
    if (!evidence.messageFromServer.empty())
        output << "Message from server: " << evidence.messageFromServer << '\n';
    if (!evidence.messageBodyOverride.empty())
        output << "Message body override: " << evidence.messageBodyOverride << '\n';
    if (!evidence.recentPackets.empty()) {
        output << "\nRecent inbound packets (oldest -> newest):\n";
        for (const auto& packet : evidence.recentPackets) {
            output << "- " << packetNameString(packet.packetId) << " ("
                   << packet.packetId << " / " << packetIdHex(packet.packetId)
                   << ") size " << packet.packetSize << " age "
                   << packet.ageMilliseconds << "ms\n";
        }
    } else {
        output << "\nRecent inbound packets: unavailable\n";
    }
    if (!evidence.nativeStackImageOffsets.empty()) {
        output << "\nNative stack (libminecraftpe image offsets):\n";
        for (const auto offset : evidence.nativeStackImageOffsets)
            output << "- libminecraftpe+" << imageOffsetHex(offset) << '\n';
    }
    output << "\nDobby " << kDobbyVersion << " | Minecraft "
           << kMinecraftVersion << " | " << kAbi << '\n';
    return output.str();
}

std::string buildJson(const Diagnostic& diagnostic) {
    if (diagnostic.kind == DiagnosticKind::disconnect && diagnostic.disconnect)
        return buildDisconnectJson(diagnostic);
    std::string json =
            std::string("{\"tool\":\"dobby\",\"tool_version\":\"") + kDobbyVersion +
            "\",\"event\":\"packet_violation\",\"captured_at\":\"" +
            jsonEscape(diagnostic.capturedAt) +
            "\",\"direction\":\"server_to_client\",\"intercept\":\"" +
            jsonEscape(diagnostic.intercept) +
            "\",\"type\":" + std::to_string(diagnostic.type) +
            ",\"type_name\":\"" + violationTypeName(diagnostic.type) +
            "\",\"severity\":" + std::to_string(diagnostic.severity) +
            ",\"severity_name\":\"" + severityName(diagnostic.severity) +
            "\",\"packet_id\":" + std::to_string(diagnostic.packetId) +
            ",\"packet_id_hex\":\"" + packetIdHex(diagnostic.packetId) +
            "\",\"packet_name\":\"" + jsonEscape(packetNameString(diagnostic.packetId)) +
            "\",\"context\":\"" + jsonEscape(diagnostic.context) +
            "\",\"context_storage\":\"" + diagnostic.contextStorage + "\"";

    if (diagnostic.validation) {
        const auto& evidence = *diagnostic.validation;
        json += std::string(",\"validation\":{\"result_success\":") +
                (evidence.resultSuccess ? "true" : "false") +
                ",\"response\":" + std::to_string(evidence.response) +
                ",\"new_or_updated\":" + (evidence.newOrUpdated ? "true" : "false") +
                ",\"error_value\":" + std::to_string(evidence.errorValue) +
                ",\"error_category\":\"" + jsonEscape(evidence.errorCategory) +
                "\",\"error_message\":\"" + jsonEscape(evidence.errorMessage) +
                "\",\"provenance_truncated\":" +
                (evidence.provenanceTruncated ? "true" : "false") +
                ",\"source_frames\":[";
        for (std::size_t index = 0; index < evidence.sourceFrames.size(); ++index) {
            if (index != 0)
                json += ',';
            const auto& frame = evidence.sourceFrames[index];
            json += std::string("{\"filename_hash\":\"") +
                    imageOffsetHex(frame.filenameHash) +
                    "\",\"filename\":\"" + jsonEscape(frame.filename) +
                    "\",\"line\":" + std::to_string(frame.line) +
                    ",\"context\":\"" + jsonEscape(frame.context) + "\"}";
        }
        json += "],\"nested_errors\":[";
        for (std::size_t index = 0; index < evidence.nestedErrors.size(); ++index) {
            if (index != 0)
                json += ',';
            const auto& nested = evidence.nestedErrors[index];
            json += std::string("{\"depth\":") + std::to_string(nested.depth) +
                    ",\"error_value\":" + std::to_string(nested.errorValue) +
                    ",\"error_category\":\"" + jsonEscape(nested.errorCategory) +
                    "\",\"error_message\":\"" + jsonEscape(nested.errorMessage) +
                    "\",\"source_frames\":[";
            for (std::size_t frameIndex = 0;
                 frameIndex < nested.sourceFrames.size(); ++frameIndex) {
                if (frameIndex != 0)
                    json += ',';
                const auto& frame = nested.sourceFrames[frameIndex];
                json += std::string("{\"filename_hash\":\"") +
                        imageOffsetHex(frame.filenameHash) +
                        "\",\"filename\":\"" + jsonEscape(frame.filename) +
                        "\",\"line\":" + std::to_string(frame.line) +
                        ",\"context\":\"" + jsonEscape(frame.context) + "\"}";
            }
            json += "]}";
        }
        json += "],\"native_stack\":[";
        for (std::size_t index = 0; index < evidence.nativeStackImageOffsets.size(); ++index) {
            if (index != 0)
                json += ',';
            json += "{\"image_offset\":\"" +
                    imageOffsetHex(evidence.nativeStackImageOffsets[index]) + "\"}";
        }
        json += "],\"recent_packets\":[";
        for (std::size_t index = 0; index < evidence.recentPackets.size(); ++index) {
            if (index != 0)
                json += ',';
            const auto& packet = evidence.recentPackets[index];
            json += std::string("{\"packet_id\":") + std::to_string(packet.packetId) +
                    ",\"packet_name\":\"" + jsonEscape(packetNameString(packet.packetId)) +
                    "\",\"packet_size\":" + std::to_string(packet.packetSize) +
                    ",\"age_ms\":" + std::to_string(packet.ageMilliseconds) + "}";
        }
        json += "]}";
    } else {
        json += ",\"validation\":null";
    }

    if (diagnostic.streamFailure) {
        const auto& failure = *diagnostic.streamFailure;
        json +=
                ",\"decode_failure\":{\"offset\":" + std::to_string(failure.failureOffset) +
                ",\"stream_size\":" + std::to_string(failure.viewSize) +
                ",\"kind\":\"" +
                (failure.packetEndMismatch ? "unconsumed_trailing_bytes" :
                 failure.overflowObserved ? "primitive_overflow" : "correlated_trace") +
                "\",\"boundary_source\":\"" +
                (failure.packetEndMismatch
                         ? "ReadOnlyBinaryStream::ensureReadCompleted direct hook"
                         : failure.overflowObserved
                         ? "ReadOnlyBinaryStream::read direct hook"
                         : "correlated trace") +
                "\"" +
                ",\"overflow_observed\":" + (failure.overflowObserved ? "true" : "false") +
                ",\"requested\":" + std::to_string(failure.requested) +
                ",\"available\":" + std::to_string(failure.available) +
                ",\"overflow_before_read\":" + (failure.overflowBeforeRead ? "true" : "false") +
                ",\"raw_truncated\":" + (failure.rawBytesTruncated ? "true" : "false") +
                ",\"raw_hex\":\"" + jsonEscape(hexBytes(failure.rawBytes)) +
                "\",\"read_trace\":[";
        for (std::size_t index = 0; index < failure.attempts.size(); ++index) {
            const auto& attempt = failure.attempts[index];
            if (index != 0)
                json += ',';
            json +=
                    std::string("{\"offset\":") + std::to_string(attempt.offset) +
                    ",\"requested\":" + std::to_string(attempt.requested) +
                    ",\"available\":" + std::to_string(attempt.available) +
                    ",\"overflow\":" + (attempt.overflow ? "true" : "false");
            if (!attempt.clientField.empty())
                json += ",\"client_field\":\"" + jsonEscape(attempt.clientField) + "\"";
            const auto bytes = attemptBytes(failure, attempt);
            json += ",\"raw_hex\":\"" + jsonEscape(hexBytes(bytes)) +
                    "\",\"candidate_interpretations\":" +
                    primitiveInterpretationsJson(bytes);
            json += "}";
        }
        json += "]}";
    } else {
        json += ",\"decode_failure\":null";
    }

    if (diagnostic.disconnect) {
        const auto& evidence = *diagnostic.disconnect;
        json += std::string(",\"disconnect\":{\"reason\":") +
                std::to_string(evidence.reason) +
                ",\"reason_name\":\"" + jsonEscape(evidence.reasonName) +
                "\",\"codeword\":\"" + jsonEscape(evidence.codeword) +
                "\",\"stage\":" + std::to_string(evidence.stage) +
                ",\"skip_message\":" +
                (evidence.skipMessage ? "true" : "false") +
                ",\"source_present\":" +
                (evidence.sourcePresent ? "true" : "false") +
                ",\"telemetry_override_present\":" +
                (evidence.telemetryOverridePresent ? "true" : "false") +
                ",\"message_from_server\":\"" +
                jsonEscape(evidence.messageFromServer) +
                "\",\"message_body_override\":\"" +
                jsonEscape(evidence.messageBodyOverride) +
                "\",\"native_stack\":[";
        for (std::size_t index = 0;
             index < evidence.nativeStackImageOffsets.size(); ++index) {
            if (index != 0)
                json += ',';
            json += "{\"image_offset\":\"" +
                    imageOffsetHex(evidence.nativeStackImageOffsets[index]) +
                    "\"}";
        }
        json += "],\"recent_packets\":[";
        for (std::size_t index = 0; index < evidence.recentPackets.size(); ++index) {
            if (index != 0)
                json += ',';
            const auto& packet = evidence.recentPackets[index];
            json += std::string("{\"packet_id\":") +
                    std::to_string(packet.packetId) +
                    ",\"packet_name\":\"" +
                    jsonEscape(packetNameString(packet.packetId)) +
                    "\",\"packet_size\":" +
                    std::to_string(packet.packetSize) +
                    ",\"age_ms\":" +
                    std::to_string(packet.ageMilliseconds) + "}";
        }
        json += "]}";
    } else {
        json += ",\"disconnect\":null";
    }

    json +=
            std::string(",\"minecraft_version\":\"") + kMinecraftVersion +
            "\",\"libminecraftpe_build_id\":\"" + kMinecraftBuildId + "\"}";
    return json;
}

std::string buildReport(const Diagnostic& diagnostic) {
    if (diagnostic.kind == DiagnosticKind::disconnect && diagnostic.disconnect)
        return buildDisconnectReport(diagnostic);
    std::string report =
            "DOBBY PACKET DIAGNOSTIC\n"
            + diagnostic.capturedAt + " | server -> client\n\n"
            + packetNameString(diagnostic.packetId) + " (" +
            std::to_string(diagnostic.packetId) + " / " +
            packetIdHex(diagnostic.packetId) + ")\n" +
            violationTypeName(diagnostic.type) + " / " + severityName(diagnostic.severity) +
            "\n" + diagnostic.context + "\n\n";

    if (diagnostic.disconnect) {
        const auto& evidence = *diagnostic.disconnect;
        report += "Disconnect callback: " + evidence.reasonName + " (" +
                std::to_string(evidence.reason) + ") / " + evidence.codeword +
                "\nDisconnect stage: " + std::to_string(evidence.stage) +
                " | skip message " + (evidence.skipMessage ? "yes" : "no") +
                " | source " + (evidence.sourcePresent ? "present" : "absent") +
                " | telemetry override " +
                (evidence.telemetryOverridePresent ? "present" : "absent") + "\n";
        if (!evidence.messageFromServer.empty())
            report += "Message from server: " + evidence.messageFromServer + "\n";
        if (!evidence.messageBodyOverride.empty())
            report += "Message body override: " +
                    evidence.messageBodyOverride + "\n";
        report += "\n";
    }

    if (diagnostic.validation)
        report += buildValidationReport(*diagnostic.validation);

    if (diagnostic.streamFailure) {
        const auto& failure = *diagnostic.streamFailure;
        if (failure.packetEndMismatch) {
            report +=
                    "Decode: ReadOnlyBinaryStream::ensureReadCompleted\n"
                    "_read: success | cursor " + std::to_string(failure.failureOffset) +
                    "/" + std::to_string(failure.viewSize) + " | remaining " +
                    std::to_string(failure.available) + " | overflow no\n" +
                    buildClientFieldLine(failure) +
                    buildReadTrace(failure) +
                    buildFieldEvidence(failure) +
                    "Raw ('>' marks cursor):\n" +
                    buildHexDump(failure) + "\n";
        } else if (failure.overflowObserved) {
            report +=
                    "Decode: ReadOnlyBinaryStream::read\n"
                    "Cursor " + std::to_string(failure.failureOffset) + "/" +
                    std::to_string(failure.viewSize) + " | requested " +
                    std::to_string(failure.requested) + " | remaining " +
                    std::to_string(failure.available) + " | overflow yes\n" +
                    buildClientFieldLine(failure) +
                    buildReadTrace(failure) +
                    buildFieldEvidence(failure) +
                    "Raw ('>' marks cursor):\n" + buildHexDump(failure) + "\n";
        } else {
            report +=
                    "Decode boundary: unconfirmed\n"
                    "Cursor " + std::to_string(failure.failureOffset) + "/" +
                    std::to_string(failure.viewSize) + " | remaining " +
                    std::to_string(failure.available) + " | overflow no\n" +
                    buildClientFieldLine(failure) +
                    buildReadTrace(failure) +
                    buildFieldEvidence(failure) +
                    "Raw ('>' marks cursor):\n" + buildHexDump(failure) + "\n";
        }
    } else {
        report +=
                "Decode boundary: unavailable\n\n";
    }

    report +=
            std::string("Dobby ") + kDobbyVersion + " | Minecraft " + kMinecraftVersion +
            " | " + kAbi + "\n";
    return report;
}

} // namespace

const char* severityName(std::int32_t severity) {
    switch (severity) {
    case -1: return "unknown";
    case 0: return "warning";
    case 1: return "final_warning";
    case 2: return "terminating_connection";
    default: return "unrecognized";
    }
}

const char* violationTypeName(std::int32_t type) {
    switch (type) {
    case -1: return "Unknown";
    case 0: return "PacketMalformed";
    default: return "Unrecognized";
    }
}

std::string packetIdHex(std::int32_t packetId) {
    std::array<char, 16> value{};
    std::snprintf(value.data(), value.size(), "0x%x", static_cast<unsigned int>(packetId));
    return value.data();
}

Diagnostic buildDiagnostic(
        const ViolationRecord& record, std::optional<StreamFailure> streamFailure,
        std::string intercept, std::optional<ValidationEvidence> validation,
        std::optional<DisconnectEvidence> disconnect) {
    Diagnostic result;
    result.capturedAt = timestamp();
    result.type = record.type;
    result.severity = record.severity;
    result.packetId = record.packetId;
    result.context = record.context;
    result.contextStorage = record.contextStorage;
    result.intercept = std::move(intercept);
    result.streamFailure = std::move(streamFailure);
    result.validation = std::move(validation);
    result.disconnect = std::move(disconnect);
    result.json = buildJson(result);
    result.report = buildReport(result);
    return result;
}

Diagnostic buildDisconnectDiagnostic(
        DisconnectEvidence evidence, std::string intercept) {
    Diagnostic result;
    result.kind = DiagnosticKind::disconnect;
    result.capturedAt = timestamp();
    result.type = -1;
    result.severity = 2;
    result.packetId = evidence.recentPackets.empty()
            ? -1 : evidence.recentPackets.back().packetId;
    result.context = "DisconnectFailReason::" + evidence.reasonName + " (" +
            std::to_string(evidence.reason) + ")\ncodeword=" + evidence.codeword;
    result.contextStorage = "disconnect_callback";
    result.intercept = std::move(intercept);
    result.disconnect = std::move(evidence);
    result.json = buildJson(result);
    result.report = buildReport(result);
    return result;
}

std::string buildDeveloperStatus(const RuntimeSnapshot& snapshot) {
    return
            "DOBBY DEVELOPER CLIENT\n"
            "Version: " + std::string(kDobbyVersion) + "\n"
            "Hook: " + snapshot.hookStatus + "\n"
            "Warning hook: " + (snapshot.hookInstalled ? "active" : "inactive") + "\n"
            "Stream probe: " + (snapshot.streamProbeInstalled ? "active" : "inactive") + "\n"
            "Target: Minecraft " + kMinecraftVersion + " / " + kAbi + "\n"
            "Session started: " + snapshot.sessionStartedAt + "\n"
            "Violations: " + std::to_string(snapshot.totalViolations) +
            " total / " + std::to_string(snapshot.retainedViolations) + " retained\n"
            "Auto popup: " + (snapshot.autoPopup ? "on" : "off") +
            "\nVerbose events: " + (snapshot.verbose ? "on" : "off") +
            "\nHistory limit: " + std::to_string(config().historyLimit) +
            "\nRaw capture limit: " + std::to_string(config().rawCaptureLimit) + " bytes\n"
            "Log: " + logPath() + "\n"
            "JSONL: " + eventPath() + "\n"
            "AI snapshot: " + latestAiPath() + "\n";
}

std::string rawPacketHex(const Diagnostic& diagnostic) {
    if (!diagnostic.streamFailure)
        return {};
    return hexBytes(diagnostic.streamFailure->rawBytes);
}

std::string streamFailureSummary(const Diagnostic& diagnostic) {
    if (!diagnostic.streamFailure)
        return "Decode boundary unavailable";
    const auto& failure = *diagnostic.streamFailure;
    if (failure.packetEndMismatch) {
        return
                "_read success at " + std::to_string(failure.failureOffset) + "/" +
                std::to_string(failure.viewSize) + "  -  " +
                std::to_string(failure.available) + " bytes remained";
    }
    if (failure.overflowObserved) {
        return
                "Decode failed at byte " + std::to_string(failure.failureOffset) +
                " / " + std::to_string(failure.viewSize) +
                "  -  needed " + std::to_string(failure.requested) +
                ", had " + std::to_string(failure.available);
    }
    return
            "Decode trace retained at byte " + std::to_string(failure.failureOffset) +
            " / " + std::to_string(failure.viewSize) + "  -  no overflow observed";
}

} // namespace dobby
