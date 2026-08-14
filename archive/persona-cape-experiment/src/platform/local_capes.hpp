#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dobby {

inline constexpr std::size_t kCapeTextureWidth = 64;
inline constexpr std::size_t kCapeTextureHeight = 32;
inline constexpr std::size_t kCapeTextureBytes =
        kCapeTextureWidth * kCapeTextureHeight * 4;
inline constexpr std::size_t kMaximumLocalCapes = 64;

struct LocalCapeDescriptor {
    std::string pieceId;
    std::string packId;
    std::string title;
};

struct LocalCape {
    LocalCapeDescriptor descriptor;
    std::array<std::uint8_t, kCapeTextureBytes> rgba{};
};

bool validLocalCapeId(std::string_view value);
std::optional<std::vector<LocalCapeDescriptor>> parseLocalCapeIndex(
        std::string_view text);
const std::vector<LocalCape>& localCapes();
const LocalCape* findLocalCape(std::string_view pieceId);

} // namespace dobby
