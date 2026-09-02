#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dobby {

inline constexpr std::ptrdiff_t kExpectedErrorValueOffset = 0x00;
inline constexpr std::ptrdiff_t kExpectedErrorCategoryOffset = 0x08;
inline constexpr std::ptrdiff_t kExpectedHasValueOffset = 0x40;
inline constexpr std::size_t kExpectedResultSize = 0x48;
inline constexpr std::ptrdiff_t kErrorInfoCallStackOffset = 0x10;
inline constexpr std::ptrdiff_t kErrorInfoStackErrorsOffset = 0x28;
inline constexpr std::size_t kErrorInfoSize = 0x40;
inline constexpr std::size_t kCallStackFrameSize = 0x50;
inline constexpr std::size_t kErrorCodeSize = 0x10;

struct ErrorCodeView {
    std::int32_t value{};
    const void* category{};
};

struct ValidationSourceFrame {
    std::uint64_t filenameHash{};
    std::string filename;
    std::uint32_t line{};
    std::string context;
};

struct ValidationNestedErrorView {
    std::uint32_t depth{};
    std::int32_t errorValue{};
    const void* errorCategory{};
    std::vector<ValidationSourceFrame> sourceFrames;
};

struct ValidationResultView {
    bool success{};
    std::int32_t errorValue{};
    const void* errorCategory{};
    std::vector<ValidationSourceFrame> sourceFrames;
    std::vector<ValidationNestedErrorView> nestedErrors;
    bool provenanceTruncated{};
};

std::optional<ValidationResultView> decodeValidationResult(const void* result);
std::optional<ErrorCodeView> decodeErrorCode(const void* errorCode);

} // namespace dobby
