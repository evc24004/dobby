#include "ui/developer_ui.hpp"
#include "ui/entity_hitbox_overlay.hpp"
#include "ui/window_policy.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "diagnostics/report_builder.hpp"
#include "hooks/chest_esp_hook.hpp"
#include "hooks/entity_hitbox_hook.hpp"
#include "hooks/ore_esp_scanner.hpp"
#include "hooks/packet_traffic_hook.hpp"
#include "network/packet_names.hpp"
#include "platform/files.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"
#include "platform/preferences_store.hpp"

#include <array>
#include <string>

namespace dobby {
namespace {

constexpr char kViolationWindowTitle[] = "Dobby##dobby_violation_v3";
constexpr char kStatusWindowTitle[] = "Dobby##dobby_status_v2";

bool menuUnselected(void*) { return false; }
bool autoPopupSelected(void*) { return runtimeState().autoPopup(); }
bool entityHitboxesSelected(void*) { return runtimeState().entityHitboxes(); }
bool chestEspSelected(void*) { return runtimeState().chestEsp(); }
bool oreEspSelected(void*) { return runtimeState().oreEsp(); }
bool networkMetricsSelected(void*) { return runtimeState().networkMetricsOverlay(); }
bool packetTrafficSelected(void*) { return runtimeState().packetTrafficOverlay(); }

void windowClosed(void*) { verboseLine("developer window closed"); }

void logCopyResult(bool copied, std::string_view subject) {
    logLine(copied
            ? std::string("UI: copied ") + std::string(subject) + " to clipboard"
            : std::string("UI: clipboard unavailable; wrote ") + clipboardPath());
}

void copyLatestReport(void*) {
    const auto diagnostic = runtimeState().latestDiagnostic();
    if (!diagnostic) {
        logLine("UI: no packet violation is available to copy");
        return;
    }
    logCopyResult(copyToClipboard(diagnostic->report), "full diagnostic");
}

void copyLatestRawPacket(void*) {
    const auto diagnostic = runtimeState().latestDiagnostic();
    if (!diagnostic) {
        logLine("UI: no raw packet capture is available to copy");
        return;
    }
    const std::string raw = rawPacketHex(*diagnostic);
    if (raw.empty()) {
        logLine("UI: the latest violation has no correlated stream capture");
        return;
    }
    logCopyResult(copyToClipboard(raw), "raw packet bytes");
}

void copyStatus(void*) {
    logCopyResult(copyToClipboard(buildDeveloperStatus(runtimeState().snapshot())), "developer status");
}

void clearHistory(void*) {
    runtimeState().clearDiagnostics();
    writeFile(latestPath(), "", "w");
    logLine("UI: cleared Dobby session history");
}

void persistDeveloperPreferences() {
    if (!saveDeveloperPreferences(runtimeState().developerPreferences()))
        logLine("ERROR: could not save Dobby preferences");
}

void toggleAutoPopup(void*) {
    const bool enabled = runtimeState().toggleAutoPopup();
    persistDeveloperPreferences();
    logLine(enabled ? "UI: automatic violation popups enabled"
                    : "UI: automatic violation popups disabled");
}

void toggleHitboxes(void*) {
    static_cast<void>(toggleEntityHitboxes());
    persistDeveloperPreferences();
}

void toggleChestEsp(void*) {
    if (!runtimeState().chestEspAvailable()) {
        logLine("ERROR: chest ESP is unavailable");
        return;
    }
    const bool enabled = runtimeState().toggleChestEsp();
    persistDeveloperPreferences();
    logLine(enabled ? "UI: chest ESP enabled" : "UI: chest ESP disabled");
}

void toggleOreEsp(void*) {
    if (!runtimeState().oreEspAvailable()) {
        logLine("ERROR: ore ESP is unavailable");
        return;
    }
    const bool enabled = runtimeState().toggleOreEsp();
    if (enabled)
        requestClientOreRescan();
    persistDeveloperPreferences();
    logLine(enabled ? "UI: ore ESP enabled" : "UI: ore ESP disabled");
}

void toggleNetworkMetrics(void*) {
    const bool enabled = runtimeState().toggleNetworkMetricsOverlay();
    persistDeveloperPreferences();
    logLine(enabled ? "UI: network metrics overlay enabled"
                    : "UI: network metrics overlay disabled");
}

void togglePacketTraffic(void*) {
    if (!runtimeState().packetTrafficAvailable()) {
        logLine("ERROR: packet traffic overlay is unavailable");
        return;
    }
    const bool enabled = runtimeState().togglePacketTrafficOverlay();
    persistDeveloperPreferences();
    logLine(enabled ? "UI: packet traffic overlay enabled"
                    : "UI: packet traffic overlay disabled");
}

} // namespace

void showDeveloperStatus(void*) {
    resolveLauncherApi();
    if (!launcherWindowAvailable()) {
        logLine("ERROR: launcher window API is unavailable");
        return;
    }

    const auto snapshot = runtimeState().snapshot();
    const std::string hook =
            "Hook: " + snapshot.hookStatus + "\n"
            "Warning hook: " + (snapshot.hookInstalled ? "active" : "inactive") +
            "  |  stream probe: " + (snapshot.streamProbeInstalled ? "active" : "inactive");
    const std::string counters =
            "Minecraft " + std::string(kMinecraftVersion) + " / " + kAbi + "\n"
            "Violations: " + std::to_string(snapshot.totalViolations) +
            " total / " + std::to_string(snapshot.retainedViolations) + " retained";
    const std::string toggles =
            std::string("Auto popup: ") + (snapshot.autoPopup ? "on" : "off") +
            "  |  verbose: " + (snapshot.verbose ? "on" : "off");
    std::array<LauncherControl, 6> controls{
            textControl("DOBBY DEVELOPER CLIENT", 2),
            textControl(hook.c_str(), 1),
            textControl(counters.c_str(), 1),
            textControl(toggles.c_str(), 1),
            buttonControl("Copy developer status", copyStatus),
            buttonControl("Clear session history", clearHistory),
    };
    showLauncherWindow(kStatusWindowTitle, controls, windowClosed);
    applyDobbyWindowPolicy(kStatusWindowTitle);
}

void showLatestViolation(void*) {
    resolveLauncherApi();
    if (!launcherWindowAvailable()) {
        logLine("ERROR: launcher window API is unavailable");
        return;
    }

    const auto diagnostic = runtimeState().latestDiagnostic();
    if (!diagnostic) {
        showDeveloperStatus();
        return;
    }

    const std::string packet =
            std::string(packetName(diagnostic->packetId)) + " (" +
            std::to_string(diagnostic->packetId) + " / " + packetIdHex(diagnostic->packetId) + ")";
    const std::string status =
            std::string(violationTypeName(diagnostic->type)) + " / " +
            severityName(diagnostic->severity);
    const std::string boundary = streamFailureSummary(*diagnostic);
    std::array<LauncherControl, 6> controls{
            textControl(packet.c_str(), 2),
            textControl(status.c_str(), 1),
            textControl(diagnostic->context.c_str(), 1),
            textControl(boundary.c_str(), 1),
            buttonControl("Copy report", copyLatestReport),
            buttonControl("Copy raw bytes", copyLatestRawPacket),
    };

    // Non-modal avoids the launcher's persistent full-screen dim layer on close.
    showLauncherWindow(kViolationWindowTitle, controls, windowClosed);
    applyDobbyWindowPolicy(kViolationWindowTitle);
    logLine("UI: displayed packet violation window");
}

void registerDeveloperUi() {
    resolveLauncherApi();
    installDobbyWindowPolicy();
    const bool overlayReady = installEntityHitboxOverlay();
    const bool hitboxesReady = entityHitboxHookInstalled() && overlayReady;
    runtimeState().setEntityHitboxesAvailable(hitboxesReady);
    const bool chestEspReady = chestEspHookInstalled() && overlayReady;
    runtimeState().setChestEspAvailable(chestEspReady);
    const bool oreEspReady =
            chestEspHookInstalled() && oreEspScannerReady() && overlayReady;
    runtimeState().setOreEspAvailable(oreEspReady);
    const bool packetTrafficReady = packetTrafficHooksInstalled() && overlayReady;
    runtimeState().setPacketTrafficAvailable(packetTrafficReady);
    if (hitboxesReady)
        logLine(runtimeState().entityHitboxes()
                        ? "entity hitboxes enabled"
                        : "entity hitboxes disabled by saved preference");
    if (chestEspReady)
        logLine(runtimeState().chestEsp()
                        ? "chest ESP enabled"
                        : "chest ESP disabled by saved preference");
    if (oreEspReady)
        logLine(runtimeState().oreEsp()
                        ? "ore ESP enabled"
                        : "ore ESP disabled by saved preference");
    if (packetTrafficReady)
        logLine(runtimeState().packetTrafficOverlay()
                        ? "packet traffic overlay enabled"
                        : "packet traffic overlay disabled by saved preference");
    logLine(launcherWindowAvailable() ? "UI: launcher window API ready"
                                      : "ERROR: launcher window API unavailable");
    logLine(launcherClipboardAvailable() ? "UI: native clipboard ready"
                                         : "ERROR: native clipboard unavailable");
    if (!launcherMenuAvailable()) {
        logLine("ERROR: launcher menu API unavailable");
        return;
    }

    static std::array<LauncherMenuEntry, 6> subentries{
            LauncherMenuEntry{
                    "Entity hitboxes", nullptr, entityHitboxesSelected, toggleHitboxes, 0, nullptr},
            LauncherMenuEntry{
                    "Chest ESP", nullptr, chestEspSelected, toggleChestEsp, 0, nullptr},
            LauncherMenuEntry{
                    "Ore ESP", nullptr, oreEspSelected, toggleOreEsp, 0, nullptr},
            LauncherMenuEntry{
                    "Network metrics", nullptr, networkMetricsSelected,
                    toggleNetworkMetrics, 0, nullptr},
            LauncherMenuEntry{
                    "Packet traffic", nullptr, packetTrafficSelected,
                    togglePacketTraffic, 0, nullptr},
            LauncherMenuEntry{"Automatic popup", nullptr, autoPopupSelected, toggleAutoPopup, 0, nullptr},
    };
    static LauncherMenuEntry root{
            "Dobby", nullptr, menuUnselected, showDeveloperStatus,
            subentries.size(), subentries.data(),
    };
    static std::array<LauncherMenuEntry, 1> roots{root};
    addLauncherMenu(roots);
    logLine("UI: registered Mods > Dobby developer menu");
}

} // namespace dobby
#endif
