#include "hooks/packet_hooks.hpp"

#include "core/config.hpp"
#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "diagnostics/client_schema_trace.hpp"
#include "diagnostics/disconnect_decoder.hpp"
#include "diagnostics/report_builder.hpp"
#include "diagnostics/stream_probe.hpp"
#include "diagnostics/validation_decoder.hpp"
#include "diagnostics/violation_decoder.hpp"
#include "hooks/minecraft_image.hpp"
#include "platform/files.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"
#include "platform/safe_memory.hpp"
#include "ui/developer_ui.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__ANDROID__)

#include <unwind.h>

extern "C" void* dobby_stream_read_continue = nullptr;
extern "C" void* dobby_packet_end_continue = nullptr;
extern "C" void* dobby_packet_security_continue = nullptr;
extern "C" void* dobby_packet_read_continue = nullptr;
extern "C" void* dobby_handle_violation_continue = nullptr;

extern "C" void dobby_capture_read_attempt(const void* stream, std::uint64_t requested) {
    dobby::captureStreamReadAttempt(stream, static_cast<std::size_t>(requested),
                                    dobby::config().rawCaptureLimit);
}

extern "C" void dobby_capture_packet_end(const void* stream) {
    dobby::capturePacketEndCheck(stream, dobby::config().rawCaptureLimit);
}

