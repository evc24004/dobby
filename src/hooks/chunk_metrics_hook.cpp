#include "hooks/chunk_metrics_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "hooks/minecraft_image.hpp"
#include "hooks/outbound_packet_hook.hpp"
#include "metrics/chunk_metrics_layout.hpp"
#include "metrics/network_metrics.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace dobby {
namespace {

using DispatcherHandleFn =
        void (*)(const void* dispatcher, const void* source, void* callback,
                 const void* sharedPacket);
constexpr std::uint32_t kMaximumSubChunksPerPacket = 4096;
std::atomic_bool chunkRateReady{false};
std::atomic_bool outstandingReady{false};
std::atomic_bool firstLevelChunkLogged{false};
std::atomic_bool firstSubChunkRequestLogged{false};
std::atomic_bool firstSubChunkResponseLogged{false};
std::atomic_bool invalidRequestLayoutLogged{false};
std::atomic_bool invalidResponseLayoutLogged{false};
DispatcherHandleFn originalLevelChunkDispatcher = nullptr;
DispatcherHandleFn originalSubChunkDispatcher = nullptr;
std::uintptr_t expectedLevelChunkVtable{};
std::uintptr_t expectedSubChunkVtable{};
std::uintptr_t expectedSubChunkRequestVtable{};

template <class Value>
Value readObjectField(const void* object, std::ptrdiff_t offset) {
    Value value{};
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset,
                sizeof(value));
    return value;
}

const void* packetFromSharedPointer(const void* sharedPacket) {
    return sharedPacket == nullptr
            ? nullptr
            : readObjectField<const void*>(sharedPacket, 0);
}

bool objectHasVtable(const void* object, std::uintptr_t expectedVtable) {
    return object != nullptr && expectedVtable != 0 &&
            readObjectField<std::uintptr_t>(object, 0) == expectedVtable;
}

void recordSubChunkResponsePacket(const void* packet) {
    const auto* begin = readObjectField<const std::byte*>(
            packet, target::kSubChunkResponseVectorBeginOffset);
    const auto* end = readObjectField<const std::byte*>(
            packet, target::kSubChunkResponseVectorEndOffset);
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
    const auto count = boundedVectorElementCount(
            beginAddress, endAddress, target::kSubChunkPacketDataSize,
            kMaximumSubChunksPerPacket);
    if (!count) {
        if (!invalidResponseLayoutLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: ignored invalid SubChunk response vector");
        return;
    }
    recordSubChunkResponse(*count);
    if (!firstSubChunkResponseLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("chunk metrics: observed inbound SubChunk response (" +
                std::to_string(*count) + " subchunks)");
    }
}

void recordSubChunkRequestPacket(const void* packet) {
    const auto* begin = readObjectField<const std::byte*>(
            packet, target::kSubChunkRequestVectorBeginOffset);
    const auto* end = readObjectField<const std::byte*>(
            packet, target::kSubChunkRequestVectorEndOffset);
    const auto count = boundedVectorElementCount(
            reinterpret_cast<std::uintptr_t>(begin),
            reinterpret_cast<std::uintptr_t>(end),
            target::kSubChunkPositionSize, kMaximumSubChunksPerPacket);
    if (!count) {
        if (!invalidRequestLayoutLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: ignored invalid SubChunkRequest vector");
        return;
    }
    recordSubChunkRequest(*count);
    if (!firstSubChunkRequestLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("chunk metrics: observed outbound SubChunkRequest (" +
                std::to_string(*count) + " subchunks)");
    }
}

void levelChunkDispatcherDetour(
        const void* dispatcher, const void* source, void* callback,
        const void* sharedPacket) {
    const void* packet = packetFromSharedPointer(sharedPacket);
    if (objectHasVtable(packet, expectedLevelChunkVtable)) {
        recordLevelChunkDecode();
        if (!firstLevelChunkLogged.exchange(true, std::memory_order_acq_rel))
            logLine("chunk metrics: observed inbound LevelChunk dispatch");
    }
    if (originalLevelChunkDispatcher != nullptr)
        originalLevelChunkDispatcher(dispatcher, source, callback, sharedPacket);
}

void subChunkDispatcherDetour(
        const void* dispatcher, const void* source, void* callback,
        const void* sharedPacket) {
    const void* packet = packetFromSharedPointer(sharedPacket);
    if (objectHasVtable(packet, expectedSubChunkVtable))
        recordSubChunkResponsePacket(packet);
    if (originalSubChunkDispatcher != nullptr)
        originalSubChunkDispatcher(dispatcher, source, callback, sharedPacket);
}

void observeOutboundPacket(void* packet, std::int32_t) {
    if (outstandingReady.load(std::memory_order_acquire) &&
        objectHasVtable(packet, expectedSubChunkRequestVtable)) {
        recordSubChunkRequestPacket(packet);
    }
}

bool validateTarget(const MinecraftImage& image, std::uintptr_t functionOffset,
                    std::uintptr_t slotOffset, const auto& signature) {
    const auto function = image.base + functionOffset;
    const auto slot = image.base + slotOffset;
    if (!addressIsExecutable(image, function) || !addressIsInImage(image, slot) ||
        !matchesSignature(reinterpret_cast<const void*>(function), signature)) {
        return false;
    }
    std::uintptr_t current{};
    std::memcpy(&current, reinterpret_cast<const void*>(slot), sizeof(current));
    return current == function;
}

bool patchSlot(std::uintptr_t slotAddress, std::uintptr_t replacement) {
    auto value = replacement;
    auto* slot = reinterpret_cast<void*>(slotAddress);
    if (mcpelauncher_patch == nullptr ||
        mcpelauncher_patch(slot, &value, sizeof(value)) == nullptr) {
        return false;
    }
    std::uintptr_t installed{};
    std::memcpy(&installed, slot, sizeof(installed));
    return installed == replacement;
}

} // namespace
} // namespace dobby

