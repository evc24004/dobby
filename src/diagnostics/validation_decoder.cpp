#include "diagnostics/validation_decoder.hpp"

#include "platform/safe_memory.hpp"

#include <array>
#include <cstring>
#include <span>

namespace dobby {
namespace {

constexpr std::size_t maximumFrames = 32;
constexpr std::size_t maximumNestedErrors = 32;
constexpr std::size_t maximumTextLength = 1024;
constexpr std::size_t frameFilenamePointerOffset = 0x08;
constexpr std::size_t frameFilenameSizeOffset = 0x10;
constexpr std::size_t frameLineOffset = 0x18;
constexpr std::size_t frameContextStringOffset = 0x20;
constexpr std::size_t frameContextEngagedOffset = 0x48;

template <class T>
T readUnaligned(const std::byte* address) {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

std::optional<std::string> copyText(const char* source, std::size_t size) {
    if (size > maximumTextLength || (source == nullptr && size != 0))
        return std::nullopt;
    std::string result(size, '\0');
    if (size != 0 && !copyReadableMemory(
                             source,
                             std::span<std::byte>(
                                     reinterpret_cast<std::byte*>(result.data()),
                                     result.size()))) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> decodeAndroidString(const std::byte* object) {
    const auto tag = readUnaligned<std::uint8_t>(object);
    if ((tag & 1U) == 0U) {
        const auto size = static_cast<std::size_t>(tag >> 1U);
        if (size > 23)
            return std::nullopt;
        return std::string(
                reinterpret_cast<const char*>(object + 1), size);
    }
    const auto size = readUnaligned<std::size_t>(object + 8);
    const auto* data = readUnaligned<const char*>(object + 16);
    return copyText(data, size);
}

bool checkedElementCount(
        const std::byte* object, std::size_t vectorOffset,
        std::size_t elementSize, std::size_t maximumCount,
        const std::byte*& begin, std::size_t& count) {
    begin = readUnaligned<const std::byte*>(object + vectorOffset);
    const auto* end = readUnaligned<const std::byte*>(object + vectorOffset + 8);
    if (begin == nullptr || end == nullptr) {
        count = 0;
        return begin == end;
    }
    const auto beginValue = reinterpret_cast<std::uintptr_t>(begin);
    const auto endValue = reinterpret_cast<std::uintptr_t>(end);
    if (endValue < beginValue)
        return false;
    const auto byteCount = endValue - beginValue;
    if (byteCount % elementSize != 0)
        return false;
    count = byteCount / elementSize;
    return count <= maximumCount;
}

std::vector<ValidationSourceFrame> decodeFrames(
        const std::byte* errorInfo, bool& truncated) {
    const std::byte* begin = nullptr;
    std::size_t count = 0;
    if (!checkedElementCount(
                errorInfo, kErrorInfoCallStackOffset, kCallStackFrameSize,
                maximumFrames, begin, count)) {
        truncated = true;
        return {};
    }

    std::vector<ValidationSourceFrame> frames;
    frames.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::array<std::byte, kCallStackFrameSize> bytes{};
        const auto* frameAddress = reinterpret_cast<const void*>(
                reinterpret_cast<std::uintptr_t>(begin) +
                index * kCallStackFrameSize);
        if (!copyReadableMemory(frameAddress, bytes)) {
            truncated = true;
            break;
        }
        ValidationSourceFrame frame;
        frame.filenameHash = readUnaligned<std::uint64_t>(bytes.data());
        const auto* filename = readUnaligned<const char*>(
                bytes.data() + frameFilenamePointerOffset);
        const auto filenameSize = readUnaligned<std::size_t>(
                bytes.data() + frameFilenameSizeOffset);
        const auto copiedFilename = copyText(filename, filenameSize);
        if (copiedFilename)
            frame.filename = *copiedFilename;
        else
            truncated = true;
        frame.line = readUnaligned<std::uint32_t>(
                bytes.data() + frameLineOffset);
        if (readUnaligned<std::uint8_t>(
                    bytes.data() + frameContextEngagedOffset) != 0) {
            const auto context = decodeAndroidString(
                    bytes.data() + frameContextStringOffset);
            if (context)
                frame.context = *context;
            else
                truncated = true;
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

void decodeNestedErrors(
        const std::byte* errorInfo, std::uint32_t depth,
        std::vector<ValidationNestedErrorView>& output, bool& truncated) {
    if (depth > 4 || output.size() >= maximumNestedErrors) {
        truncated = true;
        return;
    }
    const std::byte* begin = nullptr;
    std::size_t count = 0;
    const auto remaining = maximumNestedErrors - output.size();
    if (!checkedElementCount(
                errorInfo, kErrorInfoStackErrorsOffset, kErrorInfoSize,
                remaining, begin, count)) {
        truncated = true;
        return;
    }
    for (std::size_t index = 0; index < count; ++index) {
        std::array<std::byte, kErrorInfoSize> bytes{};
        const auto* nestedAddress = reinterpret_cast<const void*>(
                reinterpret_cast<std::uintptr_t>(begin) +
                index * kErrorInfoSize);
        if (!copyReadableMemory(nestedAddress, bytes)) {
            truncated = true;
            return;
        }
        ValidationNestedErrorView nested;
        nested.depth = depth;
        nested.errorValue = readUnaligned<std::int32_t>(
                bytes.data() + kExpectedErrorValueOffset);
        nested.errorCategory = readUnaligned<const void*>(
                bytes.data() + kExpectedErrorCategoryOffset);
        nested.sourceFrames = decodeFrames(bytes.data(), truncated);
        output.push_back(std::move(nested));
        decodeNestedErrors(bytes.data(), depth + 1, output, truncated);
        if (output.size() >= maximumNestedErrors) {
            truncated = true;
            return;
        }
    }
}

} // namespace

std::optional<ErrorCodeView> decodeErrorCode(const void* errorCode) {
    if (errorCode == nullptr)
        return std::nullopt;

    std::array<std::byte, kErrorCodeSize> bytes{};
    if (!copyReadableMemory(errorCode, std::span<std::byte>(bytes)))
        return std::nullopt;
    return ErrorCodeView{
            readUnaligned<std::int32_t>(bytes.data()),
            readUnaligned<const void*>(bytes.data() + 8)};
}

std::optional<ValidationResultView> decodeValidationResult(const void* result) {
    if (result == nullptr)
        return std::nullopt;

    std::array<std::byte, kExpectedResultSize> bytes{};
    if (!copyReadableMemory(result, std::span<std::byte>(bytes)))
        return std::nullopt;

    ValidationResultView decoded;
    decoded.success =
            (readUnaligned<std::uint8_t>(bytes.data() + kExpectedHasValueOffset) &
             1U) != 0U;
    if (!decoded.success) {
        decoded.errorValue = readUnaligned<std::int32_t>(
                bytes.data() + kExpectedErrorValueOffset);
        decoded.errorCategory = readUnaligned<const void*>(
                bytes.data() + kExpectedErrorCategoryOffset);
        decoded.sourceFrames = decodeFrames(
                bytes.data(), decoded.provenanceTruncated);
        decodeNestedErrors(
                bytes.data(), 1, decoded.nestedErrors,
                decoded.provenanceTruncated);
    }
    return decoded;
}

} // namespace dobby
