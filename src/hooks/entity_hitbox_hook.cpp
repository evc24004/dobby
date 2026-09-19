#include "hooks/entity_hitbox_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "hooks/minecraft_image.hpp"
#include "hooks/network_metrics_hook.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"
#include "ui/entity_hitbox_overlay.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

extern "C" void* dobby_actor_render_continue = nullptr;

namespace dobby {
namespace {

using ActorGetAabbFn = const EntityAabb* (*)(const void* actor);
using LevelGetRuntimeActorListFn = std::vector<const void*> (*)(const void* level);
struct OpaquePlayer;
using PlayerVisitor = std::function<bool(OpaquePlayer&)>;
using LevelForEachPlayerFn = void (*)(void* level, PlayerVisitor visitor);
using LevelGetPrimaryLocalPlayerFn = const void* (*)(const void* level);

std::atomic_bool hookInstalled{false};
std::atomic_bool batchCaptureLogged{false};
std::atomic_bool invalidBoundsLogged{false};
std::atomic_bool capacityLogged{false};
std::atomic_bool registryCaptureLogged{false};
std::atomic_bool playerCaptureLogged{false};
std::atomic_bool registryLayoutRejectedLogged{false};
std::atomic_bool registryCapacityLogged{false};
std::atomic_bool playerCapacityLogged{false};
ActorGetAabbFn actorGetAabb = nullptr;
LevelGetRuntimeActorListFn levelGetRuntimeActorList = nullptr;
LevelForEachPlayerFn levelForEachPlayer = nullptr;
LevelGetPrimaryLocalPlayerFn levelGetPrimaryLocalPlayer = nullptr;
std::uintptr_t expectedClientLevelVtable{};
MinecraftImage minecraftImage{};
std::atomic_uint64_t lastBatchCaptureFrame{
        std::numeric_limits<std::uint64_t>::max()};

template <class Value>
Value readObjectField(const void* object, std::ptrdiff_t offset) {
    Value value{};
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
    return value;
}

bool validClientLevel(const void* level) {
    if (level == nullptr || expectedClientLevelVtable == 0 ||
        levelGetRuntimeActorList == nullptr || levelForEachPlayer == nullptr ||
        levelGetPrimaryLocalPlayer == nullptr) {
        return false;
    }
    const auto levelAddress = reinterpret_cast<std::uintptr_t>(level);
    if (levelAddress % alignof(void*) != 0)
        return false;
    const auto vtable = readObjectField<std::uintptr_t>(level, 0);
    constexpr std::size_t largestSlot =
            target::kLevelGetRuntimeActorListVtableSlot;
    const auto vtableEnd = vtable + (largestSlot + 1) * sizeof(std::uintptr_t);
    if (vtable != expectedClientLevelVtable || vtableEnd < vtable ||
        !addressIsInImage(minecraftImage, vtable) ||
        vtableEnd > minecraftImage.loadEnd) {
        if (!registryLayoutRejectedLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: entity registry unavailable; ClientLevel layout mismatch");
        return false;
    }

    const auto* slots = reinterpret_cast<const std::uintptr_t*>(vtable);
    const auto actorListTarget = reinterpret_cast<std::uintptr_t>(
            levelGetRuntimeActorList);
    const auto playerListTarget = reinterpret_cast<std::uintptr_t>(
            levelForEachPlayer);
    const auto primaryPlayerTarget = reinterpret_cast<std::uintptr_t>(
            levelGetPrimaryLocalPlayer);
    if (slots[target::kLevelGetRuntimeActorListVtableSlot] != actorListTarget ||
        slots[target::kLevelForEachPlayerVtableSlot] != playerListTarget ||
        slots[target::kLevelGetPrimaryLocalPlayerVtableSlot] !=
                primaryPlayerTarget) {
        if (!registryLayoutRejectedLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: entity registry unavailable; ClientLevel vtable mismatch");
        return false;
    }
    return true;
}

std::span<const EntityHitboxObservation> captureRuntimeEntities(
        const void* level, std::size_t& actorCount, std::size_t& playerCount) {
    thread_local std::vector<EntityHitboxObservation> observations;
    observations.clear();
    observations.reserve(512);
    actorCount = 0;
    playerCount = 0;
    if (!validClientLevel(level) || actorGetAabb == nullptr)
        return observations;

    const void* localPlayer = levelGetPrimaryLocalPlayer(level);
    const auto actors = levelGetRuntimeActorList(level);
    const std::size_t count = std::min(actors.size(), std::size_t{512});
    for (std::size_t index = 0; index < count; ++index) {
        const void* actor = actors[index];
        if (actor == nullptr || actor == localPlayer)
            continue;
        const auto* bounds = actorGetAabb(actor);
        if (bounds != nullptr) {
            observations.push_back({actor, *bounds});
            ++actorCount;
        }
    }
    if (actors.size() > count &&
        !registryCapacityLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("entity registry: capture capped at 512 actors");
    }

    constexpr std::size_t maximumPlayers = 256;
    std::size_t visited = 0;
    PlayerVisitor visitor = [&](OpaquePlayer& player) {
        if (visited >= maximumPlayers || observations.size() >= 512)
            return false;
        ++visited;
        const void* actor = static_cast<const void*>(&player);
        if (actor == localPlayer)
            return true;
        const bool alreadyCaptured = std::any_of(
                observations.begin(), observations.end(),
                [actor](const EntityHitboxObservation& observation) {
                    return observation.identity == actor;
                });
        if (!alreadyCaptured) {
            const auto* bounds = actorGetAabb(actor);
            if (bounds != nullptr) {
                observations.push_back({actor, *bounds});
                ++playerCount;
            }
        }
        return visited < maximumPlayers && observations.size() < 512;
    };
    levelForEachPlayer(const_cast<void*>(level), std::move(visitor));
    if (visited == maximumPlayers &&
        !playerCapacityLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("player registry: capture capped at 256 players");
    }
    if (actorCount != 0 &&
        !registryCaptureLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
                      "entity registry: batched %zu of %zu active actors",
                      actorCount, actors.size());
        logLine(message);
    }
    if (visited != 0 &&
        !playerCaptureLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
                      "player registry: added %zu of %zu active players",
                      playerCount, visited);
        logLine(message);
    }
    return observations;
}

} // namespace

extern "C" void dobby_capture_entity_hitbox(
        const void* renderContext, const void* actor) {
    if (renderContext == nullptr || actor == nullptr) {
        return;
    }

    const void* level = readObjectField<const void*>(
            actor, target::kActorLevelOffset);
    if (runtimeState().networkMetricsOverlay())
        observeClientLevelForMetrics(level);
    if (!runtimeState().anyEspEnabled())
        return;
    const std::uint64_t presentation = entityHitboxPresentationFrame();
    if (lastBatchCaptureFrame.load(std::memory_order_acquire) == presentation)
        return;

    if (lastBatchCaptureFrame.exchange(
                presentation, std::memory_order_acq_rel) == presentation) {
        return;
    }
    std::size_t actorCount = 0;
    std::size_t playerCount = 0;
    const auto observations = runtimeState().entityHitboxes()
            ? captureRuntimeEntities(level, actorCount, playerCount)
            : std::span<const EntityHitboxObservation>{};
    const HitboxFrameSubmission result = submitEntityHitboxFrame(
            level, observations);
    if (result.invalid != 0 &&
        !invalidBoundsLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("ERROR: entity overlay rejected invalid client bounds");
    }
    if (result.capacityRejected != 0 &&
        !capacityLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("entity overlay: frame capture capped at 512 actors");
    }
    if (!observations.empty() &&
        !batchCaptureLogged.exchange(true, std::memory_order_acq_rel)) {
        const EntityAabb& bounds = observations.front().bounds;
        char message[512]{};
        std::snprintf(
                message, sizeof(message),
                "entity batch sample: accepted=%zu actors=%zu players=%zu "
                "bounds=(%.3f,%.3f,%.3f)->"
                "(%.3f,%.3f,%.3f)",
                result.accepted, actorCount, playerCount,
                bounds.minimum.x, bounds.minimum.y, bounds.minimum.z,
                bounds.maximum.x, bounds.maximum.y, bounds.maximum.z);
        logLine(message);
    }
}

} // namespace dobby

