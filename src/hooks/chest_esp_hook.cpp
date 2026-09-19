#include "hooks/chest_esp_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "hooks/minecraft_image.hpp"
#include "hooks/network_metrics_hook.hpp"
#include "hooks/ore_esp_scanner.hpp"
#include "metrics/network_metrics.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"
#include "ui/chest_esp.hpp"
#include "ui/ore_esp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace dobby {
void captureConstructedChestPosition(const void* position);
}

extern "C" void* dobby_chest_constructor_continue = nullptr;
extern "C" void* dobby_chest_factory_continue = nullptr;

extern "C" void dobby_capture_chest_constructor(const void* position) {
    dobby::captureConstructedChestPosition(position);
}

extern "C" [[gnu::naked]] void dobby_chest_constructor_detour() {
    asm volatile(
            // ChestBlockActor(type, renderer, chestType, BlockPos const&)
            // receives the observed BlockPos pointer in x4. Preserve every
            // integer argument plus x30 while copying that position.
            "sub sp, sp, #96\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "str x8, [sp, #64]\n"
            "str x30, [sp, #80]\n"
            "mov x0, x4\n"
            "bl dobby_capture_chest_constructor\n"
            "ldp x0, x1, [sp, #0]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldr x8, [sp, #64]\n"
            "ldr x30, [sp, #80]\n"
            "add sp, sp, #96\n"
            // Replay the exact four overwritten constructor instructions.
            "stp x29, x30, [sp, #-32]!\n"
            "stp x20, x19, [sp, #16]\n"
            "mov x29, sp\n"
            "mov x20, x3\n"
            "adrp x16, dobby_chest_constructor_continue\n"
            "ldr x16, [x16, :lo12:dobby_chest_constructor_continue]\n"
            "br x16\n");
}

extern "C" [[gnu::naked]] void dobby_chest_factory_detour() {
    asm volatile(
            // The client factory inlines ChestBlockActor construction and
            // receives its BlockPos pointer in x1.
            "sub sp, sp, #96\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "str x8, [sp, #64]\n"
            "str x30, [sp, #80]\n"
            "mov x0, x1\n"
            "bl dobby_capture_chest_constructor\n"
            "ldp x0, x1, [sp, #0]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldr x8, [sp, #64]\n"
            "ldr x30, [sp, #80]\n"
            "add sp, sp, #96\n"
            // Replay the exact four overwritten factory instructions.
            "stp x29, x30, [sp, #-48]!\n"
            "stp x22, x21, [sp, #16]\n"
            "stp x20, x19, [sp, #32]\n"
            "mov x29, sp\n"
            "adrp x16, dobby_chest_factory_continue\n"
            "ldr x16, [x16, :lo12:dobby_chest_factory_continue]\n"
            "br x16\n");
}

namespace dobby {
namespace {

using ChunkLoadedFn = void (*)(void* coordinator, void* source, void* chunk);
using SubChunkLoadedFn = void (*)(
        void* coordinator, void* source, void* chunk,
        std::int16_t absoluteSubChunk, bool visibilityChanged);
using ChunkUnloadedFn = void (*)(void* coordinator, void* chunk);
using ChestDestructorFn = void (*)(void* chest);

struct ChunkIdentity {
    const void* level{};
    ChunkPosition position;

    bool operator==(const ChunkIdentity&) const = default;
};

constexpr std::size_t kMaximumTrackedChunks = 4'096;
constexpr std::size_t kMaximumPendingChests = 8'192;

std::atomic_bool installed{false};
std::atomic_bool metricsLifecycleInstalled{false};
std::atomic_bool firstChestLogged{false};
std::atomic_bool firstMetricsChunkLogged{false};
std::atomic_bool layoutFailureLogged{false};
std::atomic_bool capacityFailureLogged{false};
ChunkLoadedFn originalChunkLoaded = nullptr;
SubChunkLoadedFn originalSubChunkLoaded = nullptr;
ChunkUnloadedFn originalChunkUnloaded = nullptr;
ChestDestructorFn originalChestDestructor = nullptr;
ChestDestructorFn originalChestDeletingDestructor = nullptr;
MinecraftImage minecraftImage{};
std::mutex lifecycleMutex;
std::vector<ChunkIdentity> loadedChunks;
std::vector<BlockPosition> pendingChests;

template <class Value>
Value readObjectField(const void* object, std::ptrdiff_t offset) {
    Value value{};
    std::memcpy(
            &value, static_cast<const std::byte*>(object) + offset,
            sizeof(value));
    return value;
}

bool alignedPointer(const void* pointer) {
    return pointer != nullptr &&
            reinterpret_cast<std::uintptr_t>(pointer) % alignof(void*) == 0;
}

bool imageVtable(const void* object) {
    if (!alignedPointer(object))
        return false;
    const auto vtable = readObjectField<std::uintptr_t>(object, 0);
    return addressIsInImage(minecraftImage, vtable);
}

bool readChunkIdentity(const void* chunk, ChunkIdentity& output) {
    if (!alignedPointer(chunk))
        return false;
    const void* level = readObjectField<const void*>(
            chunk, target::kLevelChunkLevelOffset);
    const ChunkPosition position = readObjectField<ChunkPosition>(
            chunk, target::kLevelChunkPositionOffset);
    if (!imageVtable(level) || !validChunkPosition(position))
        return false;
    output = {level, position};
    return true;
}

bool readBlockPosition(const void* source, BlockPosition& output) {
    if (source == nullptr ||
        reinterpret_cast<std::uintptr_t>(source) % alignof(std::int32_t) != 0) {
        return false;
    }
    std::memcpy(&output, source, sizeof(output));
    return validChestBlockPosition(output);
}

void logLayoutFailure() {
    if (!layoutFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("ERROR: chest ESP ignored an invalid client lifecycle object");
    }
}

void logCapacityFailure() {
    if (!capacityFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("chest ESP: client-known chest registry reached its safety limit");
    }
}

void registerChest(const void* level, BlockPosition position) {
    const auto result = addClientKnownChest(level, position);
    if (result == ChunkChestUpdateResult::invalidInput) {
        logLayoutFailure();
        return;
    }
    if (result != ChunkChestUpdateResult::accepted) {
        logCapacityFailure();
        return;
    }
    if (!firstChestLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[160]{};
        std::snprintf(
                message, sizeof(message),
                "chest ESP: client decoded ChestBlockActor at (%d,%d,%d)",
                position.x, position.y, position.z);
        logLine(message);
    }
}

void queueOrRegisterChest(BlockPosition position) {
    const ChunkPosition chunk = chunkPositionForBlock(position);
    std::vector<const void*> destinationLevels;
    bool capacityReached = false;
    {
        std::lock_guard lock(lifecycleMutex);
        for (const auto& loaded : loadedChunks) {
            if (loaded.position == chunk)
                destinationLevels.push_back(loaded.level);
        }
        if (destinationLevels.empty() &&
            std::find(pendingChests.begin(), pendingChests.end(), position) ==
                    pendingChests.end()) {
            if (pendingChests.size() >= kMaximumPendingChests) {
                capacityReached = true;
            } else {
                pendingChests.push_back(position);
            }
        }
    }
    if (capacityReached)
        logCapacityFailure();
    for (const void* level : destinationLevels)
        registerChest(level, position);
}

void unregisterChest(BlockPosition position) {
    const ChunkPosition chunk = chunkPositionForBlock(position);
    std::vector<const void*> destinationLevels;
    {
        std::lock_guard lock(lifecycleMutex);
        pendingChests.erase(
                std::remove(pendingChests.begin(), pendingChests.end(), position),
                pendingChests.end());
        for (const auto& loaded : loadedChunks) {
            if (loaded.position == chunk)
                destinationLevels.push_back(loaded.level);
        }
    }
    for (const void* level : destinationLevels)
        removeClientKnownChest(level, position);
}

void trackLoadedChunk(ChunkIdentity identity) {
    std::vector<BlockPosition> ready;
    bool capacityReached = false;
    {
        std::lock_guard lock(lifecycleMutex);
        if (std::find(loadedChunks.begin(), loadedChunks.end(), identity) ==
            loadedChunks.end()) {
            if (loadedChunks.size() >= kMaximumTrackedChunks) {
                capacityReached = true;
            } else {
                loadedChunks.push_back(identity);
            }
        }
        if (!capacityReached) {
            for (const auto& position : pendingChests) {
                if (chunkPositionForBlock(position) == identity.position)
                    ready.push_back(position);
            }
            pendingChests.erase(
                    std::remove_if(
                            pendingChests.begin(), pendingChests.end(),
                            [&](BlockPosition position) {
                                return chunkPositionForBlock(position) ==
                                        identity.position;
                            }),
                    pendingChests.end());
        }
    }
    if (capacityReached) {
        logCapacityFailure();
        return;
    }
    for (const auto position : ready)
        registerChest(identity.level, position);
}

void untrackLoadedChunk(ChunkIdentity identity) {
    {
        std::lock_guard lock(lifecycleMutex);
        loadedChunks.erase(
                std::remove(loadedChunks.begin(), loadedChunks.end(), identity),
                loadedChunks.end());
        pendingChests.erase(
                std::remove_if(
                        pendingChests.begin(), pendingChests.end(),
                        [&](BlockPosition position) {
                            return chunkPositionForBlock(position) ==
                                    identity.position;
                        }),
                pendingChests.end());
    }
    removeClientChunkChests(identity.level, identity.position);
}

void unregisterChestObject(const void* chest) {
    if (!alignedPointer(chest)) {
        logLayoutFailure();
        return;
    }
    BlockPosition position{};
    const auto* source = static_cast<const std::byte*>(chest) +
            target::kBlockActorPositionOffset;
    if (!readBlockPosition(source, position)) {
        logLayoutFailure();
        return;
    }
    unregisterChest(position);
}

void chestDestructorDetour(void* chest) {
    unregisterChestObject(chest);
    if (originalChestDestructor != nullptr)
        originalChestDestructor(chest);
}

void chestDeletingDestructorDetour(void* chest) {
    unregisterChestObject(chest);
    if (originalChestDeletingDestructor != nullptr)
        originalChestDeletingDestructor(chest);
}

void chunkLoadedDetour(void* coordinator, void* source, void* chunk) {
    if (originalChunkLoaded != nullptr)
        originalChunkLoaded(coordinator, source, chunk);
    const bool metricsEnabled = runtimeState().networkMetricsOverlay();
    const bool chestEnabled = runtimeState().chestEsp();
    const bool oreEnabled = runtimeState().oreEsp();
    ChunkIdentity identity{};
    if (readChunkIdentity(chunk, identity)) {
        // Keeping an opaque chunk identity is cheap and lets Ore ESP scan
        // chunks that were already loaded when its menu toggle is enabled.
        trackClientChunkForOreEsp(
                chunk, identity.level, identity.position);
        if (!metricsEnabled && !chestEnabled && !oreEnabled)
            return;
        if (metricsEnabled)
            observeClientLevelForMetrics(identity.level);
        if (metricsEnabled) {
            recordClientChunkLoaded(
                    identity.level, identity.position.x, identity.position.z);
            if (!firstMetricsChunkLogged.exchange(
                        true, std::memory_order_acq_rel)) {
                logLine("chunk metrics: client chunk lifecycle observed");
            }
        }
        if (chestEnabled)
            trackLoadedChunk(identity);
    } else if (metricsEnabled || chestEnabled || oreEnabled) {
        logLayoutFailure();
    }
}

void subChunkLoadedDetour(
        void* coordinator, void* source, void* chunk,
        std::int16_t absoluteSubChunk, bool visibilityChanged) {
    if (originalSubChunkLoaded != nullptr) {
        originalSubChunkLoaded(
                coordinator, source, chunk, absoluteSubChunk,
                visibilityChanged);
    }
    const bool metricsEnabled = runtimeState().networkMetricsOverlay();
    const bool chestEnabled = runtimeState().chestEsp();
    const bool oreEnabled = runtimeState().oreEsp();
    ChunkIdentity identity{};
    if (readChunkIdentity(chunk, identity)) {
        // Any client subchunk replacement dirties the full chunk's palette
        // snapshot. The render-side scheduler refreshes it nearest-first.
        dirtyClientChunkForOreEsp(
                chunk, identity.level, identity.position);
        if (!metricsEnabled && !chestEnabled && !oreEnabled)
            return;
        if (metricsEnabled)
            observeClientLevelForMetrics(identity.level);
        if (metricsEnabled) {
            recordClientChunkLoaded(
                    identity.level, identity.position.x, identity.position.z);
        }
        if (chestEnabled)
            trackLoadedChunk(identity);
        static_cast<void>(absoluteSubChunk);
    } else if (metricsEnabled || chestEnabled || oreEnabled) {
        logLayoutFailure();
    }
}

void chunkUnloadedDetour(void* coordinator, void* chunk) {
    const bool metricsEnabled = runtimeState().networkMetricsOverlay();
    const bool espEnabled = runtimeState().anyEspEnabled();
    ChunkIdentity identity{};
    bool valid = readChunkIdentity(chunk, identity);
    if (valid) {
        // Remove the target before Bedrock's callback may release it. The
        // scheduler holds the same lifetime gate throughout each scan.
        untrackClientChunkForOreEsp(
                chunk, identity.level, identity.position);
    } else if (const auto tracked =
                       untrackClientChunkForOreEsp(chunk)) {
        // A damaged or transiently unreadable chunk object can still be
        // removed exactly by its opaque pointer before Bedrock releases it.
        identity = {tracked->level, tracked->position};
        valid = true;
    }
    if (originalChunkUnloaded != nullptr)
        originalChunkUnloaded(coordinator, chunk);
    if (!metricsEnabled && !espEnabled)
        return;
    if (valid) {
        if (metricsEnabled) {
            recordClientChunkUnloaded(
                    identity.level, identity.position.x, identity.position.z);
        }
        if (espEnabled)
            untrackLoadedChunk(identity);
    } else {
        logLayoutFailure();
    }
}

template <std::size_t SignatureSize>
bool validateHookTarget(
        const MinecraftImage& image, std::uintptr_t functionOffset,
        std::uintptr_t slotOffset,
        const std::array<std::uint8_t, SignatureSize>& signature) {
    const auto functionAddress = image.base + functionOffset;
    const auto slotAddress = image.base + slotOffset;
    if (!addressIsExecutable(image, functionAddress) ||
        !addressIsInImage(image, slotAddress) ||
        slotAddress > image.loadEnd - sizeof(std::uintptr_t) ||
        !matchesSignature(
                reinterpret_cast<const void*>(functionAddress), signature)) {
        return false;
    }
    const auto current = readObjectField<std::uintptr_t>(
            reinterpret_cast<const void*>(slotAddress), 0);
    return current == functionAddress;
}

bool patchAddress(std::uintptr_t address, const void* data, std::size_t size) {
    if (mcpelauncher_patch == nullptr)
        return false;
    auto* targetAddress = reinterpret_cast<void*>(address);
    if (mcpelauncher_patch(
                targetAddress, const_cast<void*>(data), size) == nullptr) {
        return false;
    }
    return std::memcmp(targetAddress, data, size) == 0;
}

bool patchSlot(std::uintptr_t slotAddress, std::uintptr_t replacement) {
    return patchAddress(slotAddress, &replacement, sizeof(replacement));
}

bool installMetricsChunkLifecycleHooks(const MinecraftImage& image) {
    if (!validateHookTarget(
                image, target::kChunkCoordinatorOnChunkLoadedOffset,
                target::kChunkCoordinatorOnChunkLoadedSlotOffset,
                target::kChunkCoordinatorOnChunkLoadedSignature) ||
        !validateHookTarget(
                image, target::kChunkCoordinatorOnSubChunkLoadedOffset,
                target::kChunkCoordinatorOnSubChunkLoadedSlotOffset,
                target::kChunkCoordinatorOnSubChunkLoadedSignature) ||
        !validateHookTarget(
                image, target::kChunkCoordinatorOnChunkUnloadedOffset,
                target::kChunkCoordinatorOnChunkUnloadedSlotOffset,
                target::kChunkCoordinatorOnChunkUnloadedSignature)) {
        return false;
    }

    originalChunkLoaded = reinterpret_cast<ChunkLoadedFn>(
            image.base + target::kChunkCoordinatorOnChunkLoadedOffset);
    originalSubChunkLoaded = reinterpret_cast<SubChunkLoadedFn>(
            image.base + target::kChunkCoordinatorOnSubChunkLoadedOffset);
    originalChunkUnloaded = reinterpret_cast<ChunkUnloadedFn>(
            image.base + target::kChunkCoordinatorOnChunkUnloadedOffset);
    const bool loaded = patchSlot(
            image.base + target::kChunkCoordinatorOnChunkLoadedSlotOffset,
            reinterpret_cast<std::uintptr_t>(chunkLoadedDetour));
    const bool subChunk = loaded && patchSlot(
            image.base + target::kChunkCoordinatorOnSubChunkLoadedSlotOffset,
            reinterpret_cast<std::uintptr_t>(subChunkLoadedDetour));
    const bool unloaded = subChunk && patchSlot(
            image.base + target::kChunkCoordinatorOnChunkUnloadedSlotOffset,
            reinterpret_cast<std::uintptr_t>(chunkUnloadedDetour));
    if (unloaded)
        return true;

    if (loaded) {
        static_cast<void>(patchSlot(
                image.base + target::kChunkCoordinatorOnChunkLoadedSlotOffset,
                image.base + target::kChunkCoordinatorOnChunkLoadedOffset));
    }
    if (subChunk) {
        static_cast<void>(patchSlot(
                image.base +
                        target::kChunkCoordinatorOnSubChunkLoadedSlotOffset,
                image.base +
                        target::kChunkCoordinatorOnSubChunkLoadedOffset));
    }
    originalChunkLoaded = nullptr;
    originalSubChunkLoaded = nullptr;
    originalChunkUnloaded = nullptr;
    return false;
}

bool validateTargets(const MinecraftImage& image) {
    const auto constructor =
            image.base + target::kChestBlockActorConstructorOffset;
    const auto destructor =
            image.base + target::kChestBlockActorDestructorOffset;
    const auto factory = image.base + target::kChestBlockActorFactoryOffset;
    const auto deletingDestructor =
            image.base + target::kChestBlockActorDeletingDestructorOffset;
    const auto vtable = image.base + target::kChestBlockActorVtableOffset;
    const auto positionProbe =
            image.base + target::kBlockActorPositionLayoutProbeOffset;
    if (mcpelauncher_patch == nullptr ||
        !addressIsExecutable(image, constructor) ||
        !addressIsExecutable(image, factory) ||
        !addressIsExecutable(image, destructor) ||
        !addressIsExecutable(image, deletingDestructor) ||
        !addressIsExecutable(image, positionProbe) ||
        !matchesSignature(
                reinterpret_cast<const void*>(constructor),
                target::kChestBlockActorConstructorSignature) ||
        !matchesSignature(
                reinterpret_cast<const void*>(factory),
                target::kChestBlockActorFactorySignature) ||
        !matchesSignature(
                reinterpret_cast<const void*>(destructor),
                target::kChestBlockActorDestructorSignature) ||
        !matchesSignature(
                reinterpret_cast<const void*>(deletingDestructor),
                target::kChestBlockActorDeletingDestructorSignature) ||
        !matchesSignature(
                reinterpret_cast<const void*>(positionProbe),
                target::kBlockActorPositionLayoutProbeSignature) ||
        !matchesSignature(
                reinterpret_cast<const void*>(
                        image.base + target::kLevelChunkGetPositionOffset),
                target::kLevelChunkGetPositionSignature) ||
        !matchesSignature(
                reinterpret_cast<const void*>(
                        image.base + target::kLevelChunkGetLevelOffset),
                target::kLevelChunkGetLevelSignature) ||
        !addressIsInImage(image, vtable) ||
        vtable > image.loadEnd - 2 * sizeof(std::uintptr_t)) {
        return false;
    }
    const auto* slots = reinterpret_cast<const std::uintptr_t*>(vtable);
    if (slots[target::kChestBlockActorDestructorVtableSlot] != destructor ||
        slots[target::kChestBlockActorDeletingDestructorVtableSlot] !=
                deletingDestructor) {
        return false;
    }
    return validateHookTarget(
                   image, target::kChunkCoordinatorOnChunkLoadedOffset,
                   target::kChunkCoordinatorOnChunkLoadedSlotOffset,
                   target::kChunkCoordinatorOnChunkLoadedSignature) &&
            validateHookTarget(
                   image, target::kChunkCoordinatorOnSubChunkLoadedOffset,
                   target::kChunkCoordinatorOnSubChunkLoadedSlotOffset,
                   target::kChunkCoordinatorOnSubChunkLoadedSignature) &&
            validateHookTarget(
                   image, target::kChunkCoordinatorOnChunkUnloadedOffset,
                   target::kChunkCoordinatorOnChunkUnloadedSlotOffset,
                   target::kChunkCoordinatorOnChunkUnloadedSignature);
}

void restoreHooks(
        const MinecraftImage& image, bool constructorPatched,
        bool factoryPatched) {
    const auto vtable = image.base + target::kChestBlockActorVtableOffset;
    static_cast<void>(patchSlot(
            vtable + target::kChestBlockActorDestructorVtableSlot *
                            sizeof(std::uintptr_t),
            image.base + target::kChestBlockActorDestructorOffset));
    static_cast<void>(patchSlot(
            vtable + target::kChestBlockActorDeletingDestructorVtableSlot *
                            sizeof(std::uintptr_t),
            image.base + target::kChestBlockActorDeletingDestructorOffset));
    static_cast<void>(patchSlot(
            image.base + target::kChunkCoordinatorOnChunkLoadedSlotOffset,
            image.base + target::kChunkCoordinatorOnChunkLoadedOffset));
    static_cast<void>(patchSlot(
            image.base + target::kChunkCoordinatorOnSubChunkLoadedSlotOffset,
            image.base + target::kChunkCoordinatorOnSubChunkLoadedOffset));
    static_cast<void>(patchSlot(
            image.base + target::kChunkCoordinatorOnChunkUnloadedSlotOffset,
            image.base + target::kChunkCoordinatorOnChunkUnloadedOffset));
    if (constructorPatched) {
        static_cast<void>(patchAddress(
                image.base + target::kChestBlockActorConstructorOffset,
                target::kChestBlockActorConstructorSignature.data(),
                target::kChestBlockActorConstructorSignature.size()));
    }
    if (factoryPatched) {
        static_cast<void>(patchAddress(
                image.base + target::kChestBlockActorFactoryOffset,
                target::kChestBlockActorFactorySignature.data(),
                target::kChestBlockActorFactorySignature.size()));
    }
    dobby_chest_constructor_continue = nullptr;
    dobby_chest_factory_continue = nullptr;
}

bool installLifecycleHooks(const MinecraftImage& image) {
    if (!validateTargets(image))
        return false;

    const auto vtable = image.base + target::kChestBlockActorVtableOffset;
    const auto constructor =
            image.base + target::kChestBlockActorConstructorOffset;
    originalChestDestructor = reinterpret_cast<ChestDestructorFn>(
            image.base + target::kChestBlockActorDestructorOffset);
    originalChestDeletingDestructor = reinterpret_cast<ChestDestructorFn>(
            image.base + target::kChestBlockActorDeletingDestructorOffset);
    originalChunkLoaded = reinterpret_cast<ChunkLoadedFn>(
            image.base + target::kChunkCoordinatorOnChunkLoadedOffset);
    originalSubChunkLoaded = reinterpret_cast<SubChunkLoadedFn>(
            image.base + target::kChunkCoordinatorOnSubChunkLoadedOffset);
    originalChunkUnloaded = reinterpret_cast<ChunkUnloadedFn>(
            image.base + target::kChunkCoordinatorOnChunkUnloadedOffset);

    if (!patchSlot(
                vtable + target::kChestBlockActorDestructorVtableSlot *
                                sizeof(std::uintptr_t),
                reinterpret_cast<std::uintptr_t>(chestDestructorDetour)) ||
        !patchSlot(
                vtable +
                        target::kChestBlockActorDeletingDestructorVtableSlot *
                                sizeof(std::uintptr_t),
                reinterpret_cast<std::uintptr_t>(
                        chestDeletingDestructorDetour))) {
        restoreHooks(image, false, false);
        return false;
    }

    std::array<std::uint8_t, 16> constructorPatch{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(
            dobby_chest_constructor_detour);
    std::memcpy(constructorPatch.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(
            constructorPatch.data() + 4, &branchTarget,
            sizeof(branchTarget));
    std::memcpy(
            constructorPatch.data() + 8, &detour, sizeof(detour));
    dobby_chest_constructor_continue = reinterpret_cast<void*>(
            constructor + constructorPatch.size());
    if (!patchAddress(
                constructor, constructorPatch.data(),
                constructorPatch.size())) {
        restoreHooks(image, true, false);
        return false;
    }

    const auto factory = image.base + target::kChestBlockActorFactoryOffset;
    std::array<std::uint8_t, 16> factoryPatch{};
    const auto factoryDetour = reinterpret_cast<std::uintptr_t>(
            dobby_chest_factory_detour);
    std::memcpy(factoryPatch.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(
            factoryPatch.data() + 4, &branchTarget,
            sizeof(branchTarget));
    std::memcpy(
            factoryPatch.data() + 8, &factoryDetour,
            sizeof(factoryDetour));
    dobby_chest_factory_continue = reinterpret_cast<void*>(
            factory + factoryPatch.size());
    if (!patchAddress(factory, factoryPatch.data(), factoryPatch.size())) {
        restoreHooks(image, true, true);
        return false;
    }

    if (!patchSlot(
                image.base + target::kChunkCoordinatorOnChunkLoadedSlotOffset,
                reinterpret_cast<std::uintptr_t>(chunkLoadedDetour)) ||
        !patchSlot(
                image.base +
                        target::kChunkCoordinatorOnSubChunkLoadedSlotOffset,
                reinterpret_cast<std::uintptr_t>(subChunkLoadedDetour)) ||
        !patchSlot(
                image.base +
                        target::kChunkCoordinatorOnChunkUnloadedSlotOffset,
                reinterpret_cast<std::uintptr_t>(chunkUnloadedDetour))) {
        restoreHooks(image, true, true);
        return false;
    }
    return true;
}

} // namespace

void captureConstructedChestPosition(const void* position) {
    if (!runtimeState().chestEsp())
        return;
    BlockPosition observed{};
    if (!readBlockPosition(position, observed)) {
        logLayoutFailure();
        return;
    }
    queueOrRegisterChest(observed);
}

void installChestEspHook() {
    if (installed.load(std::memory_order_acquire))
        return;
    if (target::kBlockActorPositionLayoutProbeOffset == 0 ||
        target::kLevelChunkGetPositionOffset == 0) {
        const MinecraftImage image = findMinecraftImage();
        minecraftImage = image;
        const bool metricsReady = image.base != 0 &&
                installMetricsChunkLifecycleHooks(image);
        metricsLifecycleInstalled.store(
                metricsReady, std::memory_order_release);
        logLine(metricsReady
                ? "chunk metrics: client loaded-chunk lifecycle hooked"
                : "ERROR: loaded chunk metrics unavailable; lifecycle target mismatch");
        runtimeState().setChestEspAvailable(false);
        runtimeState().setOreEspAvailable(false);
        logLine("chest ESP: disabled; 1.26.51.1 BlockActor position probe is unverified");
        return;
    }
    const MinecraftImage image = findMinecraftImage();
    minecraftImage = image;
    const bool oreScannerReady = initializeOreEspScanner(image);
    const bool ready = image.base != 0 && installLifecycleHooks(image);
    if (!ready) {
        minecraftImage = {};
        runtimeState().setOreEspAvailable(false);
    } else {
        runtimeState().setOreEspAvailable(oreScannerReady);
    }
    installed.store(ready, std::memory_order_release);
    runtimeState().setChestEspAvailable(ready);
    logLine(ready
            ? "chest ESP: decoded ChestBlockActor lifecycle capture ready"
            : "ERROR: chest ESP unavailable; client lifecycle target mismatch");
}

bool chestEspHookInstalled() {
    return installed.load(std::memory_order_acquire);
}

} // namespace dobby

#endif
