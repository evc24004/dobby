#include "platform/local_capes.hpp"

#include "core/config.hpp"
#include "platform/files.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace dobby {
namespace {

constexpr std::string_view kIndexVersion{"version=2"};

bool validTitle(std::string_view value) {
    if (value.empty() || value.size() > 80)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t';
    });
}

std::vector<LocalCape> loadLocalCapes() {
    const auto index = readFile(outputPath("dobby-capes/index.tsv"), 16 * 1024);
    if (!index)
        return {};
    const auto descriptors = parseLocalCapeIndex(*index);
    if (!descriptors)
        return {};

    std::vector<LocalCape> result;
    result.reserve(descriptors->size());
    for (const auto& descriptor : *descriptors) {
        const auto pixels = readFile(
                outputPath("dobby-capes/" + descriptor.pieceId + ".rgba"),
                kCapeTextureBytes);
        if (!pixels || pixels->size() != kCapeTextureBytes)
            return {};
        LocalCape cape;
        cape.descriptor = descriptor;
        std::memcpy(cape.rgba.data(), pixels->data(), cape.rgba.size());
        result.push_back(std::move(cape));
    }
    return result;
}

} // namespace

bool validLocalCapeId(std::string_view value) {
    if (value.size() != 36)
        return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        const bool separator = index == 8 || index == 13 || index == 18 ||
                index == 23;
        if (separator) {
            if (character != '-')
                return false;
            continue;
        }
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<LocalCapeDescriptor>> parseLocalCapeIndex(
        std::string_view text) {
    const auto firstNewline = text.find('\n');
    if (firstNewline == std::string_view::npos ||
        text.substr(0, firstNewline) != kIndexVersion) {
        return std::nullopt;
    }

    std::vector<LocalCapeDescriptor> result;
    text.remove_prefix(firstNewline + 1);
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        text = newline == std::string_view::npos
                ? std::string_view{}
                : text.substr(newline + 1);
        if (line.empty())
            continue;
        const auto firstTab = line.find('\t');
        const auto secondTab = firstTab == std::string_view::npos
                ? std::string_view::npos
                : line.find('\t', firstTab + 1);
        if (firstTab == std::string_view::npos ||
            secondTab == std::string_view::npos ||
            line.find('\t', secondTab + 1) != std::string_view::npos) {
            return std::nullopt;
        }
        const auto pieceId = line.substr(0, firstTab);
        const auto packId = line.substr(
                firstTab + 1, secondTab - firstTab - 1);
        const auto title = line.substr(secondTab + 1);
        if (!validLocalCapeId(pieceId) || !validLocalCapeId(packId) ||
            !validTitle(title) ||
            result.size() >= kMaximumLocalCapes ||
            std::any_of(result.begin(), result.end(), [&](const auto& existing) {
                return existing.pieceId == pieceId || existing.packId == packId;
            })) {
            return std::nullopt;
        }
        result.push_back(LocalCapeDescriptor{
                std::string(pieceId), std::string(packId), std::string(title)});
    }
    return result.empty()
            ? std::nullopt
            : std::optional<std::vector<LocalCapeDescriptor>>(std::move(result));
}

const std::vector<LocalCape>& localCapes() {
    static const std::vector<LocalCape> value = loadLocalCapes();
    return value;
}

const LocalCape* findLocalCape(std::string_view pieceId) {
    const auto& capes = localCapes();
    const auto found = std::find_if(capes.begin(), capes.end(), [&](const auto& cape) {
        return cape.descriptor.pieceId == pieceId;
    });
    return found == capes.end() ? nullptr : &*found;
}

} // namespace dobby