namespace dobby {

void installChunkMetricsHooks() {
    if (chunkRateReady.load(std::memory_order_acquire) ||
        outstandingReady.load(std::memory_order_acquire)) {
        return;
    }
    if (target::kLevelChunkVtableOffset == 0 ||
        target::kSubChunkVtableOffset == 0 ||
        target::kSubChunkRequestVtableOffset == 0 ||
        target::kLevelChunkDispatcherOffset == 0 ||
        target::kSubChunkDispatcherOffset == 0) {
        logLine("chunk metrics: packet rate/pending paths disabled; 1.26.51.1 dispatch targets are unverified");
        return;
    }
    const auto image = findMinecraftImage();
    if (image.base == 0 || mcpelauncher_patch == nullptr) {
        logLine("ERROR: chunk metrics unavailable; patch API or image missing");
        return;
    }

    expectedLevelChunkVtable = image.base + target::kLevelChunkVtableOffset;
    expectedSubChunkVtable = image.base + target::kSubChunkVtableOffset;
    expectedSubChunkRequestVtable =
            image.base + target::kSubChunkRequestVtableOffset;
    const bool packetVtablesValid =
            addressIsInImage(image, expectedLevelChunkVtable) &&
            addressIsInImage(image, expectedSubChunkVtable) &&
            addressIsInImage(image, expectedSubChunkRequestVtable);

    if (packetVtablesValid &&
        validateTarget(image, target::kLevelChunkDispatcherOffset,
                       target::kLevelChunkDispatcherVtableSlotOffset,
                       target::kLevelChunkDispatcherSignature)) {
        originalLevelChunkDispatcher =
                reinterpret_cast<DispatcherHandleFn>(
                        image.base + target::kLevelChunkDispatcherOffset);
        const bool installed = patchSlot(
                image.base + target::kLevelChunkDispatcherVtableSlotOffset,
                reinterpret_cast<std::uintptr_t>(levelChunkDispatcherDetour));
        chunkRateReady.store(installed, std::memory_order_release);
        if (!installed)
            originalLevelChunkDispatcher = nullptr;
    }
    logLine(chunkRateReady.load(std::memory_order_acquire)
            ? "chunk metrics: LevelChunk runtime dispatcher hooked"
            : "ERROR: chunk rate unavailable; LevelChunk dispatcher mismatch");

    const bool responseValid = packetVtablesValid && validateTarget(
            image, target::kSubChunkDispatcherOffset,
            target::kSubChunkDispatcherVtableSlotOffset,
            target::kSubChunkDispatcherSignature);
    const bool requestValid = packetVtablesValid &&
            outboundPacketHookInstalled() &&
            registerOutboundPacketHandler(observeOutboundPacket);
    if (responseValid && requestValid) {
        originalSubChunkDispatcher = reinterpret_cast<DispatcherHandleFn>(
                image.base + target::kSubChunkDispatcherOffset);
        const bool responseInstalled = patchSlot(
                image.base + target::kSubChunkDispatcherVtableSlotOffset,
                reinterpret_cast<std::uintptr_t>(subChunkDispatcherDetour));
        const bool installed = responseInstalled;
        outstandingReady.store(installed, std::memory_order_release);
        if (!installed) {
            if (responseInstalled) {
                patchSlot(image.base + target::kSubChunkDispatcherVtableSlotOffset,
                          image.base + target::kSubChunkDispatcherOffset);
            }
        }
    }
    setOutstandingChunkMetricsAvailable(
            outstandingReady.load(std::memory_order_acquire));
    logLine(outstandingReady.load(std::memory_order_acquire)
            ? "chunk metrics: SubChunk runtime request lifecycle hooked"
            : "ERROR: pending chunks unavailable; runtime path mismatch");
}

bool chunkRateHookInstalled() {
    return chunkRateReady.load(std::memory_order_acquire);
}

bool outstandingChunkHooksInstalled() {
    return outstandingReady.load(std::memory_order_acquire);
}

} // namespace dobby

#else

namespace dobby {

void installChunkMetricsHooks() {}
bool chunkRateHookInstalled() { return false; }
bool outstandingChunkHooksInstalled() { return false; }

} // namespace dobby

#endif
