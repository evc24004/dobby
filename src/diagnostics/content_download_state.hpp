#pragma once

#include "diagnostics/types.hpp"

#include <string_view>

namespace dobby {

ContentDownloadStateEvidence decodeContentDownloadState(std::string_view json);

} // namespace dobby
