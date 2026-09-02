#include "diagnostics/stream_probe.hpp"

#include "core/constants.hpp"
#include "diagnostics/client_schema_trace.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace dobby {
namespace {

constexpr std::size_t kMaximumTraceAttempts = 96;

template <class T>
T readUnaligned(const std::byte* address) {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

struct ActiveTrace {
    const void* stream{};
    const std::uint8_t* data{};
    std::size_t size{};
    std::size_t previousOffset{};
    std::array<std::uint8_t, kMaximumRawCaptureLimit> rawBytes{};
    std::size_t rawSize{};
    bool rawBytesTruncated{};
    std::array<StreamReadAttempt, kMaximumTraceAttempts> attempts{};
    std::size_t attemptCount{};
    std::size_t nextAttempt{};
    std::chrono::steady_clock::time_point lastObservedAt{};
};

struct TimedFailure {
    StreamFailure failure;
    std::chrono::steady_clock::time_point capturedAt;
};

thread_local ActiveTrace activeTrace;
std::mutex failureMutex;
std::optional<TimedFailure> lastFailure;

void resetTrace(const void* stream, const StreamView& view, std::size_t rawCaptureLimit) {
    activeTrace.stream = stream;
    activeTrace.data = view.data;
    activeTrace.size = view.size;
    activeTrace.previousOffset = view.readPointer;
    activeTrace.rawSize = std::min({view.size, rawCaptureLimit, kMaximumRawCaptureLimit});
    std::memcpy(activeTrace.rawBytes.data(), view.data, activeTrace.rawSize);
    activeTrace.rawBytesTruncated = activeTrace.rawSize < view.size;
    activeTrace.attemptCount = 0;
    activeTrace.nextAttempt = 0;
    activeTrace.lastObservedAt = std::chrono::steady_clock::now();
}

StreamFailure snapshotTrace(bool overflowObserved) {
    StreamFailure snapshot;
    snapshot.overflowObserved = overflowObserved;
    snapshot.viewSize = activeTrace.size;
    snapshot.rawBytes.assign(
            activeTrace.rawBytes.begin(), activeTrace.rawBytes.begin() + activeTrace.rawSize);
    snapshot.rawBytesTruncated = activeTrace.rawBytesTruncated;

    const std::size_t first =
            (activeTrace.nextAttempt + kMaximumTraceAttempts - activeTrace.attemptCount) %
            kMaximumTraceAttempts;
    snapshot.attempts.reserve(activeTrace.attemptCount);
    for (std::size_t index = 0; index < activeTrace.attemptCount; ++index) {
        snapshot.attempts.push_back(
                activeTrace.attempts[(first + index) % kMaximumTraceAttempts]);
    }
    if (!snapshot.attempts.empty()) {
        const auto& last = snapshot.attempts.back();
        snapshot.failureOffset = last.offset;
        snapshot.requested = last.requested;
        snapshot.available = last.available;
        snapshot.overflowBeforeRead = last.overflow;
    }
    return snapshot;
}

} // namespace

std::optional<StreamView> inspectStream(const void* stream) {
    if (stream == nullptr)
        return std::nullopt;
    const auto* bytes = static_cast<const std::byte*>(stream);
    StreamView result;
    result.data = readUnaligned<const std::uint8_t*>(bytes + kStreamViewDataOffset);
    result.size = readUnaligned<std::size_t>(bytes + kStreamViewSizeOffset);
    result.readPointer = readUnaligned<std::size_t>(bytes + kStreamReadPointerOffset);
    result.overflowed = readUnaligned<std::uint8_t>(bytes + kStreamOverflowOffset) != 0;
    if (result.data == nullptr || result.size > (1024ULL * 1024ULL * 1024ULL) ||
        result.readPointer > result.size) {
        return std::nullopt;
    }
    return result;
}

void captureStreamReadAttempt(const void* stream, std::size_t requested, std::size_t rawCaptureLimit) {
    const auto view = inspectStream(stream);
    if (!view)
        return;

    if (activeTrace.stream != stream || activeTrace.data != view->data ||
        activeTrace.size != view->size || view->readPointer < activeTrace.previousOffset) {
        resetTrace(stream, *view, rawCaptureLimit);
    }

    const std::size_t available = view->size - view->readPointer;
    const bool overflow = view->overflowed || requested > available;
    auto& attempt = activeTrace.attempts[activeTrace.nextAttempt];
    attempt.offset = view->readPointer;
    attempt.requested = requested;
    attempt.available = available;
    attempt.overflow = overflow;
    writeCurrentClientSchemaPath(attempt.clientField);
    activeTrace.nextAttempt = (activeTrace.nextAttempt + 1) % kMaximumTraceAttempts;
    activeTrace.attemptCount = std::min(activeTrace.attemptCount + 1, kMaximumTraceAttempts);
    activeTrace.previousOffset = view->readPointer;
    if (!overflow)
        return;

    StreamFailure failure = snapshotTrace(true);
    failure.failureOffset = view->readPointer;
    failure.requested = requested;
    failure.available = available;
    failure.overflowBeforeRead = view->overflowed;

    std::lock_guard lock(failureMutex);
    lastFailure = TimedFailure{std::move(failure), std::chrono::steady_clock::now()};
}

void capturePacketEndCheck(const void* stream, std::size_t rawCaptureLimit) {
    const auto view = inspectStream(stream);
    if (!view || (!view->overflowed && view->readPointer == view->size))
        return;

    if (activeTrace.stream != stream || activeTrace.data != view->data ||
        activeTrace.size != view->size || view->readPointer < activeTrace.previousOffset) {
        resetTrace(stream, *view, rawCaptureLimit);
    }

    activeTrace.previousOffset = view->readPointer;
    activeTrace.lastObservedAt = std::chrono::steady_clock::now();
    StreamFailure failure = snapshotTrace(view->overflowed);
    failure.packetEndMismatch = !view->overflowed && view->readPointer < view->size;
    failure.failureOffset = view->readPointer;
    failure.requested = 0;
    failure.available = view->size - view->readPointer;
    failure.overflowBeforeRead = view->overflowed;

    std::lock_guard lock(failureMutex);
    lastFailure = TimedFailure{std::move(failure), std::chrono::steady_clock::now()};
}

std::optional<StreamFailure> recentStreamFailure(std::chrono::milliseconds maximumAge) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(failureMutex);
        if (lastFailure && now - lastFailure->capturedAt <= maximumAge) {
            auto result = std::move(lastFailure->failure);
            lastFailure.reset();
            return result;
        }
        if (lastFailure)
            lastFailure.reset();
    }

    // PacketViolationWarningPacket::getId runs on the decode/send thread. If
    // Bedrock reports the error after unwinding past the primitive that caused
    // it, preserve that thread's most recent bounded trace without pretending
    // an exact overflow was observed.
    if (activeTrace.attemptCount == 0 || now - activeTrace.lastObservedAt > maximumAge)
        return std::nullopt;
    return snapshotTrace(false);
}

void clearStreamProbe() {
    activeTrace = {};
    clearClientSchemaTrace();
    std::lock_guard lock(failureMutex);
    lastFailure.reset();
}

} // namespace dobby
