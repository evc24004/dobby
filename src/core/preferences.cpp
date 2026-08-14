#include "core/preferences.hpp"

#include "core/config.hpp"

#include <optional>

namespace dobby {
namespace {

constexpr std::string_view kPreferencesVersion = "1";

std::string_view trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

} // namespace

DeveloperPreferences parseDeveloperPreferences(
        std::string_view text, const DeveloperPreferences& fallback) {
    DeveloperPreferences parsed = fallback;
    std::optional<std::string_view> version;

    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = trim(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.empty() || line.front() == '#')
            continue;

        const auto separator = line.find('=');
        if (separator == std::string_view::npos)
            continue;
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key == "version")
            version = value;
        else if (key == "automatic_popup")
            parsed.autoPopup = parseBoolean(value, fallback.autoPopup);
        else if (key == "entity_hitboxes")
            parsed.entityHitboxes = parseBoolean(value, fallback.entityHitboxes);
        else if (key == "chest_esp")
            parsed.chestEsp = parseBoolean(value, fallback.chestEsp);
        else if (key == "ore_esp")
            parsed.oreEsp = parseBoolean(value, fallback.oreEsp);
        else if (key == "network_metrics")
            parsed.networkMetricsOverlay = parseBoolean(
                    value, fallback.networkMetricsOverlay);
        else if (key == "packet_traffic")
            parsed.packetTrafficOverlay = parseBoolean(
                    value, fallback.packetTrafficOverlay);
    }

    return version && *version == kPreferencesVersion ? parsed : fallback;
}

std::string serializeDeveloperPreferences(const DeveloperPreferences& preferences) {
    return std::string("version=") + std::string(kPreferencesVersion) +
            "\nautomatic_popup=" + (preferences.autoPopup ? "true" : "false") +
            "\nentity_hitboxes=" + (preferences.entityHitboxes ? "true" : "false") +
            "\nchest_esp=" + (preferences.chestEsp ? "true" : "false") +
            "\nore_esp=" + (preferences.oreEsp ? "true" : "false") +
            "\nnetwork_metrics=" +
            (preferences.networkMetricsOverlay ? "true" : "false") +
            "\npacket_traffic=" +
            (preferences.packetTrafficOverlay ? "true" : "false") + "\n";
}

} // namespace dobby