extern "C" [[gnu::naked]] void dobby_actor_render_detour() {
    asm volatile(
            // The canonical ESP mask is a lock-free byte. When every ESP
            // module is disabled, skip the capture call and its register-save
            // prologue entirely. x16 is ABI-defined intra-procedure scratch.
            "adrp x16, dobby_esp_feature_mask\n"
            "add x16, x16, :lo12:dobby_esp_feature_mask\n"
            "ldarb w16, [x16]\n"
            "cbz w16, 1f\n"
            // Capture the context and actor before Bedrock's render prologue.
            // x0-x8 and x30 are restored because the capture is transparent.
            "sub sp, sp, #96\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "str x8, [sp, #64]\n"
            "str x30, [sp, #80]\n"
            "mov x0, x1\n"
            "mov x1, x2\n"
            "bl dobby_capture_entity_hitbox\n"
            "ldp x0, x1, [sp, #0]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldr x8, [sp, #64]\n"
            "ldr x30, [sp, #80]\n"
            "add sp, sp, #96\n"
            "1:\n"
            // Replay the four validated ActorRenderDispatcher instructions.
            "sub sp, sp, #144\n"
            "stp d11, d10, [sp, #48]\n"
            "stp d9, d8, [sp, #64]\n"
            "stp x29, x30, [sp, #80]\n"
            "adrp x16, dobby_actor_render_continue\n"
            "ldr x16, [x16, :lo12:dobby_actor_render_continue]\n"
            "br x16\n");
}

namespace dobby {

void installEntityHitboxHook() {
    if (hookInstalled.load(std::memory_order_acquire))
        return;

    const auto image = findMinecraftImage();
    const auto renderAddress = image.base + target::kActorRenderOffset;
    const auto getAabbAddress = image.base + target::kActorGetAabbOffset;
    const auto getLevelAddress = image.base + target::kActorGetLevelOffset;
    const auto runtimeActorListAddress =
            image.base + target::kLevelGetRuntimeActorListOffset;
    const auto forEachPlayerAddress =
            image.base + target::kLevelForEachPlayerOffset;
    const auto primaryLocalPlayerAddress =
            image.base + target::kLevelGetPrimaryLocalPlayerOffset;
    const bool valid = image.base != 0 &&
            addressIsExecutable(image, renderAddress) &&
            addressIsExecutable(image, getAabbAddress) &&
            addressIsExecutable(image, getLevelAddress) &&
            addressIsExecutable(image, runtimeActorListAddress) &&
            addressIsExecutable(image, forEachPlayerAddress) &&
            addressIsExecutable(image, primaryLocalPlayerAddress) &&
            matchesSignature(reinterpret_cast<const void*>(renderAddress),
                             target::kActorRenderSignature) &&
            matchesSignature(reinterpret_cast<const void*>(getAabbAddress),
                             target::kActorGetAabbSignature) &&
            matchesSignature(reinterpret_cast<const void*>(getLevelAddress),
                             target::kActorGetLevelSignature) &&
            matchesSignature(reinterpret_cast<const void*>(runtimeActorListAddress),
                             target::kLevelGetRuntimeActorListSignature) &&
            matchesSignature(reinterpret_cast<const void*>(forEachPlayerAddress),
                             target::kLevelForEachPlayerSignature) &&
            matchesSignature(reinterpret_cast<const void*>(primaryLocalPlayerAddress),
                             target::kLevelGetPrimaryLocalPlayerSignature);
    if (!valid) {
        runtimeState().setEntityHitboxesAvailable(false);
        logLine("ERROR: entity overlay unavailable; Bedrock render layout mismatch");
        return;
    }
    if (mcpelauncher_patch == nullptr) {
        runtimeState().setEntityHitboxesAvailable(false);
        logLine("ERROR: entity overlay unavailable; launcher patch API missing");
        return;
    }

    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(dobby_actor_render_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));

    actorGetAabb = reinterpret_cast<ActorGetAabbFn>(getAabbAddress);
    levelGetRuntimeActorList = reinterpret_cast<LevelGetRuntimeActorListFn>(
            runtimeActorListAddress);
    levelForEachPlayer = reinterpret_cast<LevelForEachPlayerFn>(
            forEachPlayerAddress);
    levelGetPrimaryLocalPlayer = reinterpret_cast<LevelGetPrimaryLocalPlayerFn>(
            primaryLocalPlayerAddress);
    expectedClientLevelVtable = image.base + target::kClientLevelVtableOffset;
    minecraftImage = image;
    dobby_actor_render_continue = reinterpret_cast<void*>(renderAddress + replacement.size());
    auto* entry = reinterpret_cast<void*>(renderAddress);
    if (mcpelauncher_patch(entry, replacement.data(), replacement.size()) == nullptr ||
        std::memcmp(entry, replacement.data(), replacement.size()) != 0) {
        actorGetAabb = nullptr;
        levelGetRuntimeActorList = nullptr;
        levelForEachPlayer = nullptr;
        levelGetPrimaryLocalPlayer = nullptr;
        expectedClientLevelVtable = 0;
        minecraftImage = {};
        dobby_actor_render_continue = nullptr;
        runtimeState().setEntityHitboxesAvailable(false);
        logLine("ERROR: entity overlay unavailable; actor render hook rejected");
        return;
    }

    hookInstalled.store(true, std::memory_order_release);
    logLine("entity overlay: client actor bounds capture ready");
}

bool entityHitboxHookInstalled() {
    return hookInstalled.load(std::memory_order_acquire);
}

bool toggleEntityHitboxes() {
    if (!runtimeState().entityHitboxesAvailable()) {
        logLine("ERROR: entity hitbox overlay is unavailable");
        return false;
    }
    const bool enabled = !runtimeState().entityHitboxes();
    runtimeState().setEntityHitboxes(enabled);
    logLine(enabled ? "UI: entity hitboxes enabled" : "UI: entity hitboxes disabled");
    return enabled;
}

} // namespace dobby
#endif