extern "C" [[gnu::naked]] void dobby_stream_read_detour() {
    asm volatile(
            // BL replaces x30 with its own return address. Preserve the
            // caller's x30 so the original Bedrock function returns to its
            // actual caller after this detour tail-branches into it.
            "sub sp, sp, #96\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "str x8, [sp, #64]\n"
            "str x30, [sp, #80]\n"
            "mov x1, x2\n"
            "bl dobby_capture_read_attempt\n"
            "ldp x0, x1, [sp, #0]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldr x8, [sp, #64]\n"
            "ldr x30, [sp, #80]\n"
            "add sp, sp, #96\n"
            // Replay the four instructions replaced by the inline entry
            // patch, then continue in the original function. Patching the
            // entry (instead of only its vtable slot) observes direct and
            // virtual primitive reads through the same validated path.
            "sub sp, sp, #288\n"
            "stp x29, x30, [sp, #224]\n"
            "str x28, [sp, #240]\n"
            "stp x22, x21, [sp, #256]\n"
            "adrp x16, dobby_stream_read_continue\n"
            "ldr x16, [x16, :lo12:dobby_stream_read_continue]\n"
            "br x16\n");
}

extern "C" [[gnu::naked]] void dobby_packet_end_detour() {
    asm volatile(
            "sub sp, sp, #96\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "str x8, [sp, #64]\n"
            "str x30, [sp, #80]\n"
            "bl dobby_capture_packet_end\n"
            "ldp x0, x1, [sp, #0]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldr x8, [sp, #64]\n"
            "ldr x30, [sp, #80]\n"
            "add sp, sp, #96\n"
            // Replay the verified ReadOnlyBinaryStream packet-completion
            // prologue before returning to Bedrock after the inline patch.
            "sub sp, sp, #144\n"
            "stp x29, x30, [sp, #96]\n"
            "str x21, [sp, #112]\n"
            "stp x20, x19, [sp, #128]\n"
            "adrp x16, dobby_packet_end_continue\n"
            "ldr x16, [x16, :lo12:dobby_packet_end_continue]\n"
            "br x16\n");
}

extern "C" [[gnu::naked]] void dobby_packet_security_trampoline() {
    asm volatile(
            // Replay the exact four instructions replaced at the concrete
            // PacketSecurityController entry, then execute the original body.
            // The BL from the C++ detour supplies its own return address in
            // x30, which the original prologue saves and later returns to.
            "stp x29, x30, [sp, #-64]!\n"
            "stp x24, x23, [sp, #16]\n"
            "stp x22, x21, [sp, #32]\n"
            "stp x20, x19, [sp, #48]\n"
            "adrp x16, dobby_packet_security_continue\n"
            "ldr x16, [x16, :lo12:dobby_packet_security_continue]\n"
            "br x16\n");
}

extern "C" [[gnu::naked]] void dobby_packet_read_trampoline() {
    asm volatile(
            "sub sp, sp, #416\n"
            "stp x29, x30, [sp, #352]\n"
            "stp x28, x23, [sp, #368]\n"
            "stp x22, x21, [sp, #384]\n"
            "adrp x16, dobby_packet_read_continue\n"
            "ldr x16, [x16, :lo12:dobby_packet_read_continue]\n"
            "br x16\n");
}

extern "C" [[gnu::naked]] void dobby_packet_read_detour() {
    asm volatile(
            // x8 is the hidden Bedrock::Result<void> return storage. Preserve
            // it, the Packet*, and the caller's link register across the
            // original shared Packet::read body and the passive capture call.
            "sub sp, sp, #48\n"
            "stp x0, x1, [sp, #0]\n"
            "str x8, [sp, #16]\n"
            "str x30, [sp, #24]\n"
            "bl dobby_packet_read_trampoline\n"
            "ldr x0, [sp, #16]\n"
            "ldr x1, [sp, #0]\n"
            "bl dobby_capture_packet_read_result\n"
            "ldr x8, [sp, #16]\n"
            "ldr x30, [sp, #24]\n"
            "add sp, sp, #48\n"
            "ret\n");
}

extern "C" [[gnu::naked]] void dobby_handle_violation_trampoline() {
    asm volatile(
            "stp x29, x30, [sp, #-96]!\n"
            "stp x28, x27, [sp, #16]\n"
            "stp x26, x25, [sp, #32]\n"
            "stp x24, x23, [sp, #48]\n"
            "adrp x16, dobby_handle_violation_continue\n"
            "ldr x16, [x16, :lo12:dobby_handle_violation_continue]\n"
            "br x16\n");
}

namespace dobby {
namespace {

using GetIdFn = std::int32_t (*)(const void* packet);
using PacketSecurityCheckForViolationFn = std::int32_t (*)(
        void* controller, std::int32_t packetId, std::uint8_t clientSubId,
        const void* expectedResult, bool* outNewOrUpdated);
using HandlePacketViolationFn = void (*)(
        void* handler, const void* packetSecurityController,
        const void* errorCode, std::int32_t response, std::int32_t packetId,
        void* context, const void* networkIdentifier,
        std::uint8_t clientSubId, std::uint8_t senderSubId,
        std::uint32_t packetSize);
using AllowIncomingPacketIdFn = std::int32_t (*)(
        void* handler, const void* networkIdentifierWithSubId,
        std::int32_t packetId, std::uint64_t packetSize);
using OnDisconnectFn = void (*)(
        void* handler, const void* source, std::int32_t disconnectReason,
        std::int32_t disconnectStage, const void* messageFromServer,
        const void* messageBodyOverride, bool skipMessage,
        const void* telemetryOverride);
GetIdFn originalViolationGetId = nullptr;
// These are deliberately volatile. Their targets are naked trampolines whose
// inline assembly consumes the complete entry ABI; keeping calls indirect
// prevents the compiler from proving the C++ parameter list unused and
// eliding argument restoration at the call site.
PacketSecurityCheckForViolationFn volatile originalPacketSecurityCheckForViolation = nullptr;
HandlePacketViolationFn volatile originalHandlePacketViolation = nullptr;
AllowIncomingPacketIdFn originalAllowIncomingPacketId = nullptr;
OnDisconnectFn originalOnDisconnect = nullptr;
std::atomic_bool schemaTraceEnabled{false};
thread_local bool handlingViolation = false;
thread_local bool handlingDirectViolation = false;
thread_local bool handlingDisconnect = false;
thread_local const void* lastViolationPacket = nullptr;
thread_local std::chrono::steady_clock::time_point lastViolationAt{};
MinecraftImage minecraftImage;

struct InboundPacketObservation {
    std::int32_t packetId{-1};
    std::uint64_t packetSize{};
    std::chrono::steady_clock::time_point observedAt{};
    bool valid{false};
};

thread_local InboundPacketObservation lastInboundPacket;
constexpr std::size_t packetHistoryCapacity = 32;
thread_local std::array<InboundPacketObservation, packetHistoryCapacity> inboundPacketHistory;
thread_local std::size_t inboundPacketHistoryNext = 0;
thread_local std::size_t inboundPacketHistoryCount = 0;

struct UniversalValidationObservation {
    std::int32_t packetId{-1};
    std::chrono::steady_clock::time_point capturedAt{};
    std::optional<StreamFailure> streamFailure;
    std::optional<ValidationEvidence> validation;
    bool valid{false};
};

thread_local UniversalValidationObservation lastUniversalValidation;

struct UnwindCapture {
    std::vector<std::uint64_t>* offsets{};
};

_Unwind_Reason_Code captureMinecraftFrame(
        _Unwind_Context* context, void* argument) {
    auto& capture = *static_cast<UnwindCapture*>(argument);
    const auto programCounter = static_cast<std::uintptr_t>(
            _Unwind_GetIP(context));
    if (addressIsExecutable(minecraftImage, programCounter) &&
        capture.offsets->size() < 16) {
        capture.offsets->push_back(programCounter - minecraftImage.base);
    }
    return capture.offsets->size() >= 16
            ? _URC_END_OF_STACK : _URC_NO_REASON;
}

std::vector<std::uint64_t> captureMinecraftStack() {
    std::vector<std::uint64_t> offsets;
    offsets.reserve(16);
    UnwindCapture capture{&offsets};
    _Unwind_Backtrace(captureMinecraftFrame, &capture);
    return offsets;
}

std::vector<PacketHistoryEntry> snapshotInboundPacketHistory(
        std::chrono::steady_clock::time_point now) {
    std::vector<PacketHistoryEntry> result;
    result.reserve(inboundPacketHistoryCount);
    const auto oldest =
            (inboundPacketHistoryNext + packetHistoryCapacity - inboundPacketHistoryCount) %
            packetHistoryCapacity;
    for (std::size_t index = 0; index < inboundPacketHistoryCount; ++index) {
        const auto& packet = inboundPacketHistory[(oldest + index) % packetHistoryCapacity];
        if (!packet.valid)
            continue;
        const auto age = now >= packet.observedAt
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - packet.observedAt).count()
                : 0;
        result.push_back({
                packet.packetId, packet.packetSize,
                static_cast<std::uint64_t>(age)});
    }
    return result;
}

void rememberInboundPacket(const InboundPacketObservation& packet) {
    inboundPacketHistory[inboundPacketHistoryNext] = packet;
    inboundPacketHistoryNext = (inboundPacketHistoryNext + 1) % packetHistoryCapacity;
    inboundPacketHistoryCount = std::min(
            inboundPacketHistoryCount + 1, packetHistoryCapacity);
}

void readErrorText(
        std::int32_t errorValue, const void* errorCategory,
        std::string& categoryText, std::string& messageText) {
    if (errorCategory == nullptr)
        return;
    try {
        const auto* category = static_cast<const std::error_category*>(
                errorCategory);
        if (const auto* name = category->name(); name != nullptr)
            categoryText = name;
        messageText = std::error_code(errorValue, *category).message();
    } catch (...) {
        categoryText = "unavailable";
        messageText = "error category formatting failed";
    }
    constexpr std::size_t maximumTextLength = 1024;
    if (categoryText.size() > maximumTextLength)
        categoryText.resize(maximumTextLength);
    if (messageText.size() > maximumTextLength)
        messageText.resize(maximumTextLength);
}

ValidationSourceEvidence sourceEvidence(const ValidationSourceFrame& frame) {
    return {
            frame.filenameHash, frame.filename, frame.line, frame.context};
}

void populateValidationProvenance(
        const ValidationResultView& view, ValidationEvidence& evidence) {
    readErrorText(
            view.errorValue, view.errorCategory,
            evidence.errorCategory, evidence.errorMessage);
    evidence.provenanceTruncated = view.provenanceTruncated;
    evidence.sourceFrames.reserve(view.sourceFrames.size());
    for (const auto& frame : view.sourceFrames)
        evidence.sourceFrames.push_back(sourceEvidence(frame));
    evidence.nestedErrors.reserve(view.nestedErrors.size());
    for (const auto& nested : view.nestedErrors) {
        ValidationNestedErrorEvidence nestedEvidence;
        nestedEvidence.depth = nested.depth;
        nestedEvidence.errorValue = nested.errorValue;
        readErrorText(
                nested.errorValue, nested.errorCategory,
                nestedEvidence.errorCategory, nestedEvidence.errorMessage);
        nestedEvidence.sourceFrames.reserve(nested.sourceFrames.size());
        for (const auto& frame : nested.sourceFrames)
            nestedEvidence.sourceFrames.push_back(sourceEvidence(frame));
        evidence.nestedErrors.push_back(std::move(nestedEvidence));
    }
}

bool recentUniversalEvidence(
        std::int32_t packetId, std::optional<StreamFailure>& streamFailure,
        std::optional<ValidationEvidence>& validation) {
    const auto now = std::chrono::steady_clock::now();
    if (!lastUniversalValidation.valid ||
        lastUniversalValidation.packetId != packetId ||
        now - lastUniversalValidation.capturedAt > std::chrono::seconds(10)) {
        return false;
    }
    streamFailure = lastUniversalValidation.streamFailure;
    validation = lastUniversalValidation.validation;
    return true;
}

void persistDiagnostic(const Diagnostic& diagnostic) {
    writeFile(latestPath(), diagnostic.report, "w");
    writeFileAtomically(latestAiPath(), diagnostic.json + "\n");
    writeFile(eventPath(), diagnostic.json + "\n", "a");
    logLine(diagnostic.json);
}

void captureValidationFailure(
        const ValidationResultView& decoded, std::int32_t packetId,
        std::int32_t response, bool newOrUpdated, const char* intercept,
        const char* context) {
    ValidationEvidence evidence;
    evidence.resultSuccess = false;
    evidence.response = response;
    evidence.newOrUpdated = newOrUpdated;
    evidence.errorValue = decoded.errorValue;
    populateValidationProvenance(decoded, evidence);
    evidence.nativeStackImageOffsets = captureMinecraftStack();
    const auto now = std::chrono::steady_clock::now();
    evidence.recentPackets = snapshotInboundPacketHistory(now);

    auto streamFailure = recentStreamFailure(std::chrono::seconds(10));
    lastUniversalValidation = {
            packetId, now, streamFailure, evidence, true};

    const auto severity = response >= 1 && response <= 3 ? response - 1 : -1;
    ViolationRecord record{
            -1, severity, packetId, context, "validation_result"};
    auto diagnostic = buildDiagnostic(
            record, std::move(streamFailure), intercept, evidence);
    persistDiagnostic(diagnostic);
    runtimeState().addDiagnostic(std::move(diagnostic));
    if (runtimeState().autoPopup())
        requestLatestViolationPopup();
}

extern "C" void dobby_capture_packet_read_result(
        const void* expectedResult, const void*) {
    if (expectedResult == nullptr)
        return;
    std::uint8_t hasValue = 0;
    std::memcpy(
            &hasValue,
            static_cast<const std::byte*>(expectedResult) +
                    kExpectedHasValueOffset,
            sizeof(hasValue));
    if ((hasValue & 1U) != 0U)
        return;

    const auto decoded = decodeValidationResult(expectedResult);
    if (!decoded || decoded->success)
        return;

    const auto now = std::chrono::steady_clock::now();
    const bool hasRecentPacket = lastInboundPacket.valid &&
            now - lastInboundPacket.observedAt <= std::chrono::seconds(10);
    const auto packetId = hasRecentPacket ? lastInboundPacket.packetId : -1;
    captureValidationFailure(
            *decoded, packetId, -1, false,
            "Packet::read(ReadOnlyBinaryStream&) inline return",
            "Packet::read returned a deserialize error");
}

std::int32_t packetSecurityCheckForViolationDetour(
        void* controller, std::int32_t packetId, std::uint8_t clientSubId,
        const void* expectedResult, bool* outNewOrUpdated) {
    const auto response = originalPacketSecurityCheckForViolation != nullptr
            ? originalPacketSecurityCheckForViolation(
                      controller, packetId, clientSubId, expectedResult,
                      outNewOrUpdated)
            : 0;

    if (expectedResult == nullptr)
        return response;
    std::uint8_t hasValue = 0;
    std::memcpy(
            &hasValue,
            static_cast<const std::byte*>(expectedResult) +
                    kExpectedHasValueOffset,
            sizeof(hasValue));
    if ((hasValue & 1U) != 0U)
        return response;

    const auto decoded = decodeValidationResult(expectedResult);
    if (!decoded || decoded->success)
        return response;

    bool newOrUpdated = false;
    if (outNewOrUpdated != nullptr) {
        std::array<std::byte, 1> value{};
        if (copyReadableMemory(outNewOrUpdated, value))
            newOrUpdated = value[0] != std::byte{0};
    }
    captureValidationFailure(
            *decoded, packetId, response, newOrUpdated,
            "PacketSecurityController::checkForViolation inline entry",
            "PacketSecurityController::checkForViolation rejected inbound packet");
    return response;
}

void handleViolation(const char* intercept, const void* packet) {
    if (handlingViolation || handlingDirectViolation)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (packet == lastViolationPacket && now - lastViolationAt < std::chrono::seconds(1))
        return;
    lastViolationPacket = packet;
    lastViolationAt = now;
    handlingViolation = true;

    const auto record = decodeViolation(packet);
    if (!record) {
        logLine(std::string("ERROR: unable to decode PacketViolationWarningPacket at ") + intercept);
        handlingViolation = false;
        return;
    }

    auto streamFailure = recentStreamFailure(std::chrono::seconds(10));
    std::optional<ValidationEvidence> validation;
    recentUniversalEvidence(record->packetId, streamFailure, validation);
    auto diagnostic = buildDiagnostic(
            *record, std::move(streamFailure), intercept, std::move(validation));
    persistDiagnostic(diagnostic);
    runtimeState().addDiagnostic(std::move(diagnostic));

    // Every violation is eligible for a popup. History retention and repeated
    // payloads never suppress a later disconnect after the user closes a window.
    // Queue it for the launcher's render/UI thread so worker-thread callbacks
    // cannot lose the window.
    if (runtimeState().autoPopup())
        requestLatestViolationPopup();
    handlingViolation = false;
}

bool captureDirectViolation(
        std::int32_t response, std::int32_t packetId,
        const void* errorCode, const void* context) {
    if (handlingViolation)
        return false;
    handlingViolation = true;

    const auto record = decodeViolationArguments(response, packetId, context);
    if (!record) {
        logLine("ERROR: unable to decode ClientNetworkHandler::handlePacketViolation arguments");
        handlingViolation = false;
        return false;
    }

    auto streamFailure = recentStreamFailure(std::chrono::seconds(10));
    std::optional<ValidationEvidence> validation;
    recentUniversalEvidence(packetId, streamFailure, validation);
    if (!validation) {
        const auto decodedError = decodeErrorCode(errorCode);
        if (decodedError) {
            ValidationEvidence directEvidence;
            directEvidence.resultSuccess = false;
            directEvidence.response = response;
            directEvidence.errorValue = decodedError->value;
            readErrorText(
                    decodedError->value, decodedError->category,
                    directEvidence.errorCategory,
                    directEvidence.errorMessage);
            directEvidence.nativeStackImageOffsets = captureMinecraftStack();
            directEvidence.recentPackets = snapshotInboundPacketHistory(
                    std::chrono::steady_clock::now());
            validation = std::move(directEvidence);
        }
    }
    auto diagnostic = buildDiagnostic(
            *record, std::move(streamFailure),
            "ClientNetworkHandler::handlePacketViolation inline entry",
            std::move(validation));
    persistDiagnostic(diagnostic);
    runtimeState().addDiagnostic(std::move(diagnostic));
    handlingViolation = false;
    return true;
}

void handlePacketViolationDetour(
        void* handler, const void* packetSecurityController,
        const void* errorCode, std::int32_t response, std::int32_t packetId,
        void* context, const void* networkIdentifier,
        std::uint8_t clientSubId, std::uint8_t senderSubId,
        std::uint32_t packetSize) {
    const bool outermost = !handlingDirectViolation;
    bool captured = false;
    if (outermost) {
        handlingDirectViolation = true;
        captured = captureDirectViolation(
                response, packetId, errorCode, context);
    }

    if (originalHandlePacketViolation != nullptr) {
        originalHandlePacketViolation(
                handler, packetSecurityController, errorCode, response,
                packetId, context, networkIdentifier, clientSubId,
                senderSubId, packetSize);
    }

    if (outermost) {
        handlingDirectViolation = false;
        // Bedrock builds the disconnect UI inside the original callback for a
        // terminating violation. Queue Dobby afterward so its exact diagnostic
        // is not hidden behind the generic Block screen.
        if (captured && runtimeState().autoPopup())
            requestLatestViolationPopup();
    }
}

std::int32_t allowIncomingPacketIdDetour(
        void* handler, const void* networkIdentifierWithSubId,
        std::int32_t packetId, std::uint64_t packetSize) {
    lastInboundPacket = {
            packetId, packetSize, std::chrono::steady_clock::now(), true};
    rememberInboundPacket(lastInboundPacket);
    return originalAllowIncomingPacketId != nullptr
            ? originalAllowIncomingPacketId(
                      handler, networkIdentifierWithSubId, packetId, packetSize)
            : 0;
}

bool captureBadPacketDisconnect(
        const void* source, std::int32_t disconnectStage,
        const void* messageFromServer, const void* messageBodyOverride,
        bool skipMessage, const void* telemetryOverride) {
    const auto now = std::chrono::steady_clock::now();
    const bool hasRecentPacket = lastInboundPacket.valid &&
            now - lastInboundPacket.observedAt <= std::chrono::seconds(10);
    const auto packetId = hasRecentPacket ? lastInboundPacket.packetId : -1;
    const auto packetSize = hasRecentPacket ? lastInboundPacket.packetSize : 0;
    const auto record = decodeBadPacketDisconnect(
            packetId, packetSize, messageFromServer, messageBodyOverride);
    if (!record) {
        logLine("ERROR: unable to decode ClientNetworkHandler::onDisconnect BadPacket arguments");
        return false;
    }

    auto streamFailure = recentStreamFailure(std::chrono::seconds(10));
    std::optional<ValidationEvidence> validation;
    recentUniversalEvidence(packetId, streamFailure, validation);
    auto disconnect = decodeDisconnectArguments(
            90, disconnectStage, messageFromServer, messageBodyOverride,
            skipMessage, source != nullptr, telemetryOverride != nullptr);
    if (disconnect) {
        disconnect->recentPackets = snapshotInboundPacketHistory(now);
        disconnect->nativeStackImageOffsets = captureMinecraftStack();
    }
    auto diagnostic = buildDiagnostic(
            *record, std::move(streamFailure),
            "ClientNetworkHandler::onDisconnect BadPacket + allowIncomingPacketId",
            std::move(validation), std::move(disconnect));
    persistDiagnostic(diagnostic);
    runtimeState().addDiagnostic(std::move(diagnostic));
    return true;
}

bool captureDisconnect(
        const void* source, std::int32_t disconnectReason,
        std::int32_t disconnectStage, const void* messageFromServer,
        const void* messageBodyOverride, bool skipMessage,
        const void* telemetryOverride) {
    auto evidence = decodeDisconnectArguments(
            disconnectReason, disconnectStage, messageFromServer,
            messageBodyOverride, skipMessage, source != nullptr,
            telemetryOverride != nullptr);
    if (!evidence) {
        logLine("ERROR: unable to decode ClientNetworkHandler::onDisconnect arguments");
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    evidence->recentPackets = snapshotInboundPacketHistory(now);
    evidence->nativeStackImageOffsets = captureMinecraftStack();
    auto diagnostic = buildDisconnectDiagnostic(
            std::move(*evidence),
            "ClientNetworkHandler::onDisconnect inline entry");
    persistDiagnostic(diagnostic);
    runtimeState().addDiagnostic(std::move(diagnostic));
    return true;
}

void onDisconnectDetour(
        void* handler, const void* source, std::int32_t disconnectReason,
        std::int32_t disconnectStage, const void* messageFromServer,
        const void* messageBodyOverride, bool skipMessage,
        const void* telemetryOverride) {
    constexpr std::int32_t badPacketReason = 90;
    const bool outermost = !handlingDisconnect;
    bool captured = false;
    if (outermost) {
        handlingDisconnect = true;
        if (disconnectReason == badPacketReason) {
            captured = captureBadPacketDisconnect(
                    source, disconnectStage, messageFromServer,
                    messageBodyOverride, skipMessage, telemetryOverride);
        } else {
            captured = captureDisconnect(
                    source, disconnectReason, disconnectStage,
                    messageFromServer, messageBodyOverride, skipMessage,
                    telemetryOverride);
        }
    }

    if (originalOnDisconnect != nullptr) {
        originalOnDisconnect(
                handler, source, disconnectReason, disconnectStage,
                messageFromServer, messageBodyOverride, skipMessage,
                telemetryOverride);
    }

    if (outermost) {
        handlingDisconnect = false;
        lastInboundPacket.valid = false;
        // Bedrock creates its codeword screen inside this callback. Queue the
        // Dobby window afterward so the exact reason and evidence stay visible.
        if (captured && runtimeState().autoPopup())
            requestLatestViolationPopup();
    }
}

std::int32_t violationGetIdDetour(const void* packet) {
    handleViolation("PacketViolationWarningPacket::getId vtable", packet);
    return originalViolationGetId != nullptr ? originalViolationGetId(packet) : 156;
}

bool schemaPushMemberDetour(const void*, std::string_view name) {
    if (schemaTraceEnabled.load(std::memory_order_relaxed))
        pushClientSchemaMember(name);
    return true;
}

void schemaPushElementDetour(const void*, std::uint64_t index) {
    if (schemaTraceEnabled.load(std::memory_order_relaxed))
        pushClientSchemaElement(index);
}

void schemaPopDetour(const void*) {
    if (schemaTraceEnabled.load(std::memory_order_relaxed))
        popClientSchemaContext();
}

bool patchSchemaSlot(void** slot, void* replacement) {
    return mcpelauncher_patch(slot, &replacement, sizeof(replacement)) != nullptr &&
           *slot == replacement;
}

bool installClientSchemaProbe(const MinecraftImage& image) {
    const auto pushMemberAddress = image.base + target::kSchemaPushMemberOffset;
    const auto pushElementAddress = image.base + target::kSchemaPushElementOffset;
    const auto popAddress = image.base + target::kSchemaPopOffset;
    auto* pushMemberSlot = reinterpret_cast<void**>(
            image.base + target::kSchemaPushMemberVtableSlotOffset);
    auto* pushElementSlot = reinterpret_cast<void**>(
            image.base + target::kSchemaPushElementVtableSlotOffset);
    auto* popSlot = reinterpret_cast<void**>(image.base + target::kSchemaPopVtableSlotOffset);

    const bool valid =
            addressIsExecutable(image, pushMemberAddress) &&
            addressIsExecutable(image, pushElementAddress) &&
            addressIsExecutable(image, popAddress) &&
            matchesSignature(reinterpret_cast<const void*>(pushMemberAddress),
                             target::kSchemaPushMemberSignature) &&
            matchesSignature(reinterpret_cast<const void*>(pushElementAddress),
                             target::kSchemaPushElementSignature) &&
            matchesSignature(reinterpret_cast<const void*>(popAddress),
                             target::kSchemaPopSignature) &&
            *pushMemberSlot == reinterpret_cast<void*>(pushMemberAddress) &&
            *pushElementSlot == reinterpret_cast<void*>(pushElementAddress) &&
            *popSlot == reinterpret_cast<void*>(popAddress);
    if (!valid) {
        logLine("ERROR: PacketSchemaReader layout mismatch; client field trace disabled");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable; client field trace disabled");
        return false;
    }

    schemaTraceEnabled.store(false, std::memory_order_relaxed);
    if (!patchSchemaSlot(pushMemberSlot, reinterpret_cast<void*>(schemaPushMemberDetour)) ||
        !patchSchemaSlot(pushElementSlot, reinterpret_cast<void*>(schemaPushElementDetour)) ||
        !patchSchemaSlot(popSlot, reinterpret_cast<void*>(schemaPopDetour))) {
        logLine("ERROR: launcher rejected a PacketSchemaReader hook; client field trace disabled");
        return false;
    }
    clearClientSchemaTrace();
    schemaTraceEnabled.store(true, std::memory_order_release);
    logLine("installed PacketSchemaReader runtime field trace");
    return true;
}

bool installStreamProbe(const MinecraftImage& image) {
    const auto functionAddress = image.base + target::kStreamReadOffset;
    if (!addressIsExecutable(image, functionAddress) ||
        !matchesSignature(reinterpret_cast<const void*>(functionAddress), target::kStreamReadSignature)) {
        logLine("ERROR: ReadOnlyBinaryStream::read signature mismatch; raw stream probe disabled");
        return false;
    }

    auto* slot = reinterpret_cast<void**>(image.base + target::kStreamReadVtableSlotOffset);
    if (*slot != reinterpret_cast<void*>(functionAddress)) {
        logLine("ERROR: ReadOnlyBinaryStream::read vtable mismatch; raw stream probe disabled");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable; raw stream probe disabled");
        return false;
    }

    // ldr x16, #8; br x16; .quad detour. The absolute target avoids the
    // AArch64 +/-128 MiB direct-branch limit between libminecraftpe and Dobby.
    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(dobby_stream_read_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));

    dobby_stream_read_continue = reinterpret_cast<void*>(functionAddress + replacement.size());
    auto* entry = reinterpret_cast<void*>(functionAddress);
    if (mcpelauncher_patch(entry, replacement.data(), replacement.size()) == nullptr ||
        std::memcmp(entry, replacement.data(), replacement.size()) != 0) {
        dobby_stream_read_continue = nullptr;
        logLine("ERROR: launcher rejected ReadOnlyBinaryStream::read inline probe");
        return false;
    }
    logLine("installed ReadOnlyBinaryStream::read inline byte-trace probe");
    return true;
}

bool installPacketEndProbe(const MinecraftImage& image) {
    const auto functionAddress = image.base + target::kPacketEndCheckOffset;
    if (!addressIsExecutable(image, functionAddress) ||
        !matchesSignature(reinterpret_cast<const void*>(functionAddress),
                          target::kPacketEndCheckSignature)) {
        logLine("ERROR: packet completion signature mismatch; exact boundary probe disabled");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable; exact boundary probe disabled");
        return false;
    }

    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(dobby_packet_end_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));

    dobby_packet_end_continue = reinterpret_cast<void*>(functionAddress + replacement.size());
    auto* entry = reinterpret_cast<void*>(functionAddress);
    if (mcpelauncher_patch(entry, replacement.data(), replacement.size()) == nullptr ||
        std::memcmp(entry, replacement.data(), replacement.size()) != 0) {
        dobby_packet_end_continue = nullptr;
        logLine("ERROR: launcher rejected packet completion inline probe");
        return false;
    }
    logLine("installed exact packet completion boundary probe");
    return true;
}

bool installPacketReadResultHook(const MinecraftImage& image) {
    const auto functionAddress = image.base + target::kPacketReadOffset;
    auto* verificationSlot = reinterpret_cast<void**>(
            image.base + target::kPacketReadVerificationVtableSlotOffset);
    const bool valid =
            addressIsExecutable(image, functionAddress) &&
            matchesSignature(
                    reinterpret_cast<const void*>(functionAddress),
                    target::kPacketReadSignature) &&
            *verificationSlot == reinterpret_cast<void*>(functionAddress);
    if (!valid) {
        logLine("ERROR: Packet::read ABI mismatch; universal deserialize-result capture disabled");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable for Packet::read result capture");
        return false;
    }

    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(
            dobby_packet_read_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));

    dobby_packet_read_continue =
            reinterpret_cast<void*>(functionAddress + replacement.size());
    auto* entry = reinterpret_cast<void*>(functionAddress);
    if (mcpelauncher_patch(
                entry, replacement.data(), replacement.size()) == nullptr ||
        std::memcmp(entry, replacement.data(), replacement.size()) != 0) {
        dobby_packet_read_continue = nullptr;
        logLine("ERROR: launcher rejected Packet::read result hook");
        return false;
    }
    logLine("installed universal Packet::read deserialize-result inline hook");
    return true;
}

bool installUniversalValidationHook(const MinecraftImage& image) {
    const auto functionAddress =
            image.base + target::kPacketSecurityCheckForViolationOffset;
    auto* slot = reinterpret_cast<void**>(
            image.base +
            target::kPacketSecurityCheckForViolationVtableSlotOffset);
    const bool valid =
            addressIsExecutable(image, functionAddress) &&
            matchesSignature(
                    reinterpret_cast<const void*>(functionAddress),
                    target::kPacketSecurityCheckForViolationSignature) &&
            *slot == reinterpret_cast<void*>(functionAddress);
    if (!valid) {
        logLine("ERROR: PacketSecurityController::checkForViolation ABI mismatch; universal validation capture disabled");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable for universal validation capture");
        return false;
    }

    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(
            packetSecurityCheckForViolationDetour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));

    dobby_packet_security_continue =
            reinterpret_cast<void*>(functionAddress + replacement.size());
    originalPacketSecurityCheckForViolation =
            reinterpret_cast<PacketSecurityCheckForViolationFn>(
                    dobby_packet_security_trampoline);
    auto* entry = reinterpret_cast<void*>(functionAddress);
    if (mcpelauncher_patch(
                entry, replacement.data(), replacement.size()) == nullptr ||
        std::memcmp(entry, replacement.data(), replacement.size()) != 0) {
        dobby_packet_security_continue = nullptr;
        originalPacketSecurityCheckForViolation = nullptr;
        logLine("ERROR: launcher rejected PacketSecurityController::checkForViolation hook");
        return false;
    }
    logLine("installed universal PacketSecurityController::checkForViolation inline hook");
    return true;
}

