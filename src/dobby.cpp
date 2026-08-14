#include "core/constants.hpp"
#include "core/runtime_state.hpp"
#include "hooks/chunk_metrics_hook.hpp"
#include "hooks/chest_esp_hook.hpp"
#include "hooks/packet_hooks.hpp"
#include "hooks/protocol_dump_hook.hpp"
#include "hooks/packet_traffic_hook.hpp"
#include "hooks/entity_hitbox_hook.hpp"
#include "hooks/network_metrics_hook.hpp"
#include "hooks/outbound_packet_hook.hpp"
#include "hooks/overlay_camera_hook.hpp"
#include "platform/log.hpp"

#include <atomic>
#include <string>

#if defined(__ANDROID__)
#include "ui/developer_ui.hpp"
#endif

namespace {

std::atomic_bool initialized{false};

} // namespace

extern "C" [[gnu::visibility("default")]] void mod_preinit() {
    dobby::logLine("mod_preinit");
}

extern "C" [[gnu::visibility("default")]] void mod_init() {
    dobby::logLine("mod_init");
    if (initialized.exchange(true))
        return;

    static_cast<void>(dobby::runtimeState());
    dobby::installPacketHooks();
#if defined(__ANDROID__)
    dobby::dumpProtocolOnStartup();
    dobby::installOutboundPacketHook();
    dobby::installNetworkMetricsHook();
    dobby::installPacketTrafficHooks();
    dobby::installChunkMetricsHooks();
    dobby::installEntityHitboxHook();
    dobby::installOverlayCameraHook();
    dobby::installChestEspHook();
    dobby::registerDeveloperUi();
#endif
}

[[gnu::constructor]] void dobbyLoaded() {
    static_cast<void>(dobby::runtimeState());
    dobby::logLine(std::string("library loaded: Dobby ") + dobby::kDobbyVersion);
    dobby::recordLifecycleEvent(
            "session_start",
            std::string("Minecraft ") + dobby::kMinecraftVersion + " / " + dobby::kAbi);
}
