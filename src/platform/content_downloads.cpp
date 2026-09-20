#include "platform/content_downloads.hpp"

#include "core/config.hpp"
#include "diagnostics/content_download_state.hpp"
#include "platform/files.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace dobby {
namespace {

bool safeContentId(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
            std::ranges::all_of(value, [](unsigned char byte) {
                return std::isalnum(byte) != 0 || byte == '-';
            });
}

void addFileEvidence(
        const std::filesystem::path& directory,
        ContentDownloadEvidence& download) {
    if (!safeContentId(download.contentId))
        return;
    std::error_code error;
    const auto partial = directory / (download.contentId + ".partial");
    download.partialFilePresent = std::filesystem::is_regular_file(partial, error);
    if (download.partialFilePresent) {
        error.clear();
        download.partialBytes = std::filesystem::file_size(partial, error);
        if (error)
            download.partialFilePresent = false;
    }
    error.clear();
    const auto complete = directory / (download.contentId + ".complete");
    download.completeFilePresent = std::filesystem::is_regular_file(complete, error);
    if (download.completeFilePresent) {
        error.clear();
        download.completeBytes = std::filesystem::file_size(complete, error);
        if (error)
            download.completeFilePresent = false;
    }
}

} // namespace

std::optional<ContentDownloadStateEvidence> captureContentDownloadState() {
    const std::filesystem::path directory =
            std::filesystem::path(config().outputDirectory) /
            "minecraftpe" / "DownloadTemp";
    const auto state = readFile(
            (directory / "download_state.json").string(), 64 * 1024);
    if (!state)
        return std::nullopt;
    auto result = decodeContentDownloadState(*state);
    for (auto& download : result.downloads)
        addFileEvidence(directory, download);
    return result;
}

} // namespace dobby
