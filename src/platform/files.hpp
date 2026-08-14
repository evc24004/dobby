#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dobby {

std::string timestamp();
std::string jsonEscape(std::string_view value);
std::string hexBytes(std::span<const std::uint8_t> bytes);
bool writeFile(std::string_view path, std::string_view text, std::string_view mode);
bool writeFileAtomically(std::string_view path, std::string_view text);
std::optional<std::string> readFile(std::string_view path, std::size_t maximumBytes);
void ensureOutputDirectory();

const std::string& logPath();
const std::string& eventPath();
const std::string& latestPath();
const std::string& clipboardPath();
const std::string& preferencesPath();
const std::string& protocolPath();
const std::string& observedProtocolPath();
const std::string& protocolVersionPath();
const std::string& protocolStatusPath();
const std::string& protocolReferencePath();
const std::string& protocolVersionReferencePath();

} // namespace dobby
