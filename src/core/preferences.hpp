#pragma once

#include <string>
#include <string_view>

namespace dobby {

struct DeveloperPreferences {
    bool autoPopup{true};
    bool entityHitboxes{true};
    bool chestEsp{true};
    bool oreEsp{true};
    bool networkMetricsOverlay{true};
    bool packetTrafficOverlay{true};

    bool operator==(const DeveloperPreferences&) const = default;
};

DeveloperPreferences parseDeveloperPreferences(
        std::string_view text, const DeveloperPreferences& fallback);
std::string serializeDeveloperPreferences(const DeveloperPreferences& preferences);

} // namespace dobby
