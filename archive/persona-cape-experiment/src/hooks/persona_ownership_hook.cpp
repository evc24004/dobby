#include "hooks/persona_ownership_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "hooks/minecraft_image.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

extern "C" void* dobby_persona_feature_continue = nullptr;

namespace dobby {
namespace {

constexpr std::string_view kPersonaRemoveOwnedChecksName{
        "personaRemoveOwnedChecks"};

std::atomic_bool ownershipHookReady{false};
std::atomic<void*> personaOwnershipFeature{nullptr};
MinecraftImage minecraftImage{};
std::uintptr_t expectedFeatureVtable{};

template <class Value>
Value readObjectField(const void* object, std::ptrdiff_t offset) {
    Value value{};
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset,
                sizeof(value));
    return value;
}

bool libcxxStringEquals(const void* object, std::string_view expected) {
    if (object == nullptr)
        return false;
    const auto first = readObjectField<std::uint8_t>(object, 0);
    const bool isLong = (first & 1U) != 0;
    const std::size_t size = isLong
            ? readObjectField<std::size_t>(object, sizeof(std::size_t))
            : static_cast<std::size_t>(first >> 1U);
    const char* data = isLong
            ? readObjectField<const char*>(object, 2 * sizeof(std::size_t))
            : reinterpret_cast<const char*>(object) + 1;
    return size == expected.size() && data != nullptr &&
            std::memcmp(data, expected.data(), expected.size()) == 0;
}

bool validCapturedFeature(const void* feature) {
    if (feature == nullptr || expectedFeatureVtable == 0 ||
        readObjectField<std::uintptr_t>(feature, 0) != expectedFeatureVtable) {
        return false;
    }
    const void* backing = readObjectField<const void*>(
            feature, target::kPersonaFeatureBackingOffset);
    return backing != nullptr &&
            readObjectField<std::int32_t>(
                    backing, target::kPersonaFeatureBackingIdOffset) ==
                    target::kPersonaRemoveOwnedChecksFeatureId &&
            libcxxStringEquals(
                    static_cast<const std::byte*>(backing) +
                            target::kPersonaFeatureBackingNameOffset,
                    kPersonaRemoveOwnedChecksName);
}

} // namespace

extern "C" std::uintptr_t dobby_prepare_persona_feature(
        void* feature, const void* internalName, std::uintptr_t originalValue) {
    if (!libcxxStringEquals(internalName, kPersonaRemoveOwnedChecksName))
        return originalValue;

    personaOwnershipFeature.store(feature, std::memory_order_release);
    const bool enabled = runtimeState().capeTestPackets();
    logLine(enabled
            ? "cape ownership test: persona owned-check bypass captured and enabled"
            : "cape ownership test: persona owned-check bypass captured; disabled by preference");
    return enabled ? 1U : 0U;
}

} // namespace dobby

extern "C" [[gnu::naked]] void dobby_persona_feature_detour() {
    asm volatile(
            // Preserve every integer argument and the caller return address
            // while the feature name is checked. The helper returns the value
            // that Bedrock should use for constructor argument x6.
            "sub sp, sp, #80\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "str x30, [sp, #64]\n"
            "mov x1, x5\n"
            "mov x2, x6\n"
            "bl dobby_prepare_persona_feature\n"
            "str x0, [sp, #48]\n"
            "ldp x0, x1, [sp, #0]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldr x30, [sp, #64]\n"
            "add sp, sp, #80\n"
            // Replay the four validated constructor instructions.
            "sub sp, sp, #144\n"
            "stp x29, x30, [sp, #64]\n"
            "stp x26, x25, [sp, #80]\n"
            "stp x24, x23, [sp, #96]\n"
            "adrp x16, dobby_persona_feature_continue\n"
            "ldr x16, [x16, :lo12:dobby_persona_feature_continue]\n"
            "br x16\n");
}

namespace dobby {

void installPersonaOwnershipHook() {
    if (ownershipHookReady.load(std::memory_order_acquire))
        return;
    minecraftImage = findMinecraftImage();
    const auto constructor =
            minecraftImage.base + target::kPersonaFeatureConstructorOffset;
    if (minecraftImage.base == 0) {
        logLine("ERROR: cape ownership test unavailable; Minecraft image missing");
        return;
    }
    if (mcpelauncher_patch == nullptr) {
        logLine("cape ownership test: launcher patch bridge pending until mod_init");
        return;
    }
    if (!addressIsExecutable(minecraftImage, constructor) ||
        !matchesSignature(
                reinterpret_cast<const void*>(constructor),
                target::kPersonaFeatureConstructorSignature)) {
        logLine("ERROR: cape ownership test unavailable; persona feature constructor mismatch");
        minecraftImage = {};
        return;
    }

    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour =
            reinterpret_cast<std::uintptr_t>(dobby_persona_feature_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));

    expectedFeatureVtable =
            minecraftImage.base + target::kPersonaBooleanFeatureVtableOffset;
    dobby_persona_feature_continue =
            reinterpret_cast<void*>(constructor + replacement.size());
    auto* entry = reinterpret_cast<void*>(constructor);
    if (mcpelauncher_patch(entry, replacement.data(), replacement.size()) ==
                nullptr ||
        std::memcmp(entry, replacement.data(), replacement.size()) != 0) {
        dobby_persona_feature_continue = nullptr;
        expectedFeatureVtable = 0;
        minecraftImage = {};
        logLine("ERROR: cape ownership test unavailable; constructor hook rejected");
        return;
    }

    ownershipHookReady.store(true, std::memory_order_release);
    logLine("cape ownership test: awaiting exact persona feature construction");
}

bool personaOwnershipHookInstalled() {
    return ownershipHookReady.load(std::memory_order_acquire);
}

bool personaOwnershipFeatureCaptured() {
    return validCapturedFeature(
            personaOwnershipFeature.load(std::memory_order_acquire));
}

bool setPersonaOwnershipBypass(bool enabled) {
    void* feature = personaOwnershipFeature.load(std::memory_order_acquire);
    if (!validCapturedFeature(feature))
        return false;
    auto* value = reinterpret_cast<std::uint8_t*>(
            static_cast<std::byte*>(feature) +
            target::kPersonaFeatureValueOffset);
    auto* defaultValue = reinterpret_cast<std::uint8_t*>(
            static_cast<std::byte*>(feature) +
            target::kPersonaFeatureDefaultOffset);
    __atomic_store_n(value, enabled ? 1U : 0U, __ATOMIC_RELEASE);
    __atomic_store_n(defaultValue, enabled ? 1U : 0U, __ATOMIC_RELEASE);
    logLine(enabled
            ? "cape ownership test: client persona owned-check bypass enabled"
            : "cape ownership test: client persona owned-check bypass disabled");
    return true;
}

} // namespace dobby

#else

namespace dobby {

void installPersonaOwnershipHook() {}
bool personaOwnershipHookInstalled() { return false; }
bool personaOwnershipFeatureCaptured() { return false; }
bool setPersonaOwnershipBypass(bool) { return false; }

} // namespace dobby

#endif
