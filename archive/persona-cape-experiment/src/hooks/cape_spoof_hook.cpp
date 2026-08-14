#include "hooks/cape_spoof_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "hooks/minecraft_image.hpp"
#include "hooks/outbound_packet_hook.hpp"
#include "hooks/persona_cape_repository_hook.hpp"
#include "platform/launcher.hpp"
#include "platform/local_capes.hpp"
#include "platform/log.hpp"
#include "platform/safe_memory.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

extern "C" const void* dobby_local_cape_image_getter_detour(
        const void* serializedSkinRef);

namespace dobby {
namespace {

using SetSkinFlagFn = void (*)(void* serializedSkinRef, bool value);
using SetSkinStringFn = void (*)(void* serializedSkinRef, const void* value);
using GetSkinFlagFn = bool (*)(const void* serializedSkinRef);
using GetSkinMemberFn = const void* (*)(const void* serializedSkinRef);

std::atomic_bool capeHookReady{false};
std::atomic_bool invalidPacketLogged{false};
std::atomic_bool mutationFailureLogged{false};
std::atomic_bool skinImageAppliedLogged{false};
std::uintptr_t expectedPlayerSkinVtable{};
SetSkinFlagFn setPremium = nullptr;
SetSkinFlagFn setPersona = nullptr;
SetSkinFlagFn setPersonaCapeOnClassic = nullptr;
SetSkinStringFn setCapeId = nullptr;
GetSkinFlagFn getPremium = nullptr;
GetSkinFlagFn getPersona = nullptr;
GetSkinFlagFn getPersonaCapeOnClassic = nullptr;
GetSkinMemberFn getCapeId = nullptr;
GetSkinMemberFn getCapeImage = nullptr;
thread_local bool capeImageObservationActive = false;

enum class CapePixelResult {
    notLocal,
    replaced,
    invalid,
};

struct NativeUuid {
    std::uint64_t high{};
    std::uint64_t low{};
};

struct NativeVector {
    void* begin{};
    void* end{};
    void* capacity{};
};

struct SelectedLocalCape {
    const LocalCape* cape{};
    void* personaPiece{};
};

static_assert(sizeof(std::string) == 24);
static_assert(sizeof(NativeUuid) == 16);
static_assert(sizeof(NativeVector) == 24);

template <class Value>
Value readObjectField(const void* object, std::ptrdiff_t offset) {
    Value value{};
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset,
                sizeof(value));
    return value;
}

bool objectHasVtable(const void* object, std::uintptr_t expectedVtable) {
    return object != nullptr && expectedVtable != 0 &&
            readObjectField<std::uintptr_t>(object, 0) == expectedVtable;
}

std::optional<std::string> readAndroidString(
        const void* object, std::size_t maximumLength) {
    constexpr std::size_t androidStringSize = 24;
    std::array<std::byte, androidStringSize> header{};
    if (!copyReadableMemory(object, header))
        return std::nullopt;
    const auto first = std::to_integer<std::uint8_t>(header[0]);
    const bool isLong = (first & 1U) != 0;
    std::size_t length{};
    const char* data{};
    if (isLong) {
        std::memcpy(&length, header.data() + sizeof(std::size_t), sizeof(length));
        std::memcpy(&data, header.data() + 2 * sizeof(std::size_t), sizeof(data));
    } else {
        length = static_cast<std::size_t>(first >> 1U);
        data = reinterpret_cast<const char*>(object) + 1;
    }
    if (length > maximumLength || (length != 0 && data == nullptr))
        return std::nullopt;
    std::string result(length, '\0');
    if (length != 0 &&
        !copyReadableMemory(
                data,
                std::span<std::byte>(
                        reinterpret_cast<std::byte*>(result.data()), length))) {
        return std::nullopt;
    }
    return result;
}

template <class Value>
std::optional<Value> readSafeValue(const void* address) {
    Value value{};
    if (!copyReadableMemory(
                address,
                std::span<std::byte>(
                        reinterpret_cast<std::byte*>(&value), sizeof(value)))) {
        return std::nullopt;
    }
    return value;
}

int hexadecimalDigit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

std::optional<NativeUuid> parseUuid(std::string_view value) {
    if (!validLocalCapeId(value))
        return std::nullopt;
    NativeUuid result;
    std::size_t nibbleIndex = 0;
    for (const char character : value) {
        if (character == '-')
            continue;
        const int digit = hexadecimalDigit(character);
        if (digit < 0)
            return std::nullopt;
        auto& half = nibbleIndex < 16 ? result.high : result.low;
        half = (half << 4U) | static_cast<std::uint64_t>(digit);
        ++nibbleIndex;
    }
    return nibbleIndex == 32 ? std::optional<NativeUuid>(result) : std::nullopt;
}

std::optional<std::size_t> serializedPersonaPieceCount(
        const NativeVector& vector) {
    const auto begin = reinterpret_cast<std::uintptr_t>(vector.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(vector.end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(vector.capacity);
    if (begin == 0 || end == 0 || capacity == 0) {
        return begin == 0 && end == 0 && capacity == 0
                ? std::optional<std::size_t>(0)
                : std::nullopt;
    }
    if (end < begin || capacity < end)
        return std::nullopt;
    const auto bytes = end - begin;
    const auto capacityBytes = capacity - begin;
    if (bytes % target::kSerializedPersonaPieceSize != 0 ||
        capacityBytes % target::kSerializedPersonaPieceSize != 0) {
        return std::nullopt;
    }
    constexpr std::size_t maximumPieces = 128;
    const auto count = bytes / target::kSerializedPersonaPieceSize;
    const auto capacityCount = capacityBytes / target::kSerializedPersonaPieceSize;
    if (count > maximumPieces || capacityCount > maximumPieces * 4)
        return std::nullopt;
    return count;
}

std::optional<SelectedLocalCape> selectedLocalCapeFromPersonaPieces(
        const void* implementation) {
    const auto vector = readSafeValue<NativeVector>(
            static_cast<const std::byte*>(implementation) +
            target::kSerializedSkinImplPersonaPiecesOffset);
    const auto count = vector ? serializedPersonaPieceCount(*vector) : std::nullopt;
    if (!count)
        return std::nullopt;

    SelectedLocalCape selected;
    for (std::size_t index = 0; index < *count; ++index) {
        auto* piece = static_cast<std::byte*>(vector->begin) +
                index * target::kSerializedPersonaPieceSize;
        const auto pieceType = readSafeValue<std::int32_t>(
                piece + target::kSerializedPersonaPieceTypeOffset);
        if (!pieceType || *pieceType != target::kPersonaCapePieceType)
            continue;
        const auto pieceId = readAndroidString(
                piece + target::kSerializedPersonaPieceIdOffset, 36);
        if (!pieceId)
            return std::nullopt;
        const LocalCape* cape = findLocalCape(*pieceId);
        if (cape == nullptr)
            continue;
        if (selected.cape != nullptr)
            return std::nullopt;
        selected = SelectedLocalCape{cape, piece};
    }
    return selected.cape == nullptr
            ? std::optional<SelectedLocalCape>{}
            : std::optional<SelectedLocalCape>(selected);
}

bool selectedPersonaPieceMatchesDescriptor(const SelectedLocalCape& selected) {
    const auto packId = parseUuid(selected.cape->descriptor.packId);
    if (!packId || selected.personaPiece == nullptr)
        return false;
    auto* piece = static_cast<std::byte*>(selected.personaPiece);
    const auto actualPackId = readSafeValue<NativeUuid>(
            piece + target::kSerializedPersonaPiecePackIdOffset);
    const auto isDefault = readSafeValue<bool>(
            piece + target::kSerializedPersonaPieceIsDefaultOffset);
    const auto productId = readAndroidString(
            piece + target::kSerializedPersonaPieceProductIdOffset, 36);
    return actualPackId &&
            std::memcmp(&*actualPackId, &*packId, sizeof(*packId)) == 0 &&
            isDefault && !*isDefault &&
            productId && *productId == selected.cape->descriptor.pieceId;
}

bool alignSelectedPersonaPiece(const SelectedLocalCape& selected) {
    const auto packId = parseUuid(selected.cape->descriptor.packId);
    if (!packId || selected.personaPiece == nullptr)
        return false;
    auto* piece = static_cast<std::byte*>(selected.personaPiece);
    std::memcpy(
            piece + target::kSerializedPersonaPiecePackIdOffset,
            &*packId, sizeof(*packId));
    *reinterpret_cast<bool*>(
            piece + target::kSerializedPersonaPieceIsDefaultOffset) = false;
    *reinterpret_cast<std::string*>(
            piece + target::kSerializedPersonaPieceProductIdOffset) =
            selected.cape->descriptor.pieceId;
    return selectedPersonaPieceMatchesDescriptor(selected);
}

CapePixelResult replaceLocalCapePixels(
        void* serializedSkinRef, const void* implementation,
        const LocalCape*& selectedCape) {
    selectedCape = nullptr;
    const void* capeIdObject = getCapeId(serializedSkinRef);
    const void* capeImage = getCapeImage(serializedSkinRef);
    const auto* expectedCapeId = static_cast<const std::byte*>(implementation) +
            target::kSerializedSkinImplCapeIdOffset;
    const auto* expectedCapeImage = static_cast<const std::byte*>(implementation) +
            target::kSerializedSkinImplCapeImageOffset;
    if (capeIdObject != expectedCapeId || capeImage != expectedCapeImage)
        return CapePixelResult::invalid;

    auto capeId = readAndroidString(capeIdObject, 36);
    if (!capeId)
        return CapePixelResult::invalid;
    const LocalCape* localCape = findLocalCape(*capeId);
    const auto selected = selectedLocalCapeFromPersonaPieces(implementation);
    if (localCape == nullptr && !selected)
        return CapePixelResult::notLocal;
    if (!selected || selected->cape == nullptr ||
        (localCape != nullptr && localCape != selected->cape) ||
        !alignSelectedPersonaPiece(*selected)) {
        return CapePixelResult::invalid;
    }
    localCape = selected->cape;
    if (*capeId != localCape->descriptor.pieceId) {
        setCapeId(serializedSkinRef, &localCape->descriptor.pieceId);
        capeIdObject = getCapeId(serializedSkinRef);
        capeId = readAndroidString(capeIdObject, 36);
        if (capeIdObject != expectedCapeId || !capeId ||
            *capeId != localCape->descriptor.pieceId) {
            return CapePixelResult::invalid;
        }
    }

    std::array<std::byte, 48> imageLayout{};
    if (!copyReadableMemory(capeImage, imageLayout))
        return CapePixelResult::invalid;
    std::uint32_t format{};
    std::uint32_t width{};
    std::uint32_t height{};
    void* pixels{};
    std::size_t pixelCount{};
    std::memcpy(
            &format, imageLayout.data() + target::kSkinImageFormatOffset,
            sizeof(format));
    std::memcpy(
            &width, imageLayout.data() + target::kSkinImageWidthOffset,
            sizeof(width));
    std::memcpy(
            &height, imageLayout.data() + target::kSkinImageHeightOffset,
            sizeof(height));
    std::memcpy(
            &pixels, imageLayout.data() + target::kSkinImageBlobDataOffset,
            sizeof(pixels));
    std::memcpy(
            &pixelCount, imageLayout.data() + target::kSkinImageBlobSizeOffset,
            sizeof(pixelCount));
    if (format != target::kRgba8ImageFormat ||
        width != kCapeTextureWidth || height != kCapeTextureHeight ||
        pixels == nullptr || pixelCount != kCapeTextureBytes) {
        return CapePixelResult::invalid;
    }

    std::array<std::byte, kCapeTextureBytes> readablePixels{};
    if (!copyReadableMemory(pixels, readablePixels))
        return CapePixelResult::invalid;
    std::memcpy(pixels, localCape->rgba.data(), localCape->rgba.size());
    if (std::memcmp(pixels, localCape->rgba.data(), localCape->rgba.size()) != 0)
        return CapePixelResult::invalid;
    selectedCape = localCape;
    return CapePixelResult::replaced;
}

void observeSelectedLocalCapeImage(
        void* serializedSkinRef, const void* implementation) {
    if (!capeHookReady.load(std::memory_order_acquire) ||
        !runtimeState().capeTestPackets()) {
        return;
    }
    const LocalCape* selectedCape = nullptr;
    const auto result = replaceLocalCapePixels(
            serializedSkinRef, implementation, selectedCape);
    if (result != CapePixelResult::replaced || selectedCape == nullptr ||
        skinImageAppliedLogged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const std::string detail =
            "piece_id=" + selectedCape->descriptor.pieceId +
            ";pack_id=" + selectedCape->descriptor.packId +
            ";rgba_bytes=" + std::to_string(selectedCape->rgba.size());
    logLine("local cape skin image PASS: " + detail);
    recordLifecycleEvent("local_cape_skin_image_applied", detail);
}

std::optional<std::array<std::uint8_t, 12>> directBranchInstructions(
        std::uintptr_t entry, std::uintptr_t destination) {
    const auto entryPage = entry & ~std::uintptr_t{0xfff};
    const auto destinationPage = destination & ~std::uintptr_t{0xfff};
    const auto pageDelta = static_cast<std::int64_t>(destinationPage) -
            static_cast<std::int64_t>(entryPage);
    if (pageDelta % 4096 != 0)
        return std::nullopt;
    const auto pageImmediate = pageDelta / 4096;
    if (pageImmediate < -(1 << 20) || pageImmediate >= (1 << 20))
        return std::nullopt;

    const auto encodedImmediate =
            static_cast<std::uint64_t>(pageImmediate) & 0x1fffffU;
    const std::uint32_t loadPage = 0x90000010U |
            static_cast<std::uint32_t>((encodedImmediate & 3U) << 29U) |
            static_cast<std::uint32_t>(((encodedImmediate >> 2U) & 0x7ffffU)
                                       << 5U);
    const std::uint32_t addPageOffset = 0x91000210U |
            static_cast<std::uint32_t>((destination & 0xfffU) << 10U);
    constexpr std::uint32_t branchRegister = 0xd61f0200U;
    std::array<std::uint8_t, 12> result{};
    std::memcpy(result.data(), &loadPage, sizeof(loadPage));
    std::memcpy(result.data() + 4, &addPageOffset, sizeof(addPageOffset));
    std::memcpy(result.data() + 8, &branchRegister, sizeof(branchRegister));
    return result;
}

bool patchCapeImageGetters(const MinecraftImage& image) {
    const auto destination = reinterpret_cast<std::uintptr_t>(
            dobby_local_cape_image_getter_detour);
    for (const auto offset : target::kSerializedSkinCapeImageGetterOffsets) {
        const auto entry = image.base + offset;
        if (!addressIsExecutable(image, entry) ||
            !matchesSignature(
                    reinterpret_cast<const void*>(entry),
                    target::kSerializedSkinGetCapeImageSignature) ||
            !directBranchInstructions(entry, destination)) {
            return false;
        }
    }
    for (const auto offset : target::kSerializedSkinCapeImageGetterOffsets) {
        auto* entry = reinterpret_cast<void*>(image.base + offset);
        const auto replacement = directBranchInstructions(
                reinterpret_cast<std::uintptr_t>(entry), destination);
        if (!replacement || mcpelauncher_patch == nullptr ||
            mcpelauncher_patch(
                    entry,
                    const_cast<std::uint8_t*>(replacement->data()),
                    replacement->size()) == nullptr ||
            std::memcmp(entry, replacement->data(), replacement->size()) != 0) {
            return false;
        }
    }
    return true;
}

void mutatePlayerSkinPacket(void* packet, std::int32_t packetId) {
    if (packetId != target::kPlayerSkinPacketId ||
        !capeHookReady.load(std::memory_order_acquire) ||
        !runtimeState().capeTestPackets()) {
        return;
    }
    if (!objectHasVtable(packet, expectedPlayerSkinVtable)) {
        if (!invalidPacketLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: cape test ignored PlayerSkinPacket vtable mismatch");
        return;
    }

    void* serializedSkinRef = static_cast<std::byte*>(packet) +
            target::kPlayerSkinSerializedSkinRefOffset;
    const void* implementation = readObjectField<const void*>(serializedSkinRef, 0);
    const void* owner = readObjectField<const void*>(serializedSkinRef,
                                                     sizeof(void*));
    if (implementation == nullptr || owner == nullptr) {
        if (!invalidPacketLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: cape test ignored empty SerializedSkinRef");
        return;
    }

    const LocalCape* selectedCape = nullptr;
    const auto pixelResult = replaceLocalCapePixels(
            serializedSkinRef, implementation, selectedCape);
    if (pixelResult == CapePixelResult::notLocal)
        return;
    if (pixelResult == CapePixelResult::invalid) {
        if (!mutationFailureLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: cape test ignored skin without an exact local cape image");
        return;
    }

    setPremium(serializedSkinRef, true);
    setPersona(serializedSkinRef, false);
    setPersonaCapeOnClassic(serializedSkinRef, true);
    if (!getPremium(serializedSkinRef) || getPersona(serializedSkinRef) ||
        !getPersonaCapeOnClassic(serializedSkinRef) || selectedCape == nullptr) {
        if (!mutationFailureLogged.exchange(true, std::memory_order_acq_rel))
            logLine("ERROR: cape test flag verification failed; disabling mutation");
        capeHookReady.store(false, std::memory_order_release);
        runtimeState().setCapeTestPacketsAvailable(false);
        return;
    }

    const std::string detail =
            "piece_id=" + selectedCape->descriptor.pieceId +
            ";pack_id=" + selectedCape->descriptor.packId +
            ";rgba_bytes=" + std::to_string(selectedCape->rgba.size()) +
            ";premium=1;persona=0;cape_on_classic=1";
    logLine("cape test PlayerSkinPacket PASS: " + detail);
    recordLifecycleEvent(
            "cape_packet_mutated", detail);
}

bool validateFunction(const MinecraftImage& image, std::uintptr_t offset,
                      const auto& signature) {
    const auto address = image.base + offset;
    return addressIsExecutable(image, address) &&
            matchesSignature(reinterpret_cast<const void*>(address), signature);
}

} // namespace

extern "C" const void* dobby_local_cape_image_getter_detour(
        const void* serializedSkinRef) {
    if (serializedSkinRef == nullptr)
        return nullptr;
    const void* implementation = readObjectField<const void*>(
            serializedSkinRef, 0);
    if (implementation == nullptr)
        return nullptr;
    const void* result = static_cast<const std::byte*>(implementation) +
            target::kSerializedSkinImplCapeImageOffset;
    if (!capeImageObservationActive) {
        capeImageObservationActive = true;
        observeSelectedLocalCapeImage(
                const_cast<void*>(serializedSkinRef), implementation);
        capeImageObservationActive = false;
    }
    return result;
}

void installCapeSpoofHook() {
    if (capeHookReady.load(std::memory_order_acquire))
        return;
    runtimeState().setCapeTestPacketsAvailable(false);
    const auto image = findMinecraftImage();
    expectedPlayerSkinVtable =
            image.base + target::kPlayerSkinPacketVtableOffset;
    const auto getId = image.base + target::kPlayerSkinGetIdOffset;
    const auto getIdSlot = expectedPlayerSkinVtable +
            target::kPacketGetIdVtableSlot * sizeof(std::uintptr_t);
    std::uintptr_t currentGetId{};
    if (image.base == 0 || !outboundPacketHookInstalled() ||
        !personaCapeRepositoryHookInstalled() || localCapes().empty() ||
        !addressIsInImage(image, expectedPlayerSkinVtable) ||
        !addressIsInImage(image, getIdSlot) ||
        !validateFunction(image, target::kPlayerSkinGetIdOffset,
                          target::kPlayerSkinGetIdSignature) ||
        !validateFunction(image, target::kPlayerSkinLayoutProbeOffset,
                          target::kPlayerSkinLayoutProbeSignature) ||
        !validateFunction(image,
                          target::kSerializedSkinSetPersonaCapeOnClassicOffset,
                          target::kSerializedSkinSetPersonaCapeOnClassicSignature) ||
        !validateFunction(image, target::kSerializedSkinSetPremiumOffset,
                          target::kSerializedSkinSetPremiumSignature) ||
        !validateFunction(image, target::kSerializedSkinSetPersonaOffset,
                          target::kSerializedSkinSetPersonaSignature) ||
        !validateFunction(image, target::kSerializedSkinGetPremiumOffset,
                          target::kSerializedSkinGetPremiumSignature) ||
        !validateFunction(image, target::kSerializedSkinGetPersonaOffset,
                          target::kSerializedSkinGetPersonaSignature) ||
        !validateFunction(image,
                          target::kSerializedSkinGetPersonaCapeOnClassicOffset,
                          target::kSerializedSkinGetPersonaCapeOnClassicSignature) ||
        !validateFunction(image, target::kSerializedSkinGetCapeIdOffset,
                          target::kSerializedSkinGetCapeIdSignature) ||
        !validateFunction(image, target::kSerializedSkinSetCapeIdOffset,
                          target::kSerializedSkinSetCapeIdSignature) ||
        !validateFunction(image, target::kSerializedSkinGetCapeImageOffset,
                          target::kSerializedSkinGetCapeImageSignature)) {
        logLine("ERROR: cape entitlement test unavailable; target layout mismatch");
        return;
    }
    std::memcpy(&currentGetId, reinterpret_cast<const void*>(getIdSlot),
                sizeof(currentGetId));
    if (currentGetId != getId) {
        logLine("ERROR: cape entitlement test unavailable; PlayerSkinPacket vtable mismatch");
        return;
    }

    setPersonaCapeOnClassic = reinterpret_cast<SetSkinFlagFn>(
            image.base + target::kSerializedSkinSetPersonaCapeOnClassicOffset);
    setPremium = reinterpret_cast<SetSkinFlagFn>(
            image.base + target::kSerializedSkinSetPremiumOffset);
    setPersona = reinterpret_cast<SetSkinFlagFn>(
            image.base + target::kSerializedSkinSetPersonaOffset);
    getPremium = reinterpret_cast<GetSkinFlagFn>(
            image.base + target::kSerializedSkinGetPremiumOffset);
    getPersona = reinterpret_cast<GetSkinFlagFn>(
            image.base + target::kSerializedSkinGetPersonaOffset);
    getPersonaCapeOnClassic = reinterpret_cast<GetSkinFlagFn>(
            image.base + target::kSerializedSkinGetPersonaCapeOnClassicOffset);
    getCapeId = reinterpret_cast<GetSkinMemberFn>(
            image.base + target::kSerializedSkinGetCapeIdOffset);
    setCapeId = reinterpret_cast<SetSkinStringFn>(
            image.base + target::kSerializedSkinSetCapeIdOffset);
    getCapeImage = reinterpret_cast<GetSkinMemberFn>(
            image.base + target::kSerializedSkinGetCapeImageOffset);
    if (!registerOutboundPacketHandler(mutatePlayerSkinPacket)) {
        logLine("ERROR: cape entitlement test unavailable; outbound router full");
        return;
    }
    if (!patchCapeImageGetters(image)) {
        logLine("ERROR: cape entitlement test unavailable; cape image getter patch rejected");
        return;
    }

    capeHookReady.store(true, std::memory_order_release);
    runtimeState().setCapeTestPacketsAvailable(true);
    logLine(runtimeState().capeTestPackets()
            ? "cape entitlement test ready and enabled by saved preference"
            : "cape entitlement test ready; enable it in Mods > Dobby");
}

bool capeSpoofHookInstalled() {
    return capeHookReady.load(std::memory_order_acquire);
}

} // namespace dobby

#else

namespace dobby {

void installCapeSpoofHook() {}
bool capeSpoofHookInstalled() { return false; }

} // namespace dobby

#endif