bool installViolationHook(const MinecraftImage& image) {
    const auto functionAddress = image.base + target::kViolationGetIdOffset;
    if (!addressIsExecutable(image, functionAddress) ||
        !matchesSignature(reinterpret_cast<const void*>(functionAddress), target::kViolationGetIdSignature)) {
        logLine("ERROR: PacketViolationWarningPacket::getId signature mismatch");
        return false;
    }

    auto* slot = reinterpret_cast<void**>(image.base + target::kViolationGetIdVtableSlotOffset);
    if (*slot != reinterpret_cast<void*>(functionAddress)) {
        logLine("ERROR: PacketViolationWarningPacket vtable mismatch");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable");
        return false;
    }

    originalViolationGetId = reinterpret_cast<GetIdFn>(*slot);
    void* replacement = reinterpret_cast<void*>(violationGetIdDetour);
    if (mcpelauncher_patch(slot, &replacement, sizeof(replacement)) == nullptr || *slot != replacement) {
        originalViolationGetId = nullptr;
        logLine("ERROR: launcher rejected PacketViolationWarningPacket hook");
        return false;
    }
    logLine("installed PacketViolationWarningPacket::getId hook");
    return true;
}

bool installDirectViolationHook(const MinecraftImage& image) {
    const auto functionAddress = image.base + target::kHandlePacketViolationOffset;
    if (!addressIsExecutable(image, functionAddress) ||
        !matchesSignature(reinterpret_cast<const void*>(functionAddress),
                          target::kHandlePacketViolationSignature)) {
        logLine("ERROR: ClientNetworkHandler::handlePacketViolation signature mismatch");
        return false;
    }

    auto* slot = reinterpret_cast<void**>(
            image.base + target::kHandlePacketViolationVtableSlotOffset);
    if (*slot != reinterpret_cast<void*>(functionAddress)) {
        logLine("ERROR: ClientNetworkHandler::handlePacketViolation vtable mismatch");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable for direct violation hook");
        return false;
    }

    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(
            handlePacketViolationDetour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));

    dobby_handle_violation_continue =
            reinterpret_cast<void*>(functionAddress + replacement.size());
    originalHandlePacketViolation =
            reinterpret_cast<HandlePacketViolationFn>(
                    dobby_handle_violation_trampoline);
    auto* entry = reinterpret_cast<void*>(functionAddress);
    if (mcpelauncher_patch(
                entry, replacement.data(), replacement.size()) == nullptr ||
        std::memcmp(entry, replacement.data(), replacement.size()) != 0) {
        dobby_handle_violation_continue = nullptr;
        originalHandlePacketViolation = nullptr;
        logLine("ERROR: launcher rejected ClientNetworkHandler::handlePacketViolation hook");
        return false;
    }
    logLine("installed ClientNetworkHandler::handlePacketViolation inline hook");
    return true;
}

