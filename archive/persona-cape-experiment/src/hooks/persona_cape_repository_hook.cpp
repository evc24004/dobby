#include "hooks/persona_cape_repository_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "hooks/minecraft_image.hpp"
#include "platform/launcher.hpp"
#include "platform/local_capes.hpp"
#include "platform/log.hpp"
#include "platform/process_memory.hpp"
#include "platform/safe_memory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" void* dobby_captured_persona_manager = nullptr;
extern "C" void* dobby_persona_manager_owned_continue = nullptr;
extern "C" void* dobby_persona_offline_query_continue = nullptr;
extern "C" void* dobby_offer_collection_replace_continue = nullptr;
extern "C" void dobby_prepare_persona_owned_query(
        void* manager, const void* pieceTypes);
extern "C" void dobby_prepare_persona_offline_query(void* repository);
extern "C" void dobby_prepare_offer_collection(
        void* component, void* storeItems);

extern "C" [[gnu::naked]] void dobby_persona_manager_owned_capture_detour() {
    asm volatile(
            // Preserve the query arguments, hidden vector result pointer, and
            // return address while local entries are installed. This runs
            // before Bedrock builds the result, so the first picker query sees
            // the additions instead of caching the pre-injection result.
            "sub sp, sp, #32\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x8, x30, [sp, #16]\n"
            "adrp x16, dobby_captured_persona_manager\n"
            "add x16, x16, :lo12:dobby_captured_persona_manager\n"
            "str x0, [x16]\n"
            "bl dobby_prepare_persona_owned_query\n"
            "ldp x0, x1, [sp, #0]\n"
            "ldp x8, x30, [sp, #16]\n"
            "add sp, sp, #32\n"
            // Replay the exact validated prologue.
            "sub sp, sp, #128\n"
            "stp x29, x30, [sp, #32]\n"
            "stp x28, x27, [sp, #48]\n"
            "stp x26, x25, [sp, #64]\n"
            "adrp x16, dobby_persona_manager_owned_continue\n"
            "ldr x16, [x16, :lo12:dobby_persona_manager_owned_continue]\n"
            "br x16\n");
}

