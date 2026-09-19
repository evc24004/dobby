#include "hooks/ore_esp_scanner.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "platform/log.hpp"
#include "platform/safe_memory.hpp"
#include "ui/ore_esp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dobby {
namespace {

template <class Element>
struct ClientSpan {
    std::size_t size{};
    const Element* data{};
};

using GetHashedNameFn = const void* (*)(const void* blockType);
using GetHashedStringValueFn = const void* (*)(const void* hashedString);
using GetElementFn = const void* (*)(
        const void* storage, std::uint16_t index);
using GetPackedElementFn = ClientSpan<std::uint32_t> (*)(const void* storage);
using GetBitsPerElementFn = std::size_t (*)(const void* storage);
using GetPaletteSnapshotFn = ClientSpan<const void*> (*)(const void* storage);

constexpr std::size_t kBlocksPerSubChunk = 4'096;
constexpr std::size_t kMaximumSubChunksPerChunk = 64;
constexpr std::size_t kMaximumCachedBlockTypes = 32'768;
constexpr std::size_t kMaximumBlockNameLength = 128;
constexpr std::size_t kMaximumPaletteEntries = kBlocksPerSubChunk;
constexpr std::size_t kMaximumTrackedOreChunks = 4'096;
constexpr std::uint64_t kPeriodicRescanIntervalMicroseconds = 100'000;
constexpr std::uint64_t kRetryDelayMicroseconds = 250'000;

struct SubChunkRange {
    const std::byte* begin{};
    std::size_t count{};
};

struct BlockClassification {
    bool valid{};
    std::optional<OreKind> ore;
};

struct ScanMetrics {
    std::size_t subChunks{};
    std::size_t paletteEntries{};
    std::size_t packedWords{};
    std::size_t decodedBlocks{};
    std::size_t ores{};
};

enum class ChunkScanResult {
    success,
    retryableFailure,
    registryCapacityReached,
};

MinecraftImage minecraftImage{};
GetHashedNameFn getHashedName = nullptr;
GetHashedStringValueFn getHashedStringValue = nullptr;
std::atomic_bool ready{false};
std::atomic_bool layoutFailureLogged{false};
std::atomic_bool capacityFailureLogged{false};
std::atomic_bool firstOreLogged{false};
std::atomic_bool firstBlockNameLogged{false};
std::atomic_bool unreadableBlockNameLogged{false};
std::atomic_bool scanMetricsLogged{false};
thread_local const char* lastFailureStage = "not recorded";
// Schedule ownership changes and their registry cleanup are one transaction.
// The only nested lock order is rescanMutex -> ChunkOreRegistry's mutex.
std::mutex rescanMutex;
OreRescanSchedule rescanSchedule{
        kMaximumTrackedOreChunks, kPeriodicRescanIntervalMicroseconds};
bool explicitRescanActive = false;
std::uint64_t trackedEvents{};
std::uint64_t dirtyEvents{};
std::uint64_t evictedTargets{};
std::uint64_t ignoredStaleDirtyEvents{};
std::uint64_t recoveredUnloadEvents{};
std::uint64_t successfulScans{};
std::uint64_t retriedScans{};
std::uint64_t capacityFailedScans{};
std::uint64_t scanMicroseconds{};
std::uint64_t maximumScanMicroseconds{};
std::uint64_t nextDiagnosticMicroseconds{};

static_assert(sizeof(ClientSpan<std::uint32_t>) == 2 * sizeof(void*));

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

template <class Value>
std::optional<Value> readProcessField(
        const void* object, std::ptrdiff_t offset) {
    if (object == nullptr)
        return std::nullopt;
    Value value{};
    auto destination = std::as_writable_bytes(std::span{&value, 1U});
    const auto* source = static_cast<const std::byte*>(object) + offset;
    if (!copyReadableMemory(source, destination))
        return std::nullopt;
    return value;
}

bool imageVtable(const void* object) {
    if (!alignedPointer(object))
        return false;
    const auto vtable = readProcessField<std::uintptr_t>(object, 0);
    return vtable && addressIsInImage(minecraftImage, *vtable);
}

void logLayoutFailure() {
    if (!layoutFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine(std::string("ore ESP: deferred an unstable chunk scan at ") +
                lastFailureStage + "; previous results were retained");
    }
}

void logCapacityFailure() {
    if (!capacityFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        logLine("ore ESP: client-known ore registry reached its safety limit");
    }
}

std::optional<std::string> readAndroidString(const void* object) {
    if (object == nullptr)
        return std::nullopt;
    std::array<std::byte, 24> storage{};
    if (!copyReadableMemory(object, storage))
        return std::nullopt;
    const auto* bytes = storage.data();
    const auto tag = readObjectField<std::uint8_t>(bytes, 0);
    if ((tag & 1U) == 0U) {
        const std::size_t length = tag >> 1U;
        if (length > 23 || length > kMaximumBlockNameLength)
            return std::nullopt;
        return std::string(
                reinterpret_cast<const char*>(bytes + 1), length);
    }
    const std::size_t length = readObjectField<std::size_t>(bytes, 8);
    const char* data = readObjectField<const char*>(bytes, 16);
    if (data == nullptr || length == 0 || length > kMaximumBlockNameLength)
        return std::nullopt;
    std::string result(length, '\0');
    if (!copyReadableMemory(
                data, std::as_writable_bytes(std::span(result)))) {
        return std::nullopt;
    }
    return result;
}

BlockClassification classifyBlock(const void* block) {
    thread_local std::unordered_map<const void*, std::int16_t> classifications;
    const auto cached = classifications.find(block);
    if (cached != classifications.end()) {
        if (cached->second < 0)
            return {true, std::nullopt};
        return {true, static_cast<OreKind>(cached->second)};
    }
    if (!imageVtable(block) || getHashedName == nullptr ||
        getHashedStringValue == nullptr) {
        lastFailureStage = "Block vtable validation";
        return {};
    }

    const auto blockTypeField = readProcessField<const void*>(
            block, target::kBlockBlockTypeOffset);
    if (!blockTypeField || !imageVtable(*blockTypeField)) {
        lastFailureStage = "Block::mBlockType validation";
        return {};
    }
    const void* blockType = *blockTypeField;
    const void* hashedName = getHashedName(blockType);
    const auto* expectedHashedName =
            static_cast<const std::byte*>(blockType) +
            target::kBlockTypeHashedNameOffset;
    if (hashedName != expectedHashedName) {
        lastFailureStage = "BlockType HashedString return validation";
        return {};
    }
    const void* nameObject = getHashedStringValue(hashedName);
    const auto* expectedNameObject =
            static_cast<const std::byte*>(hashedName) +
            target::kHashedStringValueOffset;
    if (nameObject != expectedNameObject) {
        lastFailureStage = "HashedString value return validation";
        return {};
    }
    const auto name = readAndroidString(nameObject);
    const auto nameClassification = classifyOreBlockNameForCache(
            name ? std::optional<std::string_view>{*name} : std::nullopt);
    if (!nameClassification.cacheable) {
        if (!unreadableBlockNameLogged.exchange(
                    true, std::memory_order_acq_rel)) {
            logLine("ore ESP: skipped an unreadable client block name");
        }
        return {true, std::nullopt};
    }
    if (!firstBlockNameLogged.exchange(true, std::memory_order_acq_rel))
        logLine("ore ESP: exact client block-name decoding active");
    const std::optional<OreKind> ore = nameClassification.ore;
    if (classifications.size() < kMaximumCachedBlockTypes) {
        classifications.emplace(
                block, ore ? static_cast<std::int16_t>(*ore) : -1);
    }
    return {true, ore};
}

const target::SubChunkStorageDispatch* storageDispatch(const void* storage) {
    if (!alignedPointer(storage))
        return nullptr;
    const auto vtable = readProcessField<std::uintptr_t>(storage, 0);
    if (!vtable)
        return nullptr;
    const auto found = std::find_if(
            target::kSubChunkStorageDispatches.begin(),
            target::kSubChunkStorageDispatches.end(),
            [&](const target::SubChunkStorageDispatch& dispatch) {
                return *vtable == minecraftImage.base +
                        dispatch.vtableAddressPointOffset;
            });
    return found == target::kSubChunkStorageDispatches.end()
            ? nullptr
            : &*found;
}

bool readSubChunkRange(const void* chunk, SubChunkRange& output) {
    if (!alignedPointer(chunk)) {
        lastFailureStage = "LevelChunk pointer validation";
        return false;
    }
    const auto begin = readProcessField<const void*>(
            chunk, target::kLevelChunkSubChunksOffset);
    const auto end = readProcessField<const void*>(
            chunk, target::kLevelChunkSubChunksOffset +
                    static_cast<std::ptrdiff_t>(sizeof(void*)));
    if (!begin || !end) {
        lastFailureStage = "LevelChunk subchunk-vector readability validation";
        return false;
    }
    if (*begin == nullptr && *end == nullptr) {
        output = {};
        return true;
    }
    if (!alignedPointer(*begin) || !alignedPointer(*end)) {
        lastFailureStage = "LevelChunk subchunk-vector pointer validation";
        return false;
    }
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(*begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(*end);
    if (endAddress < beginAddress ||
        (endAddress - beginAddress) % target::kSubChunkSize != 0) {
        lastFailureStage = "LevelChunk subchunk-vector bounds validation";
        return false;
    }
    const std::size_t count =
            (endAddress - beginAddress) / target::kSubChunkSize;
    if (count > kMaximumSubChunksPerChunk) {
        lastFailureStage = "LevelChunk subchunk-vector capacity validation";
        return false;
    }
    output = {static_cast<const std::byte*>(*begin), count};
    return true;
}

template <class Element>
bool copyClientSpan(
        ClientSpan<Element> source, std::size_t maximumSize,
        std::vector<Element>& output) {
    if (source.size > maximumSize) {
        lastFailureStage = "client span capacity validation";
        return false;
    }
    if (source.size == 0) {
        output.clear();
        return true;
    }
    if (source.data == nullptr ||
        reinterpret_cast<std::uintptr_t>(source.data) % alignof(Element) != 0) {
        lastFailureStage = "client span pointer validation";
        return false;
    }
    output.resize(source.size);
    if (!copyReadableMemory(
                source.data,
                std::as_writable_bytes(std::span{output}))) {
        lastFailureStage = "client span readable-memory validation";
        return false;
    }
    return true;
}

std::optional<std::size_t> packedWordCount(std::uint8_t bitsPerElement) {
    if (bitsPerElement == 0)
        return 0;
    const bool supported = bitsPerElement == 1 || bitsPerElement == 2 ||
            bitsPerElement == 3 || bitsPerElement == 4 ||
            bitsPerElement == 5 || bitsPerElement == 6 ||
            bitsPerElement == 8 || bitsPerElement == 16;
    if (!supported)
        return std::nullopt;
    const std::size_t elementsPerWord = 32U / bitsPerElement;
    return (kBlocksPerSubChunk + elementsPerWord - 1) / elementsPerWord;
}

void logScanMetricsOnce(
        const ScanMetrics& metrics, std::chrono::steady_clock::duration elapsed) {
    if (scanMetricsLogged.exchange(true, std::memory_order_acq_rel))
        return;
    const auto microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    char message[256]{};
    std::snprintf(
            message, sizeof(message),
            "ore ESP scan: %zu subchunks, %zu palette entries, %zu packed "
            "words, %zu decoded blocks, %zu ores, %lld us",
            metrics.subChunks, metrics.paletteEntries, metrics.packedWords,
            metrics.decodedBlocks, metrics.ores,
            static_cast<long long>(microseconds));
    logLine(message);
}

bool scanSubChunk(
        const std::byte* subChunk, ChunkPosition chunk,
        std::int16_t expectedAbsoluteSubChunk, ScanMetrics& metrics,
        std::vector<OreSubChunkSnapshot>& chunkSnapshot) {
    ++metrics.subChunks;
    const auto storedAbsoluteSubChunk = readProcessField<std::int8_t>(
            subChunk, target::kSubChunkAbsoluteIndexOffset);
    if (!storedAbsoluteSubChunk) {
        lastFailureStage = "SubChunk absolute-index readability validation";
        return false;
    }
    const auto absoluteSubChunk =
            static_cast<std::int16_t>(*storedAbsoluteSubChunk);
    if (absoluteSubChunk != expectedAbsoluteSubChunk) {
        lastFailureStage = "SubChunk absolute-index validation";
        return false;
    }

    const auto storage = readProcessField<const void*>(
            subChunk, target::kSubChunkStandardStorageOffset);
    if (!storage) {
        lastFailureStage = "SubChunkStorage pointer readability validation";
        return false;
    }
    std::vector<OreBlock> ores;
    if (*storage != nullptr) {
        const auto* dispatch = storageDispatch(*storage);
        if (dispatch == nullptr) {
            lastFailureStage = "SubChunkStorage vtable validation";
            return false;
        }

        const auto getBitsPerElement = reinterpret_cast<GetBitsPerElementFn>(
                minecraftImage.base + dispatch->bitsPerElementOffset);
        const std::size_t bitsPerElement = getBitsPerElement(*storage);
        if (bitsPerElement != dispatch->bitsPerElement) {
            lastFailureStage = "SubChunkStorage bit-width validation";
            return false;
        }

        std::vector<const void*> palette;
        if (bitsPerElement == 0) {
            const auto getElement = reinterpret_cast<GetElementFn>(
                    minecraftImage.base + dispatch->getElementOffset);
            const void* uniformBlock = getElement(*storage, 0);
            if (uniformBlock == nullptr) {
                lastFailureStage = "uniform SubChunkStorage validation";
                return false;
            }
            palette.push_back(uniformBlock);
        } else {
            const auto getPaletteSnapshot =
                    reinterpret_cast<GetPaletteSnapshotFn>(
                            minecraftImage.base +
                            dispatch->paletteSnapshotOffset);
            if (!copyClientSpan(
                        getPaletteSnapshot(*storage), kMaximumPaletteEntries,
                        palette) || palette.empty()) {
                lastFailureStage = "SubChunkStorage palette validation";
                return false;
            }
        }
        metrics.paletteEntries += palette.size();

        std::vector<std::int16_t> paletteClassifications;
        paletteClassifications.reserve(palette.size());
        bool paletteContainsOre = false;
        for (const void* block : palette) {
            const BlockClassification classification = classifyBlock(block);
            if (!classification.valid)
                return false;
            const std::int16_t encoded = classification.ore
                    ? static_cast<std::int16_t>(*classification.ore)
                    : -1;
            paletteClassifications.push_back(encoded);
            paletteContainsOre =
                    paletteContainsOre || classification.ore.has_value();
        }
        if (paletteContainsOre) {
            const auto expectedPackedWords = packedWordCount(
                    static_cast<std::uint8_t>(bitsPerElement));
            if (!expectedPackedWords) {
                lastFailureStage = "SubChunkStorage packed-width validation";
                return false;
            }
            const auto getPackedElement =
                    reinterpret_cast<GetPackedElementFn>(
                            minecraftImage.base +
                            dispatch->packedElementOffset);
            const auto packedSource = getPackedElement(*storage);
            if (packedSource.size != *expectedPackedWords) {
                lastFailureStage =
                        "SubChunkStorage packed-word count validation";
                return false;
            }
            std::vector<std::uint32_t> packedWords;
            if (!copyClientSpan(
                        packedSource, *expectedPackedWords, packedWords)) {
                return false;
            }
            metrics.packedWords += packedWords.size();
            metrics.decodedBlocks += kBlocksPerSubChunk;
            ores.reserve(128);
            for (std::uint16_t index = 0; index < kBlocksPerSubChunk;
                 ++index) {
                const auto paletteIndex = packedPaletteIndex(
                        packedWords,
                        static_cast<std::uint8_t>(bitsPerElement), index,
                        paletteClassifications.size());
                if (!paletteIndex) {
                    lastFailureStage =
                            "SubChunkStorage packed-index validation";
                    return false;
                }
                const std::int16_t encoded =
                        paletteClassifications[*paletteIndex];
                if (encoded < 0)
                    continue;
                const auto localX = static_cast<std::uint8_t>(
                        (index >> 8U) & 0x0fU);
                const auto localZ = static_cast<std::uint8_t>(
                        (index >> 4U) & 0x0fU);
                const auto localY =
                        static_cast<std::uint8_t>(index & 0x0fU);
                ores.push_back({
                        worldBlockPosition(
                                chunk, absoluteSubChunk, localX, localY,
                                localZ),
                        static_cast<OreKind>(encoded)});
            }
        }
    }

    metrics.ores += ores.size();
    chunkSnapshot.push_back({absoluteSubChunk, std::move(ores)});
    return true;
}

std::uint64_t monotonicMicroseconds() {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

ChunkScanResult scanClientChunkOres(
        const void* chunk, const void* levelIdentity,
        ChunkPosition position) {
    if (!oreEspScannerReady() || levelIdentity == nullptr ||
        !validChunkPosition(position)) {
        return ChunkScanResult::retryableFailure;
    }
    const auto started = std::chrono::steady_clock::now();
    ScanMetrics metrics{};
    lastFailureStage = "not recorded";
    SubChunkRange range{};
    if (!readSubChunkRange(chunk, range)) {
        logLayoutFailure();
        return ChunkScanResult::retryableFailure;
    }
    std::vector<OreSubChunkSnapshot> chunkSnapshot;
    chunkSnapshot.reserve(range.count);
    for (std::size_t index = 0; index < range.count; ++index) {
        const auto* subChunk = range.begin + index * target::kSubChunkSize;
        const auto storedAbsolute = readProcessField<std::int8_t>(
                subChunk, target::kSubChunkAbsoluteIndexOffset);
        if (!storedAbsolute) {
            lastFailureStage =
                    "SubChunk absolute-index readability validation";
            logLayoutFailure();
            return ChunkScanResult::retryableFailure;
        }
        const auto absolute = static_cast<std::int16_t>(*storedAbsolute);
        if (!scanSubChunk(
                    subChunk, position, absolute, metrics, chunkSnapshot)) {
            logScanMetricsOnce(
                    metrics, std::chrono::steady_clock::now() - started);
            logLayoutFailure();
            return ChunkScanResult::retryableFailure;
        }
    }
    const auto update = replaceClientChunkOres(
            levelIdentity, position, chunkSnapshot);
    if (update == ChunkOreUpdateResult::invalidInput) {
        lastFailureStage = "ore registry input validation";
        logLayoutFailure();
        return ChunkScanResult::retryableFailure;
    }
    if (update != ChunkOreUpdateResult::accepted) {
        logCapacityFailure();
        return ChunkScanResult::registryCapacityReached;
    }
    logScanMetricsOnce(
            metrics, std::chrono::steady_clock::now() - started);
    if (metrics.ores != 0 &&
        !firstOreLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[160]{};
        std::snprintf(
                message, sizeof(message),
                "ore ESP: client decoded %zu ore block(s) in chunk (%d,%d)",
                metrics.ores, position.x, position.z);
        logLine(message);
    }
    return ChunkScanResult::success;
}

bool validateScannerTargets(const MinecraftImage& image) {
    const auto validateSignature = [&](std::uintptr_t offset, const auto& signature) {
        const auto address = image.base + offset;
        return addressIsExecutable(image, address) &&
                matchesSignature(reinterpret_cast<const void*>(address), signature);
    };
    if (!validateSignature(
                target::kLevelChunkSubChunkLayoutProbeOffset,
                target::kLevelChunkSubChunkLayoutProbeSignature) ||
        !validateSignature(
                target::kSubChunkAbsoluteIndexAccessorOffset,
                target::kSubChunkAbsoluteIndexAccessorSignature) ||
        !validateSignature(
                target::kSubChunkStorageLayoutProbeOffset,
                target::kSubChunkStorageLayoutProbeSignature) ||
        !validateSignature(
                target::kBlockTypeGetHashedNameOffset,
                target::kBlockTypeGetHashedNameSignature) ||
        !validateSignature(
                target::kHashedStringGetValueOffset,
                target::kHashedStringGetValueSignature)) {
        return false;
    }
    for (const auto& dispatch : target::kSubChunkStorageDispatches) {
        const auto vtable = image.base + dispatch.vtableAddressPointOffset;
        const auto getElement = image.base + dispatch.getElementOffset;
        const auto getPackedElement =
                image.base + dispatch.packedElementOffset;
        const auto getBitsPerElement =
                image.base + dispatch.bitsPerElementOffset;
        const auto getPaletteSnapshot =
                image.base + dispatch.paletteSnapshotOffset;
        if (!addressIsInImage(image, vtable) ||
            vtable > image.loadEnd -
                    (target::kSubChunkStoragePaletteSnapshotVtableSlot + 1) *
                            sizeof(std::uintptr_t) ||
            !addressIsExecutable(image, getElement) ||
            !addressIsExecutable(image, getPackedElement) ||
            !addressIsExecutable(image, getBitsPerElement) ||
            !addressIsExecutable(image, getPaletteSnapshot)) {
            return false;
        }
        const auto* slots = reinterpret_cast<const std::uintptr_t*>(vtable);
        if (slots[target::kSubChunkStorageGetElementVtableSlot] != getElement ||
            slots[target::kSubChunkStoragePackedElementVtableSlot] !=
                    getPackedElement ||
            slots[target::kSubChunkStorageBitsPerElementVtableSlot] !=
                    getBitsPerElement ||
            slots[target::kSubChunkStoragePaletteSnapshotVtableSlot] !=
                    getPaletteSnapshot) {
            return false;
        }
    }
    return true;
}

} // namespace

bool initializeOreEspScanner(const MinecraftImage& image) {
    if (!target::kOreTargetsAvailable) {
        ready.store(false, std::memory_order_release);
        runtimeState().setOreEspAvailable(false);
        logLine("ore ESP: disabled; 1.26.51.1 storage dispatch targets are unverified");
        return false;
    }
    const bool validated = image.base != 0 && validateScannerTargets(image);
    if (!validated) {
        ready.store(false, std::memory_order_release);
        runtimeState().setOreEspAvailable(false);
        logLine("ERROR: ore ESP unavailable; client chunk target mismatch");
        return false;
    }
    minecraftImage = image;
    getHashedName = reinterpret_cast<GetHashedNameFn>(
            image.base + target::kBlockTypeGetHashedNameOffset);
    getHashedStringValue = reinterpret_cast<GetHashedStringValueFn>(
            image.base + target::kHashedStringGetValueOffset);
    ready.store(true, std::memory_order_release);
    runtimeState().setOreEspAvailable(true);
    logLine("ore ESP: client-decoded subchunk scanner ready");
    return true;
}

bool oreEspScannerReady() {
    return ready.load(std::memory_order_acquire);
}

void trackClientChunkForOreEsp(
        const void* chunk, const void* levelIdentity,
        ChunkPosition position) {
    if (!oreEspScannerReady())
        return;
    std::optional<OreChunkScanTarget> evicted;
    {
        std::lock_guard lock(rescanMutex);
        const auto result = rescanSchedule.track(
                {chunk, levelIdentity, position}, &evicted);
        if (result == OreChunkTrackResult::invalidInput ||
            result == OreChunkTrackResult::capacityReached) {
            return;
        }
        ++trackedEvents;
        if (result == OreChunkTrackResult::acceptedWithEviction)
            ++evictedTargets;
        if (evicted)
            removeClientChunkOres(evicted->level, evicted->position);
    }
}

void dirtyClientChunkForOreEsp(
        const void* chunk, const void* levelIdentity,
        ChunkPosition position) {
    if (!oreEspScannerReady())
        return;
    std::optional<OreChunkScanTarget> evicted;
    {
        std::lock_guard lock(rescanMutex);
        const OreChunkScanTarget target{chunk, levelIdentity, position};
        const auto result = rescanSchedule.trackIfAbsent(target, &evicted);
        if (result == OreChunkTrackResult::invalidInput ||
            result == OreChunkTrackResult::capacityReached) {
            return;
        }
        if (result == OreChunkTrackResult::existingOwner) {
            ++ignoredStaleDirtyEvents;
            return;
        }
        if (!rescanSchedule.markDirty(target))
            return;
        ++dirtyEvents;
        if (result == OreChunkTrackResult::acceptedWithEviction)
            ++evictedTargets;
        if (evicted)
            removeClientChunkOres(evicted->level, evicted->position);
    }
}

void untrackClientChunkForOreEsp(
        const void* chunk, const void* levelIdentity,
        ChunkPosition position) {
    bool removed = false;
    {
        std::lock_guard lock(rescanMutex);
        removed = rescanSchedule.remove(
                {chunk, levelIdentity, position});
        if (removed)
            removeClientChunkOres(levelIdentity, position);
    }
}

std::optional<OreChunkScanTarget> untrackClientChunkForOreEsp(
        const void* chunk) {
    std::lock_guard lock(rescanMutex);
    auto removed = rescanSchedule.removeByChunk(chunk);
    if (removed) {
        removeClientChunkOres(removed->level, removed->position);
        ++recoveredUnloadEvents;
    }
    return removed;
}

void requestClientOreRescan() {
    std::size_t tracked = 0;
    {
        std::lock_guard lock(rescanMutex);
        rescanSchedule.requestFullRescan();
        tracked = rescanSchedule.size();
        explicitRescanActive = true;
    }
    char message[128]{};
    std::snprintf(
            message, sizeof(message),
            "ore ESP: refreshing %zu loaded chunk(s) nearest-first", tracked);
    logLine(message);
}

void processClientOreRescan(
        const void* levelIdentity, Vec3f cameraPosition) {
    if (!oreEspScannerReady() || levelIdentity == nullptr)
        return;
    bool completedExplicitRescan = false;
    bool writeDiagnostic = false;
    std::size_t tracked = 0;
    std::size_t active = 0;
    std::size_t pending = 0;
    std::uint64_t diagnosticSuccessfulScans = 0;
    std::uint64_t diagnosticRetriedScans = 0;
    std::uint64_t diagnosticCapacityFailures = 0;
    std::uint64_t diagnosticEvictions = 0;
    std::uint64_t diagnosticIgnoredStaleDirties = 0;
    std::uint64_t diagnosticRecoveredUnloads = 0;
    std::uint64_t diagnosticTrackedEvents = 0;
    std::uint64_t diagnosticDirtyEvents = 0;
    std::uint64_t diagnosticScanMicroseconds = 0;
    std::uint64_t diagnosticMaximumScanMicroseconds = 0;
    {
        // Unload removes the target under the same mutex before Bedrock may
        // release it, so the native chunk remains alive for this bounded scan.
        std::lock_guard lock(rescanMutex);
        const std::uint64_t now = monotonicMicroseconds();
        const auto target = rescanSchedule.selectNext(
                levelIdentity, cameraPosition, now);
        if (target) {
            const std::uint64_t scanStarted = monotonicMicroseconds();
            const ChunkScanResult result = scanClientChunkOres(
                    target->chunk, target->level, target->position);
            const std::uint64_t scanFinished = monotonicMicroseconds();
            const std::uint64_t elapsed = scanFinished >= scanStarted
                    ? scanFinished - scanStarted
                    : 0;
            scanMicroseconds += elapsed;
            maximumScanMicroseconds =
                    std::max(maximumScanMicroseconds, elapsed);
            if (result == ChunkScanResult::success) {
                rescanSchedule.markScanned(*target, scanFinished);
                ++successfulScans;
            } else {
                static_cast<void>(rescanSchedule.markRetry(
                        *target, scanFinished, kRetryDelayMicroseconds));
                if (result == ChunkScanResult::registryCapacityReached)
                    ++capacityFailedScans;
                else
                    ++retriedScans;
            }
        }
        if (explicitRescanActive &&
            rescanSchedule.pendingForLevel(levelIdentity) == 0) {
            explicitRescanActive = false;
            completedExplicitRescan = true;
        }
        if (now >= nextDiagnosticMicroseconds) {
            constexpr std::uint64_t interval = 5'000'000;
            nextDiagnosticMicroseconds =
                    now > std::numeric_limits<std::uint64_t>::max() - interval
                    ? std::numeric_limits<std::uint64_t>::max()
                    : now + interval;
            tracked = rescanSchedule.size();
            active = rescanSchedule.sizeForLevel(levelIdentity);
            pending = rescanSchedule.pendingForLevel(levelIdentity);
            diagnosticSuccessfulScans = successfulScans;
            diagnosticRetriedScans = retriedScans;
            diagnosticCapacityFailures = capacityFailedScans;
            diagnosticEvictions = evictedTargets;
            diagnosticIgnoredStaleDirties = ignoredStaleDirtyEvents;
            diagnosticRecoveredUnloads = recoveredUnloadEvents;
            diagnosticTrackedEvents = trackedEvents;
            diagnosticDirtyEvents = dirtyEvents;
            diagnosticScanMicroseconds = scanMicroseconds;
            diagnosticMaximumScanMicroseconds = maximumScanMicroseconds;
            writeDiagnostic = true;
        }
    }
    if (completedExplicitRescan)
        logLine("ore ESP: loaded-chunk refresh complete");
    if (writeDiagnostic) {
        const std::size_t registryChunks =
                clientKnownOreChunkCountForLevel(levelIdentity);
        const std::size_t registryOres =
                clientKnownOreCountForLevel(levelIdentity);
        char message[448]{};
        std::snprintf(
                message, sizeof(message),
                "ore ESP pipeline: tracked=%zu active=%zu pending=%zu "
                "registry_chunks=%zu ores=%zu scans=%llu retries=%llu "
                "capacity_failures=%llu evictions=%llu loads=%llu "
                "dirties=%llu stale_dirties=%llu recovered_unloads=%llu "
                "scan_us=%llu max_scan_us=%llu",
                tracked, active, pending, registryChunks, registryOres,
                static_cast<unsigned long long>(diagnosticSuccessfulScans),
                static_cast<unsigned long long>(diagnosticRetriedScans),
                static_cast<unsigned long long>(diagnosticCapacityFailures),
                static_cast<unsigned long long>(diagnosticEvictions),
                static_cast<unsigned long long>(diagnosticTrackedEvents),
                static_cast<unsigned long long>(diagnosticDirtyEvents),
                static_cast<unsigned long long>(
                        diagnosticIgnoredStaleDirties),
                static_cast<unsigned long long>(diagnosticRecoveredUnloads),
                static_cast<unsigned long long>(diagnosticScanMicroseconds),
                static_cast<unsigned long long>(
                        diagnosticMaximumScanMicroseconds));
        logLine(message);
    }
}

} // namespace dobby

#else

namespace dobby {

bool initializeOreEspScanner(const MinecraftImage&) { return false; }
bool oreEspScannerReady() { return false; }
void trackClientChunkForOreEsp(
        const void*, const void*, ChunkPosition) {}
void dirtyClientChunkForOreEsp(
        const void*, const void*, ChunkPosition) {}
void untrackClientChunkForOreEsp(
        const void*, const void*, ChunkPosition) {}
std::optional<OreChunkScanTarget> untrackClientChunkForOreEsp(
        const void*) { return std::nullopt; }
void requestClientOreRescan() {}
void processClientOreRescan(const void*, Vec3f) {}

} // namespace dobby

#endif