bool installDisconnectDiagnosticHooks(const MinecraftImage& image) {
    const auto disconnectAddress = image.base + target::kOnDisconnectOffset;
    const auto allowAddress = image.base + target::kAllowIncomingPacketIdOffset;
    auto* disconnectSlot = reinterpret_cast<void**>(
            image.base + target::kOnDisconnectVtableSlotOffset);
    auto* allowSlot = reinterpret_cast<void**>(
            image.base + target::kAllowIncomingPacketIdVtableSlotOffset);

    const bool valid =
            addressIsExecutable(image, disconnectAddress) &&
            addressIsExecutable(image, allowAddress) &&
            matchesSignature(
                    reinterpret_cast<const void*>(disconnectAddress),
                    target::kOnDisconnectSignature) &&
            matchesSignature(
                    reinterpret_cast<const void*>(allowAddress),
                    target::kAllowIncomingPacketIdSignature) &&
            *disconnectSlot == reinterpret_cast<void*>(disconnectAddress) &&
            *allowSlot == reinterpret_cast<void*>(allowAddress);
    if (!valid) {
        logLine("ERROR: disconnect diagnostic hook ABI mismatch");
        return false;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("ERROR: launcher patch API unavailable for disconnect diagnostics");
        return false;
    }

    originalOnDisconnect = reinterpret_cast<OnDisconnectFn>(*disconnectSlot);
    originalAllowIncomingPacketId =
            reinterpret_cast<AllowIncomingPacketIdFn>(*allowSlot);
    void* allowReplacement = reinterpret_cast<void*>(allowIncomingPacketIdDetour);
    if (!patchSchemaSlot(allowSlot, allowReplacement)) {
        originalOnDisconnect = nullptr;
        originalAllowIncomingPacketId = nullptr;
        logLine("ERROR: launcher rejected allowIncomingPacketId hook");
        return false;
    }

    void* disconnectReplacement = reinterpret_cast<void*>(onDisconnectDetour);
    if (!patchSchemaSlot(disconnectSlot, disconnectReplacement)) {
        void* original = reinterpret_cast<void*>(originalAllowIncomingPacketId);
        patchSchemaSlot(allowSlot, original);
        originalOnDisconnect = nullptr;
        originalAllowIncomingPacketId = nullptr;
        logLine("ERROR: launcher rejected onDisconnect hook");
        return false;
    }

    logLine("installed all-reason disconnect diagnostic hooks");
    return true;
}

} // namespace

