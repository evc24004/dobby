#pragma once

#include "diagnostics/types.hpp"

#include <cstdint>
#include <span>

namespace dobby {

ResourcePackClientResponseEvidence decodeResourcePackClientResponse(
        std::span<const std::uint8_t> bytes, bool rawBytesTruncated = false);

const char* resourcePackResponseStatusName(std::int32_t status);

} // namespace dobby