extern "C" [[gnu::naked]] void dobby_persona_offline_query_detour() {
    asm volatile(
            // Preserve the repository, PlayFab ID, hidden vector-result
            // pointer, all other volatile arguments, and SIMD arguments while
            // the exact offline manager is populated before this query reads
            // it. The public query then builds its own normal result.
            "sub sp, sp, #224\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "stp x8, x30, [sp, #64]\n"
            "stp q0, q1, [sp, #80]\n"
            "stp q2, q3, [sp, #112]\n"
            "stp q4, q5, [sp, #144]\n"
            "stp q6, q7, [sp, #176]\n"
            "bl dobby_prepare_persona_offline_query\n"
            "ldp q6, q7, [sp, #176]\n"
            "ldp q4, q5, [sp, #144]\n"
            "ldp q2, q3, [sp, #112]\n"
            "ldp q0, q1, [sp, #80]\n"
            "ldp x8, x30, [sp, #64]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x0, x1, [sp, #0]\n"
            "add sp, sp, #224\n"
            // Replay the exact validated 16-byte offline-query prologue.
            "sub sp, sp, #208\n"
            "stp x29, x30, [sp, #112]\n"
            "stp x28, x27, [sp, #128]\n"
            "stp x26, x25, [sp, #144]\n"
            "adrp x16, dobby_persona_offline_query_continue\n"
            "ldr x16, [x16, :lo12:dobby_persona_offline_query_continue]\n"
            "br x16\n");
}

extern "C" [[gnu::naked]] void dobby_offer_collection_replace_detour() {
    asm volatile(
            // OfferCollectionComponent::replaceOffers receives the component
            // and vector<IStoreCatalogItem*> in x0/x1. Preserve the complete
            // volatile argument state while the exact Pan source list is
            // expanded with validated local catalog clones.
            "sub sp, sp, #224\n"
            "stp x0, x1, [sp, #0]\n"
            "stp x2, x3, [sp, #16]\n"
            "stp x4, x5, [sp, #32]\n"
            "stp x6, x7, [sp, #48]\n"
            "stp x8, x30, [sp, #64]\n"
            "stp q0, q1, [sp, #80]\n"
            "stp q2, q3, [sp, #112]\n"
            "stp q4, q5, [sp, #144]\n"
            "stp q6, q7, [sp, #176]\n"
            "bl dobby_prepare_offer_collection\n"
            "ldp q6, q7, [sp, #176]\n"
            "ldp q4, q5, [sp, #144]\n"
            "ldp q2, q3, [sp, #112]\n"
            "ldp q0, q1, [sp, #80]\n"
            "ldp x8, x30, [sp, #64]\n"
            "ldp x6, x7, [sp, #48]\n"
            "ldp x4, x5, [sp, #32]\n"
            "ldp x2, x3, [sp, #16]\n"
            "ldp x0, x1, [sp, #0]\n"
            "add sp, sp, #224\n"
            // Replay the exact validated 16-byte replaceOffers prologue.
            "sub sp, sp, #224\n"
            "stp x29, x30, [sp, #128]\n"
            "stp x28, x27, [sp, #144]\n"
            "stp x26, x25, [sp, #160]\n"
            "adrp x16, dobby_offer_collection_replace_continue\n"
            "ldr x16, [x16, :lo12:dobby_offer_collection_replace_continue]\n"
            "br x16\n");
}

namespace dobby {
namespace {

constexpr std::string_view kPanCapeId{
        "ef479b6d-7072-47aa-8985-0f025cd24cdb"};
constexpr std::string_view kPanCapeProductId{
        "f72bd899-c3f6-4516-ad10-a70c36a98641"};
constexpr std::string_view kPanCapeTitle{"The Pan Cape!"};
constexpr std::string_view kPersonaPackId{
        "c18e65aa-7b21-4637-9b63-8ad63622ef01"};
constexpr std::size_t kAndroidStringSize = 24;
constexpr std::size_t kMaximumNativeOwnedPieces = 512;
constexpr std::size_t kMaximumOfflineCatalogItems = 512;
constexpr std::size_t kMaximumDiscoveredCatalogItems = 1024;
constexpr std::size_t kMaximumCapeIndexPieces =
        kMaximumNativeOwnedPieces + kMaximumLocalCapes;

struct NativeUuid {
    std::uint64_t high{};
    std::uint64_t low{};

    bool operator==(const NativeUuid&) const = default;
};

struct NativeVector {
    void* begin{};
    void* end{};
    void* capacity{};
};

// PersonaPieceManager::getOwnedPieces returns Bedrock non-owning handles, not
// std::shared_ptr. Its append helper writes {nullptr, PersonaPiece*} and its
// relocation helper copies the 16-byte value verbatim. Modeling this as a
// shared_ptr reads the wrong word and attempts to release a raw game pointer.
struct NativePersonaPieceHandle {
    void* owner{};
    const void* piece{};
};

// abf099c constructs PersonaOfflineCatalogItem as five libc++ strings plus
// these two scalars. The picker-facing accessor abf0da0 returns catalogId at
// +0x18. Keep this exact layout guarded: shared objects placed in Bedrock's
// native offline map are later destroyed through this module's control block.
struct NativePersonaOfflineCatalogItem {
    std::string productId;
    std::string catalogId;
    std::string personaPieceId;
    bool owned{};
    std::int32_t state{};
    std::string category;
    std::string title;
};

struct NativeTreeNodeLinks {
    std::uintptr_t left{};
    std::uintptr_t right{};
    std::uintptr_t parent{};
};

struct alignas(16) SyntheticStoreCatalogItem {
    std::array<std::byte, target::kStoreCatalogItemSize> bytes{};
};

struct PanStoreStringOffsets {
    std::vector<std::size_t> productIds;
    std::vector<std::size_t> pieceIds;
    std::vector<std::size_t> titles;
};

using ManagerLookupFn = const void* (*)(void* manager, const void* pieceId);
using ManagerInsertFn = const void* (*)(
        void* manager, const void* pieceId, void* piece);
using ManagerOwnedPiecesFn = std::vector<NativePersonaPieceHandle> (*)(
        void* manager, const std::vector<std::int32_t>* pieceTypes);
using RepositoryOwnedPiecesFn = std::vector<NativePersonaPieceHandle> (*)(
        void* repository, const std::vector<std::int32_t>* pieceTypes);
using PersonaPieceCopyFn = void* (*)(void* destination, const void* source);
using PersonaPieceDestroyFn = void (*)(void* piece);
using PersonaPieceIsValidFn = bool (*)(const void* piece);
using RepositoryOfflinePiecesFn = std::vector<std::string> (*)(
        void* repository, const std::string* playerId);
using NativeTreeInsertFn = void* (*)(
        void* tree, const std::string* lookupKey,
        const std::string* insertedKey);
using NativeTreeLookupFn = void* (*)(
        void* tree, const std::string* key);
using NativeSetInsertFn = void (*)(
        void* tree, const std::string* lookupKey,
        const std::string* insertedKey);
using OfferCollectionReplaceFn = void (*)(
        void* component, const std::vector<void*>* storeItems);
static_assert(sizeof(std::string) == kAndroidStringSize);
static_assert(sizeof(NativeUuid) == 16);
static_assert(sizeof(NativeVector) == 24);
static_assert(sizeof(NativePersonaPieceHandle) == 16);
static_assert(sizeof(NativePersonaOfflineCatalogItem) == 0x80);
static_assert(offsetof(NativePersonaOfflineCatalogItem, productId) == 0x00);
static_assert(offsetof(NativePersonaOfflineCatalogItem, catalogId) == 0x18);
static_assert(offsetof(NativePersonaOfflineCatalogItem, personaPieceId) == 0x30);
static_assert(offsetof(NativePersonaOfflineCatalogItem, owned) == 0x48);
static_assert(offsetof(NativePersonaOfflineCatalogItem, state) == 0x4c);
static_assert(offsetof(NativePersonaOfflineCatalogItem, category) == 0x50);
static_assert(offsetof(NativePersonaOfflineCatalogItem, title) == 0x68);

std::atomic_bool repositoryHookReady{false};
std::atomic_bool repositoryPreinitRegistered{false};
std::atomic_bool acceptancePassed{false};
std::atomic_bool acceptanceFailureLogged{false};
std::atomic_bool offlineCatalogAcceptancePassed{false};
// Set only after the downstream PieceOfferWrapper collection contains every
// local cape. Repository ID enumeration is necessary, but it is not evidence
// that the dressing-room ItemListComponent materialized visible tiles.
std::atomic_bool visibleOfferAcceptancePassed{false};
std::atomic_bool offlineCatalogFailureLogged{false};
std::atomic_uint32_t offlineCatalogStage{0};
std::atomic_size_t offlineCatalogItemAuditCount{0};
std::atomic_size_t offlineCatalogPlayerAuditCount{0};
std::atomic_size_t offlineCatalogConsumerAuditCount{0};
std::atomic_size_t offlineCatalogConsumerTotal{0};
std::atomic_bool liveRepositoryRetryFinished{false};
std::atomic_bool pickerQueryObserved{false};
std::atomic_bool offlineQueryObserved{false};
std::atomic_bool offerReplaceEntryObserved{false};
std::atomic_bool offerCollectionObserved{false};
std::atomic_bool panStoreItemObserved{false};
std::atomic_bool sourceExpansionLogged{false};
std::atomic_bool liveOfferScanLogged{false};
std::atomic_bool forcedVisibleOfferInjectionAttempted{false};
std::atomic_size_t renderedOfferAuditCount{0};
std::atomic_size_t localRenderedOfferAuditCount{0};
std::atomic_uint32_t liveOfferScanFrames{0};
std::atomic<void*> capturedOfferCollection{nullptr};
std::atomic_uint32_t liveRepositoryRetryFrames{0};
std::atomic<void*> capturedPersonaManager{nullptr};
std::atomic<void*> capturedPersonaRepository{nullptr};
ManagerLookupFn managerLookup = nullptr;
ManagerInsertFn managerInsert = nullptr;
ManagerOwnedPiecesFn managerOwnedPieces = nullptr;
RepositoryOwnedPiecesFn repositoryOwnedPieces = nullptr;
PersonaPieceCopyFn copyPersonaPiece = nullptr;
PersonaPieceDestroyFn destroyPersonaPiece = nullptr;
PersonaPieceIsValidFn isValidPersonaPiece = nullptr;
RepositoryOfflinePiecesFn repositoryOfflinePieces = nullptr;
NativeTreeInsertFn offlineCatalogItemInsert = nullptr;
NativeTreeInsertFn offlineCatalogPlayerInsert = nullptr;
NativeTreeLookupFn offlineCatalogPlayerLookup = nullptr;
NativeTreeLookupFn offlineCatalogItemLookup = nullptr;
NativeSetInsertFn offlineCatalogIdSetInsert = nullptr;
OfferCollectionReplaceFn offerCollectionReplace = nullptr;
std::mutex injectionMutex;
std::mutex offlineCatalogMutex;
std::mutex storeCatalogMutex;
std::vector<std::shared_ptr<NativePersonaOfflineCatalogItem>>
        retainedOfflineCatalogItems;
std::array<SyntheticStoreCatalogItem, kMaximumLocalCapes>
        syntheticStoreCatalogItems;
std::array<std::shared_ptr<void>, kMaximumLocalCapes>
        retainedSyntheticStoreCatalogItems;
std::size_t syntheticStoreCatalogItemCount = 0;
std::uintptr_t expectedOfferCollectionVtable = 0;
std::uintptr_t expectedStoreCatalogItemVtable = 0;
thread_local bool dobbyOwnedQueryActive = false;
thread_local bool dobbyOfflineQueryActive = false;
thread_local bool dobbyOfferCollectionActive = false;

template <class Value>
std::optional<Value> readMemory(const void* address) {
    Value value{};
    if (!copyReadableMemory(
                address,
                std::span<std::byte>(
                        reinterpret_cast<std::byte*>(&value), sizeof(value)))) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> readAndroidString(
        const void* object, std::size_t maximumLength = 80) {
    std::array<std::byte, kAndroidStringSize> header{};
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
        data = static_cast<const char*>(object) + 1;
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

bool exactStoreCatalogVtable(const void* item) {
    const auto vtable = readMemory<std::uintptr_t>(item);
    return vtable && expectedStoreCatalogItemVtable != 0 &&
            *vtable == expectedStoreCatalogItemVtable;
}

std::optional<std::string> storeCatalogItemKey(const void* item) {
    if (!exactStoreCatalogVtable(item))
        return std::nullopt;
    return readAndroidString(
            static_cast<const std::byte*>(item) +
                    target::kStoreCatalogItemKeyOffset,
            128);
}

bool exactPanStoreCatalogItem(const void* item) {
    const auto key = storeCatalogItemKey(item);
    return key && (*key == kPanCapeProductId || *key == kPanCapeId);
}

std::optional<PanStoreStringOffsets> findPanStoreStringOffsets(
        const void* item) {
    if (!exactPanStoreCatalogItem(item))
        return std::nullopt;

    PanStoreStringOffsets offsets;
    for (std::size_t offset = 0;
         offset + kAndroidStringSize <= target::kStoreCatalogItemSize;
         offset += alignof(std::string)) {
        const auto value = readAndroidString(
                static_cast<const std::byte*>(item) + offset, 80);
        if (!value)
            continue;
        if (*value == kPanCapeProductId)
            offsets.productIds.push_back(offset);
        else if (*value == kPanCapeId)
            offsets.pieceIds.push_back(offset);
        else if (*value == kPanCapeTitle)
            offsets.titles.push_back(offset);
    }

    constexpr std::size_t kMaximumMatchingStringsPerField = 32;
    const bool bounded = !offsets.productIds.empty() &&
            !offsets.pieceIds.empty() && !offsets.titles.empty() &&
            offsets.productIds.size() <= kMaximumMatchingStringsPerField &&
            offsets.pieceIds.size() <= kMaximumMatchingStringsPerField &&
            offsets.titles.size() <= kMaximumMatchingStringsPerField;
    if (!bounded)
        return std::nullopt;

    const auto keyOffset = static_cast<std::size_t>(
            target::kStoreCatalogItemKeyOffset);
    const bool keyProved =
            std::find(
                    offsets.productIds.begin(), offsets.productIds.end(),
                    keyOffset) != offsets.productIds.end() ||
            std::find(
                    offsets.pieceIds.begin(), offsets.pieceIds.end(),
                    keyOffset) != offsets.pieceIds.end();
    return keyProved
            ? std::optional<PanStoreStringOffsets>(std::move(offsets))
            : std::nullopt;
}

void replaceStoreStrings(
        SyntheticStoreCatalogItem& item,
        const std::vector<std::size_t>& offsets,
        const std::string& replacement) {
    for (const auto offset : offsets) {
        std::construct_at(
                reinterpret_cast<std::string*>(item.bytes.data() + offset),
                replacement);
    }
}

bool buildSyntheticStoreCatalogItems(const void* panItem) {
    if (syntheticStoreCatalogItemCount != 0)
        return syntheticStoreCatalogItemCount == localCapes().size();
    if (localCapes().empty() || localCapes().size() > kMaximumLocalCapes)
        return false;
    const auto offsets = findPanStoreStringOffsets(panItem);
    if (!offsets)
        return false;

    std::array<std::byte, target::kStoreCatalogItemSize> panSnapshot{};
    if (!copyReadableMemory(panItem, panSnapshot))
        return false;

    std::size_t built = 0;
    for (const auto& cape : localCapes()) {
        auto& item = syntheticStoreCatalogItems[built];
        item.bytes = panSnapshot;
        replaceStoreStrings(
                item, offsets->productIds, cape.descriptor.pieceId);
        replaceStoreStrings(
                item, offsets->pieceIds, cape.descriptor.pieceId);
        replaceStoreStrings(item, offsets->titles, cape.descriptor.title);
        if (!exactStoreCatalogVtable(item.bytes.data()))
            return false;
        const auto key = storeCatalogItemKey(item.bytes.data());
        if (!key || *key != cape.descriptor.pieceId)
            return false;
        retainedSyntheticStoreCatalogItems[built] = std::shared_ptr<void>(
                item.bytes.data(), [](void*) noexcept {});
        ++built;
    }
    syntheticStoreCatalogItemCount = built;
    return built == localCapes().size();
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

std::optional<std::size_t> vectorElementCount(
        const NativeVector& vector, std::size_t elementSize,
        std::size_t maximumElements) {
    const auto begin = reinterpret_cast<std::uintptr_t>(vector.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(vector.end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(vector.capacity);
    if (begin == 0 || end == 0 || capacity == 0) {
        return begin == 0 && end == 0 && capacity == 0
                ? std::optional<std::size_t>(0)
                : std::nullopt;
    }
    if (end < begin || capacity < end || elementSize == 0)
        return std::nullopt;
    const auto bytes = end - begin;
    const auto capacityBytes = capacity - begin;
    if (bytes % elementSize != 0 || capacityBytes % elementSize != 0)
        return std::nullopt;
    const auto count = bytes / elementSize;
    const auto capacityCount = capacityBytes / elementSize;
    if (count > maximumElements || capacityCount > maximumElements * 4)
        return std::nullopt;
    return count;
}

std::optional<NativeVector> capeIndex(void* manager) {
    if (manager == nullptr)
        return std::nullopt;
    const auto vectorArray = readMemory<std::uintptr_t>(
            static_cast<const std::byte*>(manager) +
            target::kPersonaManagerPiecesByTypeOffset);
    if (!vectorArray || *vectorArray == 0)
        return std::nullopt;
    return readMemory<NativeVector>(reinterpret_cast<const void*>(
            *vectorArray + static_cast<std::uintptr_t>(
                    target::kPersonaCapePieceType) * sizeof(NativeVector)));
}

std::size_t indexedLocalCapeCount(void* manager) {
    const auto index = capeIndex(manager);
    const auto count = index
            ? vectorElementCount(
                      *index, kAndroidStringSize, kMaximumCapeIndexPieces)
            : std::nullopt;
    if (!count)
        return 0;

    std::size_t matches = 0;
    for (const auto& cape : localCapes()) {
        bool found = false;
        for (std::size_t indexPosition = 0;
             indexPosition < *count && !found; ++indexPosition) {
            const auto id = readAndroidString(
                    static_cast<const std::byte*>(index->begin) +
                    indexPosition * kAndroidStringSize,
                    36);
            found = id && *id == cape.descriptor.pieceId;
        }
        matches += found ? 1U : 0U;
    }
    return matches;
}

bool exactPieceIdentity(const void* piece, std::string_view expectedId) {
    if (piece == nullptr) {
        return false;
    }
    const auto id = readAndroidString(
            static_cast<const std::byte*>(piece) +
            target::kPersonaPieceIdOffset,
            36);
    const auto pieceType = readMemory<std::int32_t>(
            static_cast<const std::byte*>(piece) +
            target::kPersonaPieceTypeOffset);
    return id && *id == expectedId && pieceType &&
            *pieceType == target::kPersonaCapePieceType;
}

bool exactPanTemplate(const void* piece) {
    // Lookup already addressed the object by the exact Pan piece ID. Pan is a
    // built-in special case whose pack identity is not valid for normal owned
    // persona entries, so insertion deliberately replaces that identity.
    return exactPieceIdentity(piece, kPanCapeId);
}

struct NativePieceAudit {
    std::size_t pieceUuidCount{};
    std::size_t packUuidCount{};
    std::size_t validCount{};
    std::size_t displayNameCount{};
};

NativePieceAudit auditLocalPieceFields(void* manager) {
    NativePieceAudit audit;
    if (managerLookup == nullptr || isValidPersonaPiece == nullptr)
        return audit;
    const auto packUuid = parseUuid(kPersonaPackId);
    if (!packUuid)
        return audit;

    for (const auto& cape : localCapes()) {
        const void* piece = managerLookup(manager, &cape.descriptor.pieceId);
        if (piece == nullptr)
            continue;
        const auto expectedPieceUuid = parseUuid(cape.descriptor.pieceId);
        const auto actualPieceUuid = readMemory<NativeUuid>(
                static_cast<const std::byte*>(piece) +
                target::kPersonaPieceUuidOffset);
        const auto actualPackUuid = readMemory<NativeUuid>(
                static_cast<const std::byte*>(piece) +
                target::kPersonaPiecePackUuidOffset);
        audit.pieceUuidCount += expectedPieceUuid && actualPieceUuid &&
                        *expectedPieceUuid == *actualPieceUuid
                ? 1U
                : 0U;
        audit.packUuidCount += actualPackUuid && *packUuid == *actualPackUuid
                ? 1U
                : 0U;
        audit.validCount += piece != nullptr && isValidPersonaPiece(piece)
                ? 1U
                : 0U;
        const auto hasDisplayName = readMemory<std::uint8_t>(
                static_cast<const std::byte*>(piece) +
                target::kPersonaPieceHasDisplayNameOffset);
        const auto displayName = hasDisplayName && *hasDisplayName != 0U
                ? readAndroidString(
                          static_cast<const std::byte*>(piece) +
                          target::kPersonaPieceDisplayNameOffset)
                : std::nullopt;
        audit.displayNameCount += displayName &&
                        *displayName == cape.descriptor.title
                ? 1U
                : 0U;
    }
    return audit;
}

void logAcceptanceFailure(std::string detail) {
    if (acceptanceFailureLogged.exchange(true, std::memory_order_acq_rel))
        return;
    logLine("ERROR: local cape acceptance FAIL: " + detail);
    recordLifecycleEvent("local_cape_acceptance_failed", detail);
}

std::size_t lookupLocalCapeCount(void* manager) {
    if (managerLookup == nullptr)
        return 0;
    std::size_t matches = 0;
    for (const auto& cape : localCapes()) {
        matches += exactPieceIdentity(
                           managerLookup(manager, &cape.descriptor.pieceId),
                           cape.descriptor.pieceId)
                ? 1U
                : 0U;
    }
    return matches;
}

struct NativeOwnedAudit {
    std::size_t totalCount{};
    std::size_t localCount{};
    std::size_t validCount{};
    std::size_t capeTypeCount{};
};

std::size_t auditRepositoryOwnedPieces() {
    void* repository = capturedPersonaRepository.load(std::memory_order_acquire);
    if (repository == nullptr || repositoryOwnedPieces == nullptr)
        return 0;
    const std::vector<std::int32_t> pieceTypes{
            target::kPersonaCapePieceType};
    dobbyOwnedQueryActive = true;
    const auto pieces = repositoryOwnedPieces(repository, &pieceTypes);
    dobbyOwnedQueryActive = false;
    if (pieces.size() > kMaximumNativeOwnedPieces)
        return 0;
    std::size_t matches = 0;
    for (const auto& cape : localCapes()) {
        matches += std::any_of(
                pieces.begin(), pieces.end(), [&](const auto& handle) {
                    return exactPieceIdentity(
                            handle.piece, cape.descriptor.pieceId);
                })
                ? 1U
                : 0U;
    }
    return matches;
}

NativeOwnedAudit auditOwnedPieces(void* manager) {
    NativeOwnedAudit audit;
    if (managerOwnedPieces == nullptr)
        return audit;
    const std::vector<std::int32_t> pieceTypes{
            target::kPersonaCapePieceType};
    dobbyOwnedQueryActive = true;
    const auto pieces = managerOwnedPieces(manager, &pieceTypes);
    dobbyOwnedQueryActive = false;
    if (pieces.size() > kMaximumNativeOwnedPieces)
        return audit;
    audit.totalCount = pieces.size();

    for (const auto& handle : pieces) {
        const void* piece = handle.piece;
        if (piece == nullptr)
            continue;
        const auto pieceType = readMemory<std::int32_t>(
                static_cast<const std::byte*>(piece) +
                target::kPersonaPieceTypeOffset);
        audit.capeTypeCount += pieceType &&
                        *pieceType == target::kPersonaCapePieceType
                ? 1U
                : 0U;
        audit.validCount += isValidPersonaPiece != nullptr &&
                        isValidPersonaPiece(piece)
                ? 1U
                : 0U;
    }
    for (const auto& cape : localCapes()) {
        bool found = false;
        for (const auto& handle : pieces) {
            if (exactPieceIdentity(
                        handle.piece, cape.descriptor.pieceId)) {
                found = true;
                break;
            }
        }
        audit.localCount += found ? 1U : 0U;
    }
    return audit;
}

bool insertLocalCape(
        void* manager, const void* panPiece, const LocalCape& cape) {
    if (managerLookup == nullptr || managerInsert == nullptr ||
        copyPersonaPiece == nullptr ||
        destroyPersonaPiece == nullptr) {
        return false;
    }
    if (exactPieceIdentity(
                managerLookup(manager, &cape.descriptor.pieceId),
                cape.descriptor.pieceId)) {
        return true;
    }

    const auto uuid = parseUuid(cape.descriptor.pieceId);
    const auto packUuid = parseUuid(kPersonaPackId);
    if (!uuid || !packUuid)
        return false;

    alignas(8) std::array<std::byte, target::kPersonaPieceSize> temporary{};
    copyPersonaPiece(temporary.data(), panPiece);
    auto* pieceId = reinterpret_cast<std::string*>(
            temporary.data() + target::kPersonaPieceIdOffset);
    *pieceId = cape.descriptor.pieceId;
    auto* displayName = reinterpret_cast<std::string*>(
            temporary.data() + target::kPersonaPieceDisplayNameOffset);
    auto* hasDisplayName = reinterpret_cast<std::uint8_t*>(
            temporary.data() + target::kPersonaPieceHasDisplayNameOffset);
    if (*hasDisplayName != 0U) {
        *displayName = cape.descriptor.title;
    } else {
        std::construct_at(displayName, cape.descriptor.title);
        *hasDisplayName = 1U;
    }
    std::memcpy(
            temporary.data() + target::kPersonaPieceUuidOffset,
            &*uuid,
            sizeof(*uuid));
    std::memcpy(
            temporary.data() + target::kPersonaPiecePackUuidOffset,
            &*packUuid,
            sizeof(*packUuid));

    const bool prepared = exactPieceIdentity(
            temporary.data(), cape.descriptor.pieceId);
    const void* inserted = nullptr;
    if (prepared) {
        inserted = managerInsert(
                manager, &cape.descriptor.pieceId, temporary.data());
    }
    destroyPersonaPiece(temporary.data());
    return prepared &&
            exactPieceIdentity(inserted, cape.descriptor.pieceId);
}

bool auditNativeOwnedPath(void* manager) {
    const auto expected = localCapes().size();
    const auto lookupCount = lookupLocalCapeCount(manager);
    const auto indexCount = indexedLocalCapeCount(manager);
    const auto pieceAudit = auditLocalPieceFields(manager);
    const auto ownedAudit = auditOwnedPieces(manager);
    const auto repositoryOwnedCount = auditRepositoryOwnedPieces();
    const bool passed = expected != 0 && lookupCount == expected &&
            indexCount == expected &&
            pieceAudit.pieceUuidCount == expected &&
            pieceAudit.packUuidCount == expected &&
            pieceAudit.validCount == expected &&
            pieceAudit.displayNameCount == expected &&
            ownedAudit.localCount == expected &&
            repositoryOwnedCount == expected &&
            ownedAudit.capeTypeCount >= expected &&
            ownedAudit.validCount >= expected;
    if (passed) {
        acceptancePassed.store(true, std::memory_order_release);
        const std::string detail =
                "expected=" + std::to_string(expected) +
                ";manager_lookup=" + std::to_string(lookupCount) +
                ";type_index=" + std::to_string(indexCount) +
                ";piece_uuid=" + std::to_string(pieceAudit.pieceUuidCount) +
                ";pack_uuid=" + std::to_string(pieceAudit.packUuidCount) +
                ";native_valid=" + std::to_string(pieceAudit.validCount) +
                ";native_display_name=" +
                std::to_string(pieceAudit.displayNameCount) +
                ";owned_total=" + std::to_string(ownedAudit.totalCount) +
                ";owned_cape_type=" +
                std::to_string(ownedAudit.capeTypeCount) +
                ";owned_valid=" + std::to_string(ownedAudit.validCount) +
                ";owned_result=" + std::to_string(ownedAudit.localCount);
        const std::string completeDetail = detail +
                ";repository_owned_result=" +
                std::to_string(repositoryOwnedCount);
        logLine("local cape acceptance PASS: " + completeDetail);
        recordLifecycleEvent("local_cape_acceptance_passed", completeDetail);
        return true;
    }

    logAcceptanceFailure(
            "expected=" + std::to_string(expected) +
            ";manager_lookup=" + std::to_string(lookupCount) +
            ";type_index=" + std::to_string(indexCount) +
            ";piece_uuid=" + std::to_string(pieceAudit.pieceUuidCount) +
            ";pack_uuid=" + std::to_string(pieceAudit.packUuidCount) +
            ";native_valid=" + std::to_string(pieceAudit.validCount) +
            ";native_display_name=" +
            std::to_string(pieceAudit.displayNameCount) +
            ";owned_total=" + std::to_string(ownedAudit.totalCount) +
            ";owned_cape_type=" + std::to_string(ownedAudit.capeTypeCount) +
            ";owned_valid=" + std::to_string(ownedAudit.validCount) +
            ";owned_result=" + std::to_string(ownedAudit.localCount) +
            ";repository_owned_result=" +
            std::to_string(repositoryOwnedCount));
    return false;
}

bool injectNativeLocalCapes(void* manager, const void* panPiece) {
    std::lock_guard lock(injectionMutex);
    if (acceptancePassed.load(std::memory_order_acquire))
        return true;
    if (!repositoryHookReady.load(std::memory_order_acquire) ||
        !runtimeState().capeTestPackets() || !exactPanTemplate(panPiece)) {
        return false;
    }

    for (const auto& cape : localCapes()) {
        if (!insertLocalCape(manager, panPiece, cape))
            return auditNativeOwnedPath(manager);
    }
    return auditNativeOwnedPath(manager);
}

std::optional<std::vector<std::uintptr_t>> boundedTreeNodes(
        const void* tree, std::size_t maximumNodes) {
    if (tree == nullptr)
        return std::nullopt;
    const auto count = readMemory<std::size_t>(
            static_cast<const std::byte*>(tree) + 0x10);
    const auto root = readMemory<std::uintptr_t>(
            static_cast<const std::byte*>(tree) + 0x08);
    if (!count || !root || *count > maximumNodes)
        return std::nullopt;
    if (*count == 0)
        return *root == 0
                ? std::optional<std::vector<std::uintptr_t>>(
                          std::vector<std::uintptr_t>{})
                : std::nullopt;
    if (*root == 0)
        return std::nullopt;

    std::vector<std::uintptr_t> pending{*root};
    std::vector<std::uintptr_t> visited;
    visited.reserve(*count);
    while (!pending.empty() && visited.size() <= *count) {
        const auto node = pending.back();
        pending.pop_back();
        if (node == 0)
            continue;
        if (std::find(visited.begin(), visited.end(), node) != visited.end())
            return std::nullopt;
        const auto links = readMemory<NativeTreeNodeLinks>(
                reinterpret_cast<const void*>(node));
        if (!links)
            return std::nullopt;
        visited.push_back(node);
        if (links->left != 0)
            pending.push_back(links->left);
        if (links->right != 0)
            pending.push_back(links->right);
    }
    if (visited.size() != *count)
        return std::nullopt;
    return visited;
}

bool treeContainsString(const void* tree, std::string_view expected) {
    const auto nodes = boundedTreeNodes(tree, kMaximumOfflineCatalogItems);
    if (!nodes)
        return false;
    return std::any_of(nodes->begin(), nodes->end(), [&](const auto node) {
        const auto key = readAndroidString(
                reinterpret_cast<const std::byte*>(node) + 0x20, 128);
        return key && *key == expected;
    });
}

std::optional<std::string> offlineCatalogPlayerId(void* offlineManager) {
    auto playerId = readAndroidString(
            static_cast<const std::byte*>(offlineManager) + 0x30, 128);
    return playerId && !playerId->empty() ? playerId : std::nullopt;
}

const NativePersonaOfflineCatalogItem* lookupOfflineCatalogItem(
        void* offlineManager, const std::string& productId) {
    if (offlineCatalogItemLookup == nullptr)
        return nullptr;
    void* node = offlineCatalogItemLookup(offlineManager, &productId);
    if (node == static_cast<std::byte*>(offlineManager) + 0x08)
        return nullptr;
    const auto object = readMemory<std::uintptr_t>(
            static_cast<const std::byte*>(node) + 0x38);
    return object && *object != 0
            ? reinterpret_cast<const NativePersonaOfflineCatalogItem*>(*object)
            : nullptr;
}

bool exactOfflineCatalogItem(
        const NativePersonaOfflineCatalogItem* item, const LocalCape& cape) {
    if (item == nullptr)
        return false;
    const auto* bytes = reinterpret_cast<const std::byte*>(item);
    const auto productId = readAndroidString(bytes + 0x00, 128);
    const auto catalogId = readAndroidString(bytes + 0x18, 128);
    const auto personaPieceId = readAndroidString(bytes + 0x30, 128);
    const auto title = readAndroidString(bytes + 0x68, 128);
    return productId && *productId == cape.descriptor.pieceId && catalogId &&
            *catalogId == cape.descriptor.pieceId && personaPieceId &&
            *personaPieceId == cape.descriptor.pieceId && title &&
            *title == cape.descriptor.title;
}

bool insertOfflineCatalogItem(
        void* offlineManager, void* playerSet, const LocalCape& cape) {
    const std::string productId(cape.descriptor.pieceId);
    auto* existing = lookupOfflineCatalogItem(offlineManager, productId);
    if (existing != nullptr && !exactOfflineCatalogItem(existing, cape))
        return false;
    if (existing == nullptr) {
        auto item = std::make_shared<NativePersonaOfflineCatalogItem>(
                NativePersonaOfflineCatalogItem{
                        cape.descriptor.pieceId,
                        cape.descriptor.pieceId,
                        cape.descriptor.pieceId,
                        true,
                        0,
                        "persona_capes",
                        cape.descriptor.title});

        void* node = offlineCatalogItemInsert(
                offlineManager, &productId, &productId);
        if (node == nullptr ||
            node == static_cast<std::byte*>(offlineManager) + 0x08)
            return false;
        auto* slot = reinterpret_cast<
                std::shared_ptr<NativePersonaOfflineCatalogItem>*>(
                static_cast<std::byte*>(node) + 0x38);
        *slot = item;
        retainedOfflineCatalogItems.push_back(std::move(item));
    }

    offlineCatalogIdSetInsert(playerSet, &productId, &productId);
    const std::uint8_t dirty = 1;
    std::memcpy(
            static_cast<std::byte*>(offlineManager) + 0x52,
            &dirty,
            sizeof(dirty));
    return true;
}

bool auditOfflineCatalog(
        void* repository, void* offlineManager,
        const std::string& playerId, void* playerSet) {
    if (repositoryOfflinePieces == nullptr)
        return false;
    const auto expected = localCapes().size();
    // The public offline query is itself hooked so UI calls are populated
    // before result construction. Internal acceptance audits must bypass that
    // preparation callback while retaining the original query implementation;
    // otherwise injectOfflineCatalog recursively tries to lock its mutex.
    const bool previousOfflineQueryActive = dobbyOfflineQueryActive;
    dobbyOfflineQueryActive = true;
    const auto pickerPieces = repositoryOfflinePieces(repository, &playerId);
    dobbyOfflineQueryActive = previousOfflineQueryActive;
    if (pickerPieces.size() > kMaximumOfflineCatalogItems)
        return false;

    std::size_t itemMapCount = 0;
    std::size_t playerSetCount = 0;
    std::size_t pickerResultCount = 0;
    for (const auto& cape : localCapes()) {
        itemMapCount += exactOfflineCatalogItem(
                                lookupOfflineCatalogItem(
                                        offlineManager,
                                        cape.descriptor.pieceId),
                                cape)
                ? 1U
                : 0U;
        playerSetCount += treeContainsString(
                                  playerSet, cape.descriptor.pieceId)
                ? 1U
                : 0U;
        pickerResultCount += std::find(
                                     pickerPieces.begin(), pickerPieces.end(),
                                     cape.descriptor.pieceId) !=
                        pickerPieces.end()
                ? 1U
                : 0U;
    }
    offlineCatalogItemAuditCount.store(itemMapCount, std::memory_order_release);
    offlineCatalogPlayerAuditCount.store(
            playerSetCount, std::memory_order_release);
    offlineCatalogConsumerAuditCount.store(
            pickerResultCount, std::memory_order_release);
    offlineCatalogConsumerTotal.store(
            pickerPieces.size(), std::memory_order_release);
    offlineCatalogStage.store(5, std::memory_order_release);
    const bool passed = expected != 0 && itemMapCount == expected &&
            playerSetCount == expected && pickerResultCount == expected;
    if (!passed)
        return false;

    offlineCatalogAcceptancePassed.store(true, std::memory_order_release);
    const std::string detail =
            "expected=" + std::to_string(expected) +
            ";offline_item_map=" + std::to_string(itemMapCount) +
            ";player_id_set=" + std::to_string(playerSetCount) +
            ";picker_consumer_result=" + std::to_string(pickerResultCount) +
            ";picker_consumer_total=" + std::to_string(pickerPieces.size());
    logLine("local cape offline ID enumeration PASS: " + detail);
    recordLifecycleEvent(
            "local_cape_offline_id_enumeration_passed", detail);
    return true;
}

bool injectOfflineCatalog(void* repository) {
    std::lock_guard lock(offlineCatalogMutex);
    if (offlineCatalogAcceptancePassed.load(std::memory_order_acquire))
        return true;
    if (repository == nullptr || offlineCatalogItemInsert == nullptr ||
        offlineCatalogPlayerInsert == nullptr ||
        offlineCatalogPlayerLookup == nullptr ||
        offlineCatalogItemLookup == nullptr ||
        offlineCatalogIdSetInsert == nullptr ||
        repositoryOfflinePieces == nullptr) {
        return false;
    }
    offlineCatalogStage.store(1, std::memory_order_release);
    const auto offlineManager = readMemory<std::uintptr_t>(
            static_cast<const std::byte*>(repository) +
            target::kPersonaRepositoryOfflineManagerOffset);
    if (!offlineManager || *offlineManager == 0)
        return false;
    auto* manager = reinterpret_cast<void*>(*offlineManager);
    offlineCatalogStage.store(2, std::memory_order_release);
    const auto playerId = offlineCatalogPlayerId(manager);
    if (!playerId)
        return false;
    offlineCatalogStage.store(3, std::memory_order_release);

    void* playerTree = static_cast<std::byte*>(manager) + 0x18;
    void* playerNode = offlineCatalogPlayerInsert(
            playerTree, &*playerId, &*playerId);
    if (playerNode == nullptr ||
        playerNode == static_cast<std::byte*>(playerTree) + 0x08) {
        return false;
    }
    void* playerSet = static_cast<std::byte*>(playerNode) + 0x38;
    for (const auto& cape : localCapes()) {
        if (!insertOfflineCatalogItem(manager, playerSet, cape)) {
            return false;
        }
    }
    offlineCatalogStage.store(4, std::memory_order_release);
    return auditOfflineCatalog(repository, manager, *playerId, playerSet);
}

bool validateFunction(
        const MinecraftImage& image, std::uintptr_t offset,
        const auto& signature) {
    const auto address = image.base + offset;
    return addressIsExecutable(image, address) &&
            matchesSignature(reinterpret_cast<const void*>(address), signature);
}

void* resolveLivePersonaManager(const MinecraftImage& image) {
    if (image.base == 0)
        return nullptr;

    const auto singletonAddress =
            image.base + target::kMinecraftGameSingletonPointerOffset;
    if (!addressIsInImage(image, singletonAddress))
        return nullptr;
    const auto minecraftGame = readMemory<std::uintptr_t>(
            reinterpret_cast<const void*>(singletonAddress));
    if (!minecraftGame || *minecraftGame == 0)
        return nullptr;
    const auto gameVtable = readMemory<std::uintptr_t>(
            reinterpret_cast<const void*>(*minecraftGame));
    if (!gameVtable ||
        *gameVtable != image.base + target::kMinecraftGameVtableOffset) {
        return nullptr;
    }

    const auto repository = readMemory<std::uintptr_t>(
            reinterpret_cast<const std::byte*>(*minecraftGame) +
            target::kMinecraftGamePersonaRepositoryOffset);
    if (!repository || *repository == 0)
        return nullptr;
    const auto repositoryVtable = readMemory<std::uintptr_t>(
            reinterpret_cast<const void*>(*repository));
    if (!repositoryVtable ||
        *repositoryVtable !=
                image.base + target::kPersonaRepositoryVtableOffset) {
        return nullptr;
    }
    capturedPersonaRepository.store(
            reinterpret_cast<void*>(*repository), std::memory_order_release);

    const auto manager = readMemory<std::uintptr_t>(
            reinterpret_cast<const std::byte*>(*repository) +
            target::kPersonaRepositoryManagerOffset);
    if (!manager || *manager == 0 || !capeIndex(reinterpret_cast<void*>(*manager)))
        return nullptr;
    return reinterpret_cast<void*>(*manager);
}

bool injectFromLiveRepository(
        const MinecraftImage& image, bool reportMissingManager) {
    void* manager = __atomic_load_n(
            &dobby_captured_persona_manager, __ATOMIC_ACQUIRE);
    // Resolve even after an early query captured the manager: the UI-facing
    // repository pointer is a separate acceptance boundary populated by this
    // exact singleton chain.
    if (void* resolvedManager = resolveLivePersonaManager(image))
        manager = resolvedManager;
    if (manager == nullptr || managerLookup == nullptr) {
        if (reportMissingManager)
            logAcceptanceFailure("live PersonaPieceManager chain rejected");
        return false;
    }
    capturedPersonaManager.store(manager, std::memory_order_release);
    static const std::string panCapeId(kPanCapeId);
    const void* panPiece = managerLookup(manager, &panCapeId);
    if (!exactPanTemplate(panPiece)) {
        if (reportMissingManager)
            logAcceptanceFailure("exact native Pan Cape template rejected");
        return false;
    }
    const bool managerAccepted = injectNativeLocalCapes(manager, panPiece);
    void* repository = capturedPersonaRepository.load(
            std::memory_order_acquire);
    const bool pickerAccepted = injectOfflineCatalog(repository);
    return managerAccepted && pickerAccepted;
}

void retryLiveRepositoryInjection(void*, void*, void*) {
    if (acceptancePassed.load(std::memory_order_acquire) &&
        offlineCatalogAcceptancePassed.load(std::memory_order_acquire)) {
        liveRepositoryRetryFinished.store(true, std::memory_order_release);
        return;
    }
    if (liveRepositoryRetryFinished.load(std::memory_order_acquire)) {
        return;
    }

    constexpr std::uint32_t kRetryEveryFrames = 6;
    constexpr std::uint32_t kMaximumRetryFrames = 3600;
    const auto frame = liveRepositoryRetryFrames.fetch_add(
                               1, std::memory_order_acq_rel) +
            1;
    if (frame % kRetryEveryFrames != 0 && frame < kMaximumRetryFrames)
        return;

    if (injectFromLiveRepository(findMinecraftImage(), false)) {
        liveRepositoryRetryFinished.store(true, std::memory_order_release);
        return;
    }
    if (frame >= kMaximumRetryFrames) {
        const std::string detail =
                "manager_accepted=" +
                std::to_string(
                        acceptancePassed.load(std::memory_order_acquire)) +
                ";picker_catalog_accepted=" +
                std::to_string(offlineCatalogAcceptancePassed.load(
                        std::memory_order_acquire)) +
                ";stage=" + std::to_string(offlineCatalogStage.load(
                        std::memory_order_acquire)) +
                ";offline_item_map=" +
                std::to_string(offlineCatalogItemAuditCount.load(
                        std::memory_order_acquire)) +
                ";player_id_set=" +
                std::to_string(offlineCatalogPlayerAuditCount.load(
                        std::memory_order_acquire)) +
                ";picker_consumer_result=" +
                std::to_string(offlineCatalogConsumerAuditCount.load(
                        std::memory_order_acquire)) +
                ";picker_consumer_total=" +
                std::to_string(offlineCatalogConsumerTotal.load(
                        std::memory_order_acquire)) +
                ";bounded startup retry exhausted";
        if (!offlineCatalogFailureLogged.exchange(
                    true, std::memory_order_acq_rel)) {
            logLine("ERROR: local cape offline ID enumeration FAIL: " + detail);
            recordLifecycleEvent(
                    "local_cape_offline_id_enumeration_failed", detail);
        }
        liveRepositoryRetryFinished.store(true, std::memory_order_release);
    }
}

std::vector<const void*> sourceCatalogItems(
        const void* sourceVector, std::size_t count) {
    std::vector<const void*> items;
    items.reserve(count);
    const auto native = readMemory<NativeVector>(sourceVector);
    if (!native)
        return {};
    for (std::size_t index = 0; index < count; ++index) {
        const auto item = readMemory<std::uintptr_t>(
                static_cast<const std::byte*>(native->begin) +
                index * sizeof(void*));
        if (!item)
            return {};
        items.push_back(reinterpret_cast<const void*>(*item));
    }
    return items;
}

std::size_t localStoreCatalogItemCount(
        const std::vector<const void*>& items) {
    std::size_t matches = 0;
    for (const auto& cape : localCapes()) {
        const bool found = std::any_of(
                items.begin(), items.end(), [&](const void* item) {
                    const auto key = storeCatalogItemKey(item);
                    return key && *key == cape.descriptor.pieceId;
                });
        matches += found ? 1U : 0U;
    }
    return matches;
}

bool expandOfferCollectionSources(void* component, void* sourceVector) {
    if (component == nullptr || sourceVector == nullptr ||
        expectedOfferCollectionVtable == 0 ||
        !runtimeState().capeTestPackets()) {
        return false;
    }
    const auto componentVtable = readMemory<std::uintptr_t>(component);
    if (!componentVtable || *componentVtable != expectedOfferCollectionVtable)
        return false;

    std::lock_guard lock(storeCatalogMutex);
    const auto native = readMemory<NativeVector>(sourceVector);
    const auto beforeCount = native
            ? vectorElementCount(
                      *native, sizeof(void*), kMaximumDiscoveredCatalogItems)
            : std::nullopt;
    if (!beforeCount)
        return false;
    auto before = sourceCatalogItems(sourceVector, *beforeCount);
    if (before.size() != *beforeCount)
        return false;

    const auto existingLocalCount = localStoreCatalogItemCount(before);
    const auto expected = localCapes().size();
    if (expected != 0 && existingLocalCount == expected)
        return true;

    const auto pan = std::find_if(
            before.begin(), before.end(), exactPanStoreCatalogItem);
    if (pan == before.end())
        return false;
    panStoreItemObserved.store(true, std::memory_order_release);
    if (!buildSyntheticStoreCatalogItems(*pan))
        return false;

    if (*beforeCount + expected > kMaximumDiscoveredCatalogItems)
        return false;
    auto* items = reinterpret_cast<std::vector<void*>*>(sourceVector);
    items->reserve(*beforeCount + expected);
    for (std::size_t index = 0; index < expected; ++index) {
        const auto& cape = localCapes()[index];
        const bool alreadyPresent = std::any_of(
                before.begin(), before.end(), [&](const void* item) {
                    const auto key = storeCatalogItemKey(item);
                    return key && *key == cape.descriptor.pieceId;
                });
        if (!alreadyPresent)
            items->push_back(syntheticStoreCatalogItems[index].bytes.data());
    }

    const auto afterLayout = readMemory<NativeVector>(sourceVector);
    const auto afterCount = afterLayout
            ? vectorElementCount(
                      *afterLayout, sizeof(void*),
                      kMaximumDiscoveredCatalogItems)
            : std::nullopt;
    if (!afterCount)
        return false;
    const auto after = sourceCatalogItems(sourceVector, *afterCount);
    const auto localCount = after.size() == *afterCount
            ? localStoreCatalogItemCount(after)
            : 0;
    const bool passed = expected != 0 && localCount == expected;
    if (!passed)
        return false;

    capturedOfferCollection.store(component, std::memory_order_release);
    if (!offerCollectionObserved.exchange(true, std::memory_order_acq_rel)) {
        logLine("local cape exact OfferCollectionComponent observed");
        recordLifecycleEvent(
                "local_cape_offer_collection_observed", "vtable=exact");
    }
    if (!sourceExpansionLogged.exchange(
                true, std::memory_order_acq_rel)) {
        const std::string detail =
                "expected=" + std::to_string(expected) +
                ";source_before=" + std::to_string(*beforeCount) +
                ";source_after=" + std::to_string(*afterCount) +
                ";local_store_items=" + std::to_string(localCount) +
                ";pan_template=exact";
        logLine("local cape offer sources expanded: " + detail);
        recordLifecycleEvent(
                "local_cape_offer_sources_expanded", detail);
    }
    return true;
}

struct LiveOfferCollectionState {
    void* component{};
    const void* panItem{};
    std::size_t renderedCount{};
    std::uint32_t offerLimit{};
};

std::optional<LiveOfferCollectionState> exactLiveOfferCollection(
        void* component) {
    const auto componentVtable = readMemory<std::uintptr_t>(component);
    if (!componentVtable || *componentVtable != expectedOfferCollectionVtable)
        return std::nullopt;
    const auto entries = readMemory<NativeVector>(
            static_cast<const std::byte*>(component) +
            target::kOfferCollectionRenderedEntriesOffset);
    const auto count = entries
            ? vectorElementCount(
                      *entries, sizeof(void*), kMaximumDiscoveredCatalogItems)
            : std::nullopt;
    const auto limit = readMemory<std::uint32_t>(
            static_cast<const std::byte*>(component) + 0xbc);
    if (!count || *count == 0 || !limit ||
        *limit > kMaximumDiscoveredCatalogItems) {
        return std::nullopt;
    }

    const void* panItem = nullptr;
    for (std::size_t index = 0; index < *count; ++index) {
        const auto wrapper = readMemory<std::uintptr_t>(
                static_cast<const std::byte*>(entries->begin) +
                index * sizeof(void*));
        const auto item = wrapper && *wrapper != 0
                ? readMemory<std::uintptr_t>(
                          reinterpret_cast<const void*>(*wrapper))
                : std::nullopt;
        if (!item || *item == 0)
            return std::nullopt;
        const void* catalogItem = reinterpret_cast<const void*>(*item);
        if (exactPanStoreCatalogItem(catalogItem))
            panItem = catalogItem;
    }
    if (panItem == nullptr)
        return std::nullopt;
    return LiveOfferCollectionState{
            component, panItem, *count, *limit};
}

std::optional<LiveOfferCollectionState> discoverLiveOfferCollection() {
    constexpr std::size_t kMaximumVtableMatches = 256;
    constexpr std::size_t kMaximumScanBytes = 512U * 1024U * 1024U;
    const auto matches = findWritablePointerMatches(
            expectedOfferCollectionVtable, kMaximumVtableMatches,
            kMaximumScanBytes);
    std::vector<LiveOfferCollectionState> candidates;
    for (const auto address : matches) {
        const auto state = exactLiveOfferCollection(
                reinterpret_cast<void*>(address));
        if (state)
            candidates.push_back(*state);
    }
    if (candidates.size() != 1) {
        if (!liveOfferScanLogged.exchange(true, std::memory_order_acq_rel)) {
            const std::string detail =
                    "vtable_matches=" + std::to_string(matches.size()) +
                    ";exact_pan_components=" +
                    std::to_string(candidates.size());
            logLine("local cape live offer scan pending: " + detail);
            recordLifecycleEvent("local_cape_live_offer_scan_pending", detail);
        }
        return std::nullopt;
    }
    return candidates.front();
}

bool injectVisibleOffers(const LiveOfferCollectionState& state) {
    if (offerCollectionReplace == nullptr || mcpelauncher_patch == nullptr ||
        forcedVisibleOfferInjectionAttempted.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard lock(storeCatalogMutex);
    if (!buildSyntheticStoreCatalogItems(state.panItem))
        return false;
    const auto expected = localCapes().size();
    if (expected == 0 ||
        state.renderedCount + expected > kMaximumDiscoveredCatalogItems) {
        return false;
    }

    const auto requiredLimit = static_cast<std::uint32_t>(
            state.renderedCount + expected);
    auto newLimit = std::max(state.offerLimit, requiredLimit);
    auto* limitAddress = static_cast<std::byte*>(state.component) + 0xbc;
    if (mcpelauncher_patch(
                limitAddress, &newLimit,
                sizeof(newLimit)) == nullptr) {
        return false;
    }
    const auto verifiedLimit = readMemory<std::uint32_t>(limitAddress);
    if (!verifiedLimit || *verifiedLimit != newLimit)
        return false;

    std::vector<void*> localItems;
    localItems.reserve(expected);
    for (std::size_t index = 0; index < expected; ++index)
        localItems.push_back(syntheticStoreCatalogItems[index].bytes.data());
    forcedVisibleOfferInjectionAttempted.store(true, std::memory_order_release);
    dobbyOfferCollectionActive = true;
    offerCollectionReplace(state.component, &localItems);
    dobbyOfferCollectionActive = false;

    const auto after = readMemory<NativeVector>(
            static_cast<const std::byte*>(state.component) +
            target::kOfferCollectionRenderedEntriesOffset);
    const auto afterCount = after
            ? vectorElementCount(
                      *after, sizeof(void*), kMaximumDiscoveredCatalogItems)
            : std::nullopt;
    const std::string detail =
            "expected=" + std::to_string(expected) +
            ";rendered_before=" + std::to_string(state.renderedCount) +
            ";rendered_after=" +
            std::to_string(afterCount.value_or(0)) +
            ";limit_before=" + std::to_string(state.offerLimit) +
            ";limit_after=" + std::to_string(newLimit) +
            ";pan_template=exact";
    logLine("local cape live visible-offer injection attempted: " + detail);
    recordLifecycleEvent(
            "local_cape_live_visible_offer_injection_attempted", detail);
    return afterCount && *afterCount == state.renderedCount + expected;
}

void auditVisiblePieceOffers(void*, void*, void*) {
    if (expectedOfferCollectionVtable == 0)
        return;
    auto* component = capturedOfferCollection.load(std::memory_order_acquire);
    if (component == nullptr &&
        acceptancePassed.load(std::memory_order_acquire) &&
        offlineCatalogAcceptancePassed.load(std::memory_order_acquire)) {
        const auto frame = liveOfferScanFrames.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        if (frame >= 60 && (frame == 60 || frame % 120 == 0)) {
            const auto discovered = discoverLiveOfferCollection();
            if (discovered) {
                component = discovered->component;
                capturedOfferCollection.store(
                        component, std::memory_order_release);
                offerCollectionObserved.store(true, std::memory_order_release);
                logLine(
                        "local cape exact live OfferCollectionComponent discovered from rendered Pan");
                recordLifecycleEvent(
                        "local_cape_live_offer_collection_discovered",
                        "vtable=exact;pan=exact");
                injectVisibleOffers(*discovered);
            }
        }
    }
    if (component == nullptr)
        return;
    const auto componentVtable = readMemory<std::uintptr_t>(component);
    if (!componentVtable || *componentVtable != expectedOfferCollectionVtable) {
        capturedOfferCollection.store(nullptr, std::memory_order_release);
        return;
    }
    const auto entries = readMemory<NativeVector>(
            static_cast<const std::byte*>(component) +
            target::kOfferCollectionRenderedEntriesOffset);
    const auto count = entries
            ? vectorElementCount(
                      *entries, sizeof(void*), kMaximumDiscoveredCatalogItems)
            : std::nullopt;
    if (!count)
        return;
    std::vector<const void*> renderedItems;
    renderedItems.reserve(*count);
    for (std::size_t index = 0; index < *count; ++index) {
        const auto entry = readMemory<std::uintptr_t>(
                static_cast<const std::byte*>(entries->begin) +
                index * sizeof(void*));
        if (!entry || *entry == 0)
            return;
        const auto item = readMemory<std::uintptr_t>(
                reinterpret_cast<const void*>(*entry));
        if (!item || *item == 0)
            return;
        renderedItems.push_back(reinterpret_cast<const void*>(*item));
    }
    const auto localCount = localStoreCatalogItemCount(renderedItems);
    const auto previous = renderedOfferAuditCount.exchange(
            *count, std::memory_order_acq_rel);
    const auto previousLocal = localRenderedOfferAuditCount.exchange(
            localCount, std::memory_order_acq_rel);
    const auto expected = localCapes().size();
    if (expected != 0 && localCount == expected &&
        !visibleOfferAcceptancePassed.exchange(
                true, std::memory_order_acq_rel)) {
        const std::string detail =
                "expected=" + std::to_string(expected) +
                ";exact_local_ids=" + std::to_string(localCount) +
                ";rendered_offers=" + std::to_string(*count);
        logLine("local cape visible offer acceptance PASS: " + detail);
        recordLifecycleEvent("local_cape_visible_offer_acceptance_pass", detail);
        return;
    }
    if (*count == previous && localCount == previousLocal)
        return;
    const std::string detail =
            "rendered_offers=" + std::to_string(*count) +
            ";previous_rendered_offers=" + std::to_string(previous) +
            ";exact_local_ids=" + std::to_string(localCount) +
            ";exact_ids_pending=" +
            std::string(localCount == expected ? "false" : "true");
    logLine("local cape rendered offer observation: " + detail);
    recordLifecycleEvent("local_cape_rendered_offer_observed", detail);
}

bool patchOwnedCapture(std::uintptr_t entry) {
    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(
            dobby_persona_manager_owned_capture_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));
    auto* address = reinterpret_cast<void*>(entry);
    return mcpelauncher_patch != nullptr &&
            mcpelauncher_patch(
                    address, replacement.data(), replacement.size()) != nullptr &&
            std::memcmp(address, replacement.data(), replacement.size()) == 0;
}

bool patchOfflineQuery(std::uintptr_t entry) {
    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(
            dobby_persona_offline_query_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));
    auto* address = reinterpret_cast<void*>(entry);
    return mcpelauncher_patch != nullptr &&
            mcpelauncher_patch(
                    address, replacement.data(), replacement.size()) != nullptr &&
            std::memcmp(address, replacement.data(), replacement.size()) == 0;
}

bool patchOfferCollectionReplace(std::uintptr_t entry) {
    std::array<std::uint8_t, 16> replacement{};
    constexpr std::uint32_t loadTarget = 0x58000050U;
    constexpr std::uint32_t branchTarget = 0xd61f0200U;
    const auto detour = reinterpret_cast<std::uintptr_t>(
            dobby_offer_collection_replace_detour);
    std::memcpy(replacement.data(), &loadTarget, sizeof(loadTarget));
    std::memcpy(replacement.data() + 4, &branchTarget, sizeof(branchTarget));
    std::memcpy(replacement.data() + 8, &detour, sizeof(detour));
    auto* address = reinterpret_cast<void*>(entry);
    return mcpelauncher_patch != nullptr &&
            mcpelauncher_patch(
                    address, replacement.data(), replacement.size()) != nullptr &&
            std::memcmp(address, replacement.data(), replacement.size()) == 0;
}

} // namespace

extern "C" void dobby_prepare_persona_owned_query(
        void* manager, const void* pieceTypes) {
    if (dobbyOwnedQueryActive || manager == nullptr || pieceTypes == nullptr ||
        !runtimeState().capeTestPackets()) {
        return;
    }

    const auto nativeTypes = readMemory<NativeVector>(pieceTypes);
    const auto typeCount = nativeTypes
            ? vectorElementCount(*nativeTypes, sizeof(std::int32_t), 32)
            : std::nullopt;
    if (!typeCount)
        return;

    bool requestsCape = false;
    for (std::size_t index = 0; index < *typeCount; ++index) {
        const auto pieceType = readMemory<std::int32_t>(
                static_cast<const std::byte*>(nativeTypes->begin) +
                index * sizeof(std::int32_t));
        if (pieceType && *pieceType == target::kPersonaCapePieceType) {
            requestsCape = true;
            break;
        }
    }
    if (!requestsCape)
        return;

    capturedPersonaManager.store(manager, std::memory_order_release);
    if (!acceptancePassed.load(std::memory_order_acquire) &&
        managerLookup != nullptr) {
        static const std::string panCapeId(kPanCapeId);
        injectNativeLocalCapes(
                manager, managerLookup(manager, &panCapeId));
    }

    if (acceptancePassed.load(std::memory_order_acquire) &&
        !pickerQueryObserved.exchange(true, std::memory_order_acq_rel)) {
        const std::string detail =
                "native cape query entered after " +
                std::to_string(localCapes().size()) +
                " local entries were accepted";
        logLine("local cape picker query prepared: " + detail);
        recordLifecycleEvent("local_cape_picker_query_prepared", detail);
    }
}

extern "C" void dobby_prepare_persona_offline_query(void* repository) {
    if (dobbyOfflineQueryActive || repository == nullptr ||
        !runtimeState().capeTestPackets()) {
        return;
    }
    dobbyOfflineQueryActive = true;
    capturedPersonaRepository.store(repository, std::memory_order_release);
    const bool injected = injectOfflineCatalog(repository);
    if (!offlineQueryObserved.exchange(true, std::memory_order_acq_rel)) {
        const std::string detail =
                "injected_before_query=" +
                std::string(injected ? "true" : "false") +
                ";expected=" + std::to_string(localCapes().size()) +
                ";exact_public_query_entry=true";
        logLine("local cape offline picker query observed: " + detail);
        recordLifecycleEvent("local_cape_offline_picker_query_observed", detail);
    }
    dobbyOfflineQueryActive = false;
}

extern "C" void dobby_prepare_offer_collection(
        void* component, void* storeItems) {
    if (dobbyOfferCollectionActive)
        return;
    dobbyOfferCollectionActive = true;
    if (!offerReplaceEntryObserved.exchange(true, std::memory_order_acq_rel)) {
        const bool enabled = runtimeState().capeTestPackets();
        const auto vtable = component == nullptr
                ? std::nullopt
                : readMemory<std::uintptr_t>(component);
        const bool exactComponent = vtable &&
                expectedOfferCollectionVtable != 0 &&
                *vtable == expectedOfferCollectionVtable;
        const auto layout = storeItems == nullptr
                ? std::nullopt
                : readMemory<NativeVector>(storeItems);
        const auto count = layout
                ? vectorElementCount(
                          *layout, sizeof(void*),
                          kMaximumDiscoveredCatalogItems)
                : std::nullopt;
        std::size_t panCount = 0;
        std::size_t localCount = 0;
        if (count) {
            const auto items = sourceCatalogItems(storeItems, *count);
            if (items.size() == *count) {
                panCount = static_cast<std::size_t>(std::count_if(
                        items.begin(), items.end(), exactPanStoreCatalogItem));
                localCount = localStoreCatalogItemCount(items);
            }
        }
        const std::string detail =
                "runtime_enabled=" +
                std::string(enabled ? "true" : "false") +
                ";component_vtable_exact=" +
                std::string(exactComponent ? "true" : "false") +
                ";source_layout_valid=" +
                std::string(count ? "true" : "false") +
                ";source_count=" + std::to_string(count.value_or(0)) +
                ";pan_source_count=" + std::to_string(panCount) +
                ";local_source_count=" + std::to_string(localCount);
        logLine("local cape replaceOffers entry observed: " + detail);
        recordLifecycleEvent(
                "local_cape_replace_offers_entry_observed", detail);
    }
    expandOfferCollectionSources(component, storeItems);
    dobbyOfferCollectionActive = false;
}

void installRepositoryWhenMinecraftImageLoads(void*, void*) {
    logLine("local cape repository: Minecraft image-load callback entered");
    installPersonaCapeRepositoryHook();
}

void registerPersonaCapeRepositoryPreinit() {
    if (repositoryPreinitRegistered.exchange(
                true, std::memory_order_acq_rel)) {
        return;
    }
    if (!addMinecraftImageLoadedCallback(
                nullptr, installRepositoryWhenMinecraftImageLoads)) {
        repositoryPreinitRegistered.store(false, std::memory_order_release);
        logLine("ERROR: local cape repository image-load callback unavailable");
        return;
    }
    logLine("local cape repository: image-load callback registered during preinit");
}

void installPersonaCapeRepositoryHook() {
    if (repositoryHookReady.load(std::memory_order_acquire))
        return;
    if (localCapes().empty()) {
        logLine("ERROR: local cape repository unavailable; no validated cape data");
        return;
    }
    const auto image = findMinecraftImage();
    if (image.base == 0) {
        logLine("local cape repository: Minecraft image pending until mod_init");
        return;
    }

    if (!validateFunction(
                image,
                target::kPersonaManagerLookupOffset,
                target::kPersonaManagerLookupSignature) ||
        !validateFunction(
                image,
                target::kPersonaManagerInsertOffset,
                target::kPersonaManagerInsertSignature) ||
        !validateFunction(
                image,
                target::kPersonaManagerOwnedPiecesOffset,
                target::kPersonaManagerOwnedPiecesSignature) ||
        !validateFunction(
                image,
                target::kPersonaRepositoryOwnedPiecesOffset,
                target::kPersonaRepositoryOwnedPiecesSignature) ||
        !validateFunction(
                image,
                target::kPersonaRepositoryOfflinePiecesOffset,
                target::kPersonaRepositoryOfflinePiecesSignature) ||
        !validateFunction(
                image,
                target::kOfflineCatalogItemMapInsertOffset,
                target::kOfflineCatalogItemMapInsertSignature) ||
        !validateFunction(
                image,
                target::kOfflineCatalogPlayerMapInsertOffset,
                target::kOfflineCatalogPlayerMapInsertSignature) ||
        !validateFunction(
                image,
                target::kOfflineCatalogPlayerMapLookupOffset,
                target::kOfflineCatalogPlayerMapLookupSignature) ||
        !validateFunction(
                image,
                target::kOfflineCatalogItemMapLookupOffset,
                target::kOfflineCatalogItemMapLookupSignature) ||
        !validateFunction(
                image,
                target::kOfflineCatalogIdSetInsertOffset,
                target::kOfflineCatalogIdSetInsertSignature) ||
        !validateFunction(
                image,
                target::kPersonaPieceCopyConstructorOffset,
                target::kPersonaPieceCopyConstructorSignature) ||
        !validateFunction(
                image,
                target::kPersonaPieceDestructorOffset,
                target::kPersonaPieceDestructorSignature) ||
        !validateFunction(
                image,
                target::kPersonaPieceIsValidOffset,
                target::kPersonaPieceIsValidSignature) ||
        !validateFunction(
                image,
                target::kOfferCollectionReplaceOffersOffset,
                target::kOfferCollectionReplaceOffersSignature) ||
        !validateFunction(
                image,
                target::kStoreCatalogItemKeyGetterOffset,
                target::kStoreCatalogItemKeyGetterSignature)) {
        logLine("ERROR: local cape repository unavailable; target layout mismatch");
        return;
    }

    expectedOfferCollectionVtable =
            image.base + target::kOfferCollectionComponentVtableOffset;
    expectedStoreCatalogItemVtable =
            image.base + target::kStoreCatalogItemVtableOffset;
    if (!addressIsInImage(image, expectedOfferCollectionVtable) ||
        !addressIsInImage(image, expectedStoreCatalogItemVtable)) {
        expectedOfferCollectionVtable = 0;
        expectedStoreCatalogItemVtable = 0;
        logLine("ERROR: local cape repository unavailable; catalog vtable target mismatch");
        return;
    }

    managerLookup = reinterpret_cast<ManagerLookupFn>(
            image.base + target::kPersonaManagerLookupOffset);
    managerInsert = reinterpret_cast<ManagerInsertFn>(
            image.base + target::kPersonaManagerInsertOffset);
    managerOwnedPieces = reinterpret_cast<ManagerOwnedPiecesFn>(
            image.base + target::kPersonaManagerOwnedPiecesOffset);
    repositoryOwnedPieces = reinterpret_cast<RepositoryOwnedPiecesFn>(
            image.base + target::kPersonaRepositoryOwnedPiecesOffset);
    repositoryOfflinePieces = reinterpret_cast<RepositoryOfflinePiecesFn>(
            image.base + target::kPersonaRepositoryOfflinePiecesOffset);
    offlineCatalogItemInsert = reinterpret_cast<NativeTreeInsertFn>(
            image.base + target::kOfflineCatalogItemMapInsertOffset);
    offlineCatalogPlayerInsert = reinterpret_cast<NativeTreeInsertFn>(
            image.base + target::kOfflineCatalogPlayerMapInsertOffset);
    offlineCatalogPlayerLookup = reinterpret_cast<NativeTreeLookupFn>(
            image.base + target::kOfflineCatalogPlayerMapLookupOffset);
    offlineCatalogItemLookup = reinterpret_cast<NativeTreeLookupFn>(
            image.base + target::kOfflineCatalogItemMapLookupOffset);
    offlineCatalogIdSetInsert = reinterpret_cast<NativeSetInsertFn>(
            image.base + target::kOfflineCatalogIdSetInsertOffset);
    copyPersonaPiece = reinterpret_cast<PersonaPieceCopyFn>(
            image.base + target::kPersonaPieceCopyConstructorOffset);
    destroyPersonaPiece = reinterpret_cast<PersonaPieceDestroyFn>(
            image.base + target::kPersonaPieceDestructorOffset);
    isValidPersonaPiece = reinterpret_cast<PersonaPieceIsValidFn>(
            image.base + target::kPersonaPieceIsValidOffset);
    offerCollectionReplace = reinterpret_cast<OfferCollectionReplaceFn>(
            image.base + target::kOfferCollectionReplaceOffersOffset);
    const auto ownedAddress =
            image.base + target::kPersonaManagerOwnedPiecesOffset;
    const auto offlineQueryAddress =
            image.base + target::kPersonaRepositoryOfflinePiecesOffset;
    const auto offerCollectionReplaceAddress =
            image.base + target::kOfferCollectionReplaceOffersOffset;
    dobby_persona_manager_owned_continue =
            reinterpret_cast<void*>(ownedAddress + 16);
    dobby_persona_offline_query_continue =
            reinterpret_cast<void*>(offlineQueryAddress + 16);
    dobby_offer_collection_replace_continue =
            reinterpret_cast<void*>(offerCollectionReplaceAddress + 16);
    if (!patchOwnedCapture(ownedAddress) ||
        !patchOfflineQuery(offlineQueryAddress) ||
        !patchOfferCollectionReplace(offerCollectionReplaceAddress)) {
        dobby_persona_manager_owned_continue = nullptr;
        dobby_persona_offline_query_continue = nullptr;
        dobby_offer_collection_replace_continue = nullptr;
        managerLookup = nullptr;
        managerInsert = nullptr;
        managerOwnedPieces = nullptr;
        repositoryOwnedPieces = nullptr;
        repositoryOfflinePieces = nullptr;
        offlineCatalogItemInsert = nullptr;
        offlineCatalogPlayerInsert = nullptr;
        offlineCatalogPlayerLookup = nullptr;
        offlineCatalogItemLookup = nullptr;
        offlineCatalogIdSetInsert = nullptr;
        copyPersonaPiece = nullptr;
        destroyPersonaPiece = nullptr;
        isValidPersonaPiece = nullptr;
        offerCollectionReplace = nullptr;
        expectedOfferCollectionVtable = 0;
        expectedStoreCatalogItemVtable = 0;
        logLine("ERROR: local cape repository unavailable; native picker patch rejected");
        return;
    }
    repositoryHookReady.store(true, std::memory_order_release);
    logLine("local cape repository ready: validated entitlement, offline query, and exact OfferCollectionComponent API; replaceOffers patch installed; rendered offer acceptance pending");
    const bool injected = injectFromLiveRepository(image, false);
    const bool retryCallbackAdded = addLauncherSwapBuffersCallback(
            nullptr, retryLiveRepositoryInjection);
    const bool visibleAuditCallbackAdded = addLauncherSwapBuffersCallback(
            nullptr, auditVisiblePieceOffers);
    if (!visibleAuditCallbackAdded) {
        logLine("ERROR: local cape rendered offer audit callback unavailable");
    }
    if (!injected && !retryCallbackAdded) {
        liveRepositoryRetryFinished.store(true, std::memory_order_release);
        logAcceptanceFailure(
                "live repository was not ready and startup retry callback is unavailable");
    }
}

bool personaCapeRepositoryHookInstalled() {
    return repositoryHookReady.load(std::memory_order_acquire);
}

bool personaCapeRepositoryAccepted() {
    return acceptancePassed.load(std::memory_order_acquire) &&
            offlineCatalogAcceptancePassed.load(std::memory_order_acquire) &&
            visibleOfferAcceptancePassed.load(std::memory_order_acquire);
}

bool setPersonaCapeRepositoryEnabled(bool enabled) {
    if (!repositoryHookReady.load(std::memory_order_acquire))
        return false;
    if (!enabled)
        return !personaCapeRepositoryAccepted();
    if (personaCapeRepositoryAccepted())
        return true;
    return injectFromLiveRepository(findMinecraftImage(), false);
}

} // namespace dobby

#else

namespace dobby {

void registerPersonaCapeRepositoryPreinit() {}
void installPersonaCapeRepositoryHook() {}
bool personaCapeRepositoryHookInstalled() { return false; }
bool personaCapeRepositoryAccepted() { return false; }
bool setPersonaCapeRepositoryEnabled(bool) { return false; }

} // namespace dobby

#endif