void installPacketHooks() {
    const auto image = findMinecraftImage();
    if (image.base == 0 || image.executableBegin == 0) {
        runtimeState().setHookStatus("failed: libminecraftpe.so not found", false, false);
        recordLifecycleEvent("hook_error", "libminecraftpe.so not found");
        logLine("ERROR: libminecraftpe.so not found");
        return;
    }
    minecraftImage = image;

    const bool byteTraceProbe = installStreamProbe(image);
    const bool packetEndProbe = installPacketEndProbe(image);
    const bool schemaProbe = installClientSchemaProbe(image);
    const bool streamProbe = byteTraceProbe || packetEndProbe;
    const bool packetReadResult = installPacketReadResultHook(image);
    const bool universalValidation = installUniversalValidationHook(image);
    const bool disconnectCorrelation = installDisconnectDiagnosticHooks(image);
    const bool directViolationHook = installDirectViolationHook(image);
    const bool warningHook = installViolationHook(image);
    if (!packetReadResult && !universalValidation && !disconnectCorrelation &&
        !directViolationHook && !warningHook) {
        runtimeState().setHookStatus("failed: violation hook unavailable", false, streamProbe);
        recordLifecycleEvent("hook_error", "universal, direct, disconnect, and warning violation hooks unavailable");
        return;
    }

    const bool complete = packetReadResult && universalValidation && disconnectCorrelation &&
            directViolationHook && warningHook && byteTraceProbe &&
            packetEndProbe && schemaProbe;
    const std::string status = complete
            ? "active: universal read/validation + disconnect/warning + byte/field trace"
            : packetReadResult && universalValidation && streamProbe
            ? "active: universal read/validation + partial downstream/stream capture"
            : universalValidation && streamProbe
            ? "active: universal validation + partial downstream/stream capture"
            : universalValidation
            ? "active: universal validation; downstream/stream capture partial"
            : packetReadResult && streamProbe
            ? "active: universal deserialize results + partial downstream/stream capture"
            : packetReadResult
            ? "active: universal deserialize results; validation/downstream capture partial"
            : "active: fallback violation capture; universal validation unavailable";
    runtimeState().setHookStatus(
            status,
            packetReadResult || universalValidation || disconnectCorrelation ||
                    directViolationHook || warningHook,
            streamProbe);
    recordLifecycleEvent(
            "hook_ready",
            complete
                    ? "universal inbound deserialize results and validation, downstream violations, disconnect correlation, packet boundaries, byte tracing, and client field tracing active"
                    : status);
    logLine(std::string("READY: Dobby ") + kDobbyVersion + " developer diagnostics active");
}

} // namespace dobby
#endif
