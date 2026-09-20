#pragma once

#include "diagnostics/types.hpp"

#include <optional>

namespace dobby {

std::optional<ContentDownloadStateEvidence> captureContentDownloadState();

} // namespace dobby
