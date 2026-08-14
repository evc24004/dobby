#include "core/config.hpp"
#include "core/constants.hpp"
#include "core/preferences.hpp"
#include "core/runtime_state.hpp"
#include "diagnostics/client_schema_trace.hpp"
#include "diagnostics/protocol_dump.hpp"
#include "diagnostics/report_builder.hpp"
#include "diagnostics/stream_probe.hpp"
#include "diagnostics/violation_decoder.hpp"
#include "hooks/render_camera.hpp"
#include "hooks/overlay_camera_hook.hpp"
#include "metrics/chunk_metrics_layout.hpp"
#include "metrics/client_performance.hpp"
#include "network/packet_names.hpp"
#include "metrics/network_metrics.hpp"
#include "metrics/packet_traffic.hpp"
#include "platform/preferences_store.hpp"
#include "platform/safe_memory.hpp"
#include "ui/chest_esp.hpp"
#include "ui/entity_hitbox_overlay.hpp"
#include "ui/network_metrics_overlay.hpp"
#include "ui/packet_traffic_overlay.hpp"
#include "ui/ore_esp.hpp"
#include "ui/window_policy.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

template <class T>
void store(std::byte* destination, T value) {
    std::memcpy(destination, &value, sizeof(value));
}

void testViolationDecoder() {
    std::array<std::byte, 0x80> packet{};
    store<std::int32_t>(packet.data() + dobby::kViolationTypeOffset, 0);
    store<std::int32_t>(packet.data() + dobby::kViolationSeverityOffset, 2);
    store<std::int32_t>(packet.data() + dobby::kViolationPacketIdOffset, 50);

    const std::string shortContext = "BinaryStream read() incomplete";
    packet[dobby::kViolationContextOffset] =
            static_cast<std::byte>(shortContext.size() << 1U);
    std::memcpy(packet.data() + dobby::kViolationContextOffset + 1,
                shortContext.data(), shortContext.size());
    const auto shortRecord = dobby::decodeViolation(packet.data());
    assert(shortRecord);
    assert(shortRecord->type == 0);
    assert(shortRecord->severity == 2);
    assert(shortRecord->packetId == 50);
    assert(shortRecord->context == shortContext);
    assert(shortRecord->contextStorage == "short");

    const std::string longContext(80, 'x');
    packet[dobby::kViolationContextOffset] = std::byte{1};
    store<std::size_t>(packet.data() + dobby::kViolationContextOffset + 8, longContext.size());
    store<const char*>(packet.data() + dobby::kViolationContextOffset + 16, longContext.data());
    const auto longRecord = dobby::decodeViolation(packet.data());
    assert(longRecord);
    assert(longRecord->context == longContext);
    assert(longRecord->contextStorage == "long");
}

void testStreamProbeAndReport() {
    const std::array<std::uint8_t, 6> body{0x01, 0x02, 0x03, 0x04, 0xaa, 0xbb};
    std::array<std::byte, 0x48> stream{};
    store<const std::uint8_t*>(stream.data() + dobby::kStreamViewDataOffset, body.data());
    store<std::size_t>(stream.data() + dobby::kStreamViewSizeOffset, body.size());
    store<std::size_t>(stream.data() + dobby::kStreamReadPointerOffset, 0);
    stream[dobby::kStreamOverflowOffset] = std::byte{0};

    dobby::clearStreamProbe();
    dobby::captureStreamReadAttempt(stream.data(), 2, 2048);
    store<std::size_t>(stream.data() + dobby::kStreamReadPointerOffset, 2);
    dobby::captureStreamReadAttempt(stream.data(), 2, 2048);
    store<std::size_t>(stream.data() + dobby::kStreamReadPointerOffset, 4);
    dobby::captureStreamReadAttempt(stream.data(), 4, 2048);
    auto failure = dobby::recentStreamFailure(std::chrono::seconds(1));
    assert(failure);
    assert(failure->overflowObserved);
    assert(failure->failureOffset == 4);
    assert(failure->viewSize == 6);
    assert(failure->requested == 4);
    assert(failure->available == 2);
    assert(failure->rawBytes.size() == body.size());
    assert(failure->attempts.size() == 3);
    assert(!failure->attempts.front().overflow);
    assert(failure->attempts.back().overflow);

    dobby::ViolationRecord record{0, 2, 50, "read incomplete", "short"};
    auto diagnostic = dobby::buildDiagnostic(record, std::move(failure), "unit test");
    assert(diagnostic.report.find("Decode: ReadOnlyBinaryStream::read") != std::string::npos);
    assert(diagnostic.json.find("\"offset\":4") != std::string::npos);
    assert(dobby::rawPacketHex(diagnostic) == "01 02 03 04 aa bb");

    dobby::clearStreamProbe();
    store<std::size_t>(stream.data() + dobby::kStreamReadPointerOffset, 0);
    dobby::captureStreamReadAttempt(stream.data(), 2, 2048);
    store<std::size_t>(stream.data() + dobby::kStreamReadPointerOffset, 2);
    dobby::captureStreamReadAttempt(stream.data(), 2, 2048);
    auto correlated = dobby::recentStreamFailure(std::chrono::seconds(1));
    assert(correlated);
    assert(!correlated->overflowObserved);
    assert(correlated->rawBytes.size() == body.size());
    assert(correlated->attempts.size() == 2);
    auto correlatedDiagnostic = dobby::buildDiagnostic(
            record, std::move(correlated), "unit test");
    assert(correlatedDiagnostic.report.find("Decode boundary: unconfirmed") != std::string::npos);
    assert(correlatedDiagnostic.json.find("\"overflow_observed\":false") != std::string::npos);

    dobby::clearStreamProbe();
    store<std::size_t>(stream.data() + dobby::kStreamReadPointerOffset, 4);
    dobby::capturePacketEndCheck(stream.data(), 2048);
    auto trailing = dobby::recentStreamFailure(std::chrono::seconds(1));
    assert(trailing);
    assert(trailing->packetEndMismatch);
    assert(trailing->failureOffset == 4);
    assert(trailing->available == 2);
    auto trailingDiagnostic = dobby::buildDiagnostic(record, std::move(trailing), "unit test");
    assert(trailingDiagnostic.report.find("Decode: ReadOnlyBinaryStream::ensureReadCompleted") != std::string::npos);
    assert(trailingDiagnostic.report.find("_read: success | cursor 4/6 | remaining 2 | overflow no") != std::string::npos);
    assert(trailingDiagnostic.json.find("\"kind\":\"unconsumed_trailing_bytes\"") != std::string::npos);
    assert(trailingDiagnostic.report.find("INFER") == std::string::npos);
    assert(trailingDiagnostic.report.find("Likely") == std::string::npos);
    assert(trailingDiagnostic.json.find("expected_schema") == std::string::npos);
    assert(trailingDiagnostic.json.find("inferred_divergence") == std::string::npos);
}

void testClientSchemaFieldTrace() {
    const std::array<std::uint8_t, 2> body{0xaa, 0xbb};
    std::array<std::byte, 0x48> stream{};
    store<const std::uint8_t*>(stream.data() + dobby::kStreamViewDataOffset, body.data());
    store<std::size_t>(stream.data() + dobby::kStreamViewSizeOffset, body.size());

    dobby::clearStreamProbe();
    dobby::pushClientSchemaMember("item");
    dobby::pushClientSchemaMember("stackNetworkId");
    dobby::captureStreamReadAttempt(stream.data(), 1, 2048);
    dobby::popClientSchemaContext();
    dobby::popClientSchemaContext();
    store<std::size_t>(stream.data() + dobby::kStreamReadPointerOffset, 1);
    dobby::captureStreamReadAttempt(stream.data(), 2, 2048);

    auto failure = dobby::recentStreamFailure(std::chrono::seconds(1));
    assert(failure);
    assert(failure->attempts.front().clientField == "item.stackNetworkId");
    assert(failure->attempts.back().clientField.empty());
    auto diagnostic = dobby::buildDiagnostic(
            {0, 2, 50, "read incomplete", "short"}, std::move(failure), "unit test");
    assert(diagnostic.json.find("\"client_field\":\"item.stackNetworkId\"") != std::string::npos);
}

void testProtocolDumpCompilation() {
    require(dobby::protocolPacketName("SetTimePacket") == "set_time");
    require(dobby::protocolPacketName("NPCDialoguePacket") == "npc_dialogue");
    require(dobby::protocolPacketName("ClientboundDataDrivenUIShowScreenPacket") ==
            "clientbound_data_driven_ui_show_screen");

    dobby::ProtocolDumpObservation dump{
            "1.26.40.5", "test-build", 2168, 351,
            {
                    {10,
                     "SetTimePacket",
                     0,
                     true,
                     true,
                     {},
                     {{"time", "zigzag32"}}},
                    {11,
                     "StartGamePacket",
                     5,
                     true,
                     false,
                     "default_collection_does_not_prove_element_schema",
                     {{"level_settings.seed", "lu64"}}},
                    {300,
                     "CameraInstructionPacket",
                     5,
                     false,
                     false,
                     "write_failed",
                     {}},
            }};

    const auto protocol = dobby::buildProtocolJson(dump);
    require(protocol.find("\"10\": \"set_time\"") != std::string::npos);
    require(protocol.find("\"set_time\": \"packet_set_time\"") !=
            std::string::npos);
    require(protocol.find("\"packet_set_time\"") != std::string::npos);
    require(protocol.find("\"name\":\"time\",\"type\":\"zigzag32\"") !=
            std::string::npos);
    require(protocol.find("\"packet_start_game\"") != std::string::npos);
    require(protocol.find("\"name\":\"unresolved\",\"type\":\"restBuffer\"") !=
            std::string::npos);

    const auto version = dobby::buildProtocolVersionJson(dump);
    require(version.find("\"version\": 2168") != std::string::npos);
    require(version.find("\"minecraftVersion\": \"1.26.40\"") !=
            std::string::npos);
    require(version.find("\"majorVersion\": \"1.26\"") !=
            std::string::npos);

    const auto status = dobby::buildProtocolStatusJson(dump, false);
    require(status.find("\"factory_packets\": 3") != std::string::npos);
    require(status.find("\"serialized_packets\": 2") != std::string::npos);
    require(status.find("\"branch_free_observed_packets\": 1") !=
            std::string::npos);
    require(status.find("\"reference_packets\": 244") != std::string::npos);
    require(status.find("\"complete_packets\": 0") != std::string::npos);
    require(status.find("\"reference_verified\": false") != std::string::npos);
    const auto verifiedStatus = dobby::buildProtocolStatusJson(dump, true);
    require(verifiedStatus.find("\"complete_packets\": 244") !=
            std::string::npos);
    require(verifiedStatus.find("\"reference_verified\": true") !=
            std::string::npos);
    require(status.find("default_collection_does_not_prove_element_schema") !=
            std::string::npos);
}

void testRepeatViolationsAreRetained() {
    auto& state = dobby::runtimeState();
    state.clearDiagnostics();
    dobby::Diagnostic first;
    first.packetId = 50;
    first.context = "same failure";
    state.addDiagnostic(first);
    state.addDiagnostic(first);
    const auto snapshot = state.snapshot();
    assert(snapshot.totalViolations == 2);
    assert(snapshot.retainedViolations == 2);
    state.clearDiagnostics();
}

void testEntityHitboxState() {
    auto& state = dobby::runtimeState();
    state.setEntityHitboxesAvailable(false);
    state.setEntityHitboxes(false);
    assert(!state.entityHitboxesAvailable());
    assert(!state.entityHitboxes());
    state.setEntityHitboxesAvailable(true);
    state.setEntityHitboxes(true);
    assert(state.entityHitboxesAvailable());
    assert(state.entityHitboxes());
    state.setEntityHitboxes(false);
    state.setChestEspAvailable(true);
    const bool chestInitiallyVisible = state.chestEsp();
    require(state.toggleChestEsp() != chestInitiallyVisible);
    require(state.toggleChestEsp() == chestInitiallyVisible);
    const bool oreInitiallyVisible = state.oreEsp();
    if (state.chestEsp())
        static_cast<void>(state.toggleChestEsp());
    if (state.oreEsp())
        static_cast<void>(state.toggleOreEsp());
    require(!state.entityHitboxes());
    require(!state.chestEsp());
    require(!state.oreEsp());
    require(!state.anyEspEnabled());
    state.setEntityHitboxes(true);
    require(state.anyEspEnabled());
    state.setEntityHitboxes(false);
    if (chestInitiallyVisible)
        static_cast<void>(state.toggleChestEsp());
    if (oreInitiallyVisible)
        static_cast<void>(state.toggleOreEsp());
    require(state.anyEspEnabled() ==
            (chestInitiallyVisible || oreInitiallyVisible));
    const bool metricsInitiallyVisible = state.networkMetricsOverlay();
    assert(state.toggleNetworkMetricsOverlay() != metricsInitiallyVisible);
    assert(state.toggleNetworkMetricsOverlay() == metricsInitiallyVisible);
    const bool packetTrafficInitiallyVisible = state.packetTrafficOverlay();
    assert(state.togglePacketTrafficOverlay() !=
           packetTrafficInitiallyVisible);
    assert(state.togglePacketTrafficOverlay() ==
           packetTrafficInitiallyVisible);
    state.setPacketTrafficAvailable(true);
    assert(state.packetTrafficAvailable());
    static_cast<void>(metricsInitiallyVisible);
    static_cast<void>(packetTrafficInitiallyVisible);
    static_cast<void>(chestInitiallyVisible);
    static_cast<void>(oreInitiallyVisible);
}

void testEntityProjection() {
    static_assert(sizeof(dobby::EntityAabb) == 24);
    static_assert(dobby::target::kActorGetAabbOffset == 0x0ec86fb4);
    static_assert(dobby::target::kActorGetAabbSignature[1] == 0x08);
    static_assert(dobby::target::kCameraProjectionStackOffset == 0x90);
    static_assert(dobby::target::kCameraRightOffset == 0x118);
    static_assert(dobby::target::kCameraPositionOffset == 0x13c);
    static_assert(dobby::target::kViewMatrixGetterOffset == 0x0a5d8ba4);
    static_assert(dobby::target::kCameraPositionGetterOffset == 0x0a5d8b70);
    static_assert(dobby::target::kActorLevelOffset == 0x1d0);
    static_assert(dobby::target::kActorGetLevelOffset == 0x0eca7920);
    static_assert(
            dobby::target::kLevelRenderFrameOffset == 0x0ae0aba8);
    static_assert(
            dobby::target::kLevelRenderFrameVtableSlotOffset ==
            0x11fbe330);
    static_assert(
            dobby::target::kLevelRenderFrameSignature[0] == 0xff);
    static_assert(
            dobby::target::kLevelRendererPlayerVtableOffset ==
            0x11fbe270);
    static_assert(
            dobby::target::kLevelRenderCameraPointerOffset == 0x18);
    static_assert(
            dobby::target::kLevelRenderCameraPointerProbeOffset ==
            0x0ae1a1bc);
    static_assert(
            dobby::target::kLevelRenderCameraPointerProbeSignature[0] ==
            0xc0);
    static_assert(
            dobby::target::kLevelRenderCameraCaptureOffset ==
            0x0ae1a1c4);
    static_assert(
            dobby::target::kLevelRenderCameraCaptureSignature[0] ==
            0x68);
    static_assert(
            dobby::target::kLevelRendererCameraPositionOffset == 0x6f4);
    static_assert(
            dobby::target::kLevelRendererCameraPositionUseProbeOffset ==
            0x0ae0ad30);
    static_assert(
            dobby::target::kLevelRendererCameraPositionUseProbeSignature[0] ==
            0x01);
    static_assert(dobby::target::kLevelRendererLevelOffset == 0x958);
    static_assert(
            dobby::target::kLevelRendererLevelLayoutProbeOffset ==
            0x0ae2354c);
    static_assert(
            dobby::target::kLevelRendererLevelLayoutProbeSignature[0] ==
            0x76);
    static_assert(
            dobby::target::kLevelRendererLevelUseProbeOffset ==
            0x0ae23c64);
    static_assert(
            dobby::target::kLevelRendererLevelUseProbeSignature[0] ==
            0x60);
    static_assert(
            static_cast<std::size_t>(
                    dobby::RenderCameraCaptureFailure::count) <= 32);
    static_assert(
            dobby::renderCameraCaptureFailureName(
                    dobby::RenderCameraCaptureFailure::cameraPositionUnavailable) ==
            "camera_position");
    static_assert(dobby::target::kLevelGetRuntimeActorListOffset == 0x0f226d10);
    static_assert(dobby::target::kLevelGetRuntimeActorListVtableSlot == 326);
    static_assert(dobby::target::kLevelForEachPlayerOffset == 0x0f225e4c);
    static_assert(dobby::target::kLevelForEachPlayerVtableSlot == 223);
    static_assert(dobby::target::kLevelGetPrimaryLocalPlayerOffset == 0x0f225818);
    static_assert(dobby::target::kLevelGetPrimaryLocalPlayerVtableSlot == 77);
    static_assert(dobby::target::kClientLevelVtableOffset == 0x11ed28b0);
    require(dobby::entityHitboxObservedForPresentation(1, 0));
    require(dobby::entityHitboxObservedForPresentation(8, 0));
    require(!dobby::entityHitboxObservedForPresentation(9, 0));
    require(!dobby::entityHitboxObservedForPresentation(24, 25));
    require(dobby::shouldUseCompactEspMarker(8.0F, 8.0F));
    require(dobby::shouldUseCompactEspMarker(3.0F, 7.0F));
    require(!dobby::shouldUseCompactEspMarker(9.0F, 3.0F));
    require(!dobby::shouldUseCompactEspMarker(-1.0F, 3.0F));
    require(!dobby::shouldUseCompactEspMarker(
            std::numeric_limits<float>::quiet_NaN(), 3.0F));
    const dobby::CameraFrame camera{
            {0.0F, 0.0F, 0.0F},
            {{1.0F, 0.0F, 0.0F, 0.0F,
              0.0F, 1.0F, 0.0F, 0.0F,
              0.0F, 0.0F, 1.0F, 0.0F,
              0.0F, 0.0F, 0.0F, 1.0F}},
            {{0.5625F, 0.0F, 0.0F, 0.0F,
              0.0F, 1.0F, 0.0F, 0.0F,
              0.0F, 0.0F, -1.0F, -1.0F,
              0.0F, 0.0F, -0.2F, 0.0F}},
    };
    auto zeroViewCamera = camera;
    zeroViewCamera.view.fill(0.0F);
    require(!dobby::validCameraFrame(zeroViewCamera));
    auto impossiblePositionCamera = camera;
    impossiblePositionCamera.position.x = 1.0e20F;
    require(!dobby::validCameraFrame(impossiblePositionCamera));
    const std::array observations{
            dobby::EntityHitboxObservation{
                    reinterpret_cast<const void*>(0x2000),
                    {{-0.5F, 0.0F, -5.5F}, {0.5F, 1.0F, -4.5F}}},
            dobby::EntityHitboxObservation{
                    reinterpret_cast<const void*>(0x3000),
                    {{0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 1.0F}}},
    };
    const auto submitted = dobby::submitEntityHitboxFrame(
            reinterpret_cast<const void*>(0x1000), observations);
    require(submitted.accepted == 1);
    require(submitted.invalid == 1);
    const void* cameraLevel = nullptr;
    dobby::CameraFrame retainedCamera{};
    std::uint64_t missedFrames = 0;
    require(!dobby::currentOverlayCamera(
            0, cameraLevel, retainedCamera, &missedFrames));
    auto refreshedCamera = camera;
    refreshedCamera.position = {4.0F, 5.0F, 6.0F};
    require(dobby::submitOverlayCameraFrame(
            reinterpret_cast<const void*>(0x1000), refreshedCamera));
    require(dobby::currentOverlayCamera(
            0, cameraLevel, retainedCamera, &missedFrames));
    require(retainedCamera.position.x == 4.0F);
    require(retainedCamera.position.y == 5.0F);
    require(retainedCamera.position.z == 6.0F);
    require(dobby::submitEntityHitboxFrame(
                    reinterpret_cast<const void*>(0x9000), observations)
                    .accepted == 1);
    require(dobby::currentOverlayCamera(
            0, cameraLevel, retainedCamera, &missedFrames));
    require(cameraLevel == reinterpret_cast<const void*>(0x1000));
    require(retainedCamera.position.x == 4.0F);
    require(!dobby::submitOverlayCameraFrame(
            reinterpret_cast<const void*>(0x1000),
            impossiblePositionCamera));
    require(dobby::currentOverlayCamera(
            0, cameraLevel, retainedCamera, &missedFrames));
    require(retainedCamera.position.x == 4.0F);
    require(!dobby::submitOverlayCameraFrame(nullptr, refreshedCamera));
    dobby::ScreenPoint center{};
    const bool centerProjected = dobby::projectWorldPoint(
            camera, {0.0F, 0.0F, -5.0F}, 1920.0F, 1080.0F, center);
    assert(centerProjected);
    assert(center.x == 960.0F);
    assert(center.y == 540.0F);
    require(dobby::worldPointWithinViewport(
            camera, {0.0F, 0.0F, -5.0F}));
    require(!dobby::worldPointWithinViewport(
            camera, {100.0F, 0.0F, -5.0F}));

    dobby::ScreenPoint right{};
    const bool rightProjected = dobby::projectWorldPoint(
            camera, {1.0F, 0.0F, -5.0F}, 1920.0F, 1080.0F, right);
    assert(rightProjected);
    assert(right.x > center.x);
    dobby::ScreenPoint behind{};
    const bool behindProjected = dobby::projectWorldPoint(
            camera, {0.0F, 0.0F, 1.0F}, 1920.0F, 1080.0F, behind);
    assert(!behindProjected);
    dobby::ScreenPoint clippedFirst{};
    dobby::ScreenPoint clippedSecond{};
    bool nearPlaneClipped = false;
    const bool crossingProjected = dobby::projectWorldSegment(
            camera, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -5.0F},
            1920.0F, 1080.0F, clippedFirst, clippedSecond,
            nearPlaneClipped);
    require(crossingProjected);
    require(nearPlaneClipped);
    require(std::isfinite(clippedFirst.x));
    require(std::isfinite(clippedFirst.y));
    bool rejectedSegmentClipped = false;
    require(!dobby::projectWorldSegment(
            camera, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 2.0F},
            1920.0F, 1080.0F, clippedFirst, clippedSecond,
            rejectedSegmentClipped));
    const float projection[16]{
            0.5625F, 0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, -1.0F, -1.0F,
            0.0F, 0.0F, -0.2F, 0.0F,
    };
    static_cast<void>(centerProjected);
    static_cast<void>(rightProjected);
    static_cast<void>(behindProjected);
    static_cast<void>(center);
    static_cast<void>(right);
    static_cast<void>(behind);
    static_cast<void>(projection);
}

void testChestEspRegistry() {
    static_assert(dobby::target::kLevelChunkPositionOffset == 0x50);
    static_assert(dobby::target::kLevelChunkLevelOffset == 0x28);
    static_assert(dobby::target::kBlockActorPositionOffset == 0x08);
    static_assert(
            dobby::target::kChestBlockActorDestructorVtableSlot == 0);
    static_assert(
            dobby::target::kChestBlockActorDeletingDestructorVtableSlot == 1);

    require(dobby::subChunkStorageIndex(0, 0, 0) == 0);
    require(dobby::subChunkStorageIndex(1, 2, 3) == 0x132);
    require(dobby::subChunkStorageIndex(15, 15, 15) == 0x0fff);
    require(dobby::worldBlockPosition({-2, 7}, -4, 3, 5, 9) ==
            dobby::BlockPosition{-29, -59, 121});

    dobby::ChunkChestRegistry registry(2, 3);
    const auto* level = reinterpret_cast<const void*>(0x1000);
    require(registry.replaceSubChunk(
                    level, {-2, 7}, -4,
                    {{-29, -59, 121}, {-20, -58, 126}}) ==
            dobby::ChunkChestUpdateResult::accepted);
    require(registry.replaceSubChunk(
                    level, {-1, 7}, -4, {{-15, -60, 112}}) ==
            dobby::ChunkChestUpdateResult::accepted);
    require(registry.snapshot(level).size() == 3);
    require(registry.sizeForLevel(level) == 3);
    require(registry.sizeForLevel(
                    reinterpret_cast<const void*>(0x2000)) == 0);
    require(registry.snapshot(level).size() == 3);

    require(registry.replaceSubChunk(
                    level, {-2, 7}, -4, {{-28, -57, 122}}) ==
            dobby::ChunkChestUpdateResult::accepted);
    auto positions = registry.snapshot(level);
    require(positions.size() == 2);
    require(std::find(positions.begin(), positions.end(),
                      dobby::BlockPosition{-29, -59, 121}) == positions.end());
    require(std::find(positions.begin(), positions.end(),
                      dobby::BlockPosition{-28, -57, 122}) != positions.end());

    require(registry.replaceSubChunk(
                    level, {0, 7}, -4, {{0, -60, 112}}) ==
            dobby::ChunkChestUpdateResult::chunkCapacityReached);
    require(registry.replaceSubChunk(
                    level, {-2, 7}, -3,
                    {{-31, -48, 112}, {-30, -48, 112}}) ==
            dobby::ChunkChestUpdateResult::positionCapacityReached);
    require(registry.replaceSubChunk(
                    level, {-2, 7}, -3, {{30'000'001, 64, 0}}) ==
            dobby::ChunkChestUpdateResult::invalidInput);

    registry.removeChunk(level, {-2, 7});
    require(registry.snapshot(level).size() == 1);
    require(registry.snapshot(reinterpret_cast<const void*>(0x2000)).empty());
    registry.clear();
    require(registry.size() == 0);

    require(dobby::chunkPositionForBlock({-1, -1, 127}) ==
            dobby::ChunkPosition{-1, 7});
    require(dobby::absoluteSubChunkForBlock({-1, -1, 127}) == -1);
    require(registry.add(level, {-1, -1, 127}) ==
            dobby::ChunkChestUpdateResult::accepted);
    require(registry.add(level, {-1, -1, 127}) ==
            dobby::ChunkChestUpdateResult::accepted);
    require(registry.size() == 1);
    registry.remove(level, {-1, -1, 127});
    require(registry.size() == 0);
    require(registry.add(nullptr, {-1, -1, 127}) ==
            dobby::ChunkChestUpdateResult::invalidInput);
}

void testNetworkMetrics() {
    dobby::NetworkMetricsTracker metrics;
    metrics.recordPing(48, 42, 1000);
    auto snapshot = metrics.snapshot(1000);
    assert(snapshot.connected);
    assert(snapshot.pingMilliseconds == 42);
    assert(!snapshot.observedTicksPerSecond);

    for (std::uint64_t elapsed = 0; elapsed <= 1500; elapsed += 100)
        metrics.recordServerTick(100 + elapsed / 50, 1000 + elapsed);
    metrics.recordChunk(1800);
    metrics.recordChunk(2400);
    metrics.recordChunkLoaded(0x1000, -2, 7);
    metrics.recordChunkLoaded(0x1000, -2, 7);
    metrics.recordChunkLoaded(0x1000, -1, 7);
    metrics.recordChunkUnloaded(0x1000, 99, 99);
    dobby::setOutstandingChunkMetricsAvailable(true);
    metrics.recordSubChunkRequest(12);
    metrics.recordSubChunkResponse(5);
    snapshot = metrics.snapshot(2500);
    assert(snapshot.observedTicksPerSecond);
    assert(*snapshot.observedTicksPerSecond == 20.0);
    assert(snapshot.loadedChunks == 2);
    assert(snapshot.chunksPerSecond == 2);
    assert(snapshot.outstandingSubChunkRequests == 7);

    const auto text = dobby::formatNetworkMetrics(snapshot);
    assert(text.visible);
    assert(text.ping == "PING 42 MS");
    assert(text.observedTps == "TPS~ 20.0");
    assert(text.chunks == "CHUNKS 2 (2/S)");
    assert(text.pending == "PENDING 7");
    metrics.recordChunkUnloaded(0x1000, -2, 7);
    assert(metrics.snapshot(2500).loadedChunks == 1);

    dobby::NetworkMetricsTracker noChunkTraffic;
    for (std::uint64_t elapsed = 0; elapsed <= 300'000; elapsed += 100) {
        if (elapsed % 1'000 == 0)
            noChunkTraffic.recordPing(35, 35, 300'000 + elapsed);
        noChunkTraffic.recordServerTick(
                6'000 + elapsed / 50, 300'000 + elapsed);
    }
    const auto noChunkSnapshot = noChunkTraffic.snapshot(600'000);
    require(noChunkSnapshot.observedTicksPerSecond == 20.0);
    require(noChunkSnapshot.loadedChunks == 0);
    require(noChunkSnapshot.chunksPerSecond == 0);
    noChunkTraffic.recordChunkLoaded(0x1000, 4, 7);
    noChunkTraffic.resetWorld();
    const auto resetWorldSnapshot = noChunkTraffic.snapshot(600'000);
    require(resetWorldSnapshot.connected);
    require(!resetWorldSnapshot.observedTicksPerSecond);
    require(resetWorldSnapshot.loadedChunks == 0);
    const auto geometry = dobby::buildNetworkMetricsGeometry(
            snapshot, 1280.0F, 720.0F);
    assert(!geometry.shadowVertices.empty());
    assert(!geometry.pingVertices.empty());
    assert(!geometry.tpsVertices.empty());
    assert(!geometry.chunkVertices.empty());
    assert(!geometry.pendingVertices.empty());

    assert(!metrics.snapshot(5001).connected);
    metrics.recordServerTick(1, 5100);
    assert(metrics.retainedTickSamples() == 1);
    for (std::uint64_t index = 1; index < 100; ++index)
        metrics.recordServerTick(index + 1, 5100 + index * 50);
    assert(metrics.retainedTickSamples() <= 64);

    metrics.reset();
    assert(metrics.snapshot(5001).loadedChunks == 0);
    metrics.recordPing(-1, -1, 9000);
    assert(!metrics.snapshot(9000).connected);
    const auto hidden = dobby::formatNetworkMetrics(metrics.snapshot(9000));
    assert(!hidden.visible);

    static_assert(dobby::target::kLevelGetCurrentServerTickOffset == 0x09ad9014);
    static_assert(dobby::target::kLevelGetCurrentServerTickVtableSlot == 81);
    static_assert(dobby::target::kRakNetPeerUpdateOffset == 0x0c2bda48);
    static_assert(dobby::target::kRakNetPeerLastPingOffset == 0x104);
    static_assert(dobby::target::kRakNetPeerAveragePingOffset == 0x108);
    static_assert(dobby::target::kLevelChunkDispatcherOffset == 0x0c2b88e4);
    static_assert(dobby::target::kLevelChunkDispatcherVtableSlotOffset == 0x1209f3c0);
    static_assert(dobby::target::kSubChunkDispatcherOffset == 0x0c2bb704);
    static_assert(dobby::target::kSubChunkDispatcherVtableSlotOffset == 0x120a3080);
    static_assert(dobby::target::kLoopbackSendOffset == 0x0c2de4a4);
    static_assert(dobby::target::kLoopbackSendVtableSlotOffset == 0x120a55a8);
    static_assert(dobby::target::kSubChunkRequestVectorBeginOffset == 0x38);
    static_assert(dobby::target::kSubChunkRequestVectorEndOffset == 0x40);
    static_assert(dobby::target::kSubChunkPositionSize == 12);
    static_assert(dobby::target::kSubChunkPacketDataSize == 576);

    assert(dobby::boundedVectorElementCount(0, 0, 12, 4096) == 0);
    assert(dobby::boundedVectorElementCount(0x1000, 0x1030, 12, 4096) == 4);
    assert(!dobby::boundedVectorElementCount(0x1000, 0x102f, 12, 4096));
    assert(!dobby::boundedVectorElementCount(0x1030, 0x1000, 12, 4096));
    assert(!dobby::boundedVectorElementCount(0, 0x1000, 12, 4096));
    assert(!dobby::boundedVectorElementCount(0x1000, 0x100c, 0, 4096));
    assert(!dobby::boundedVectorElementCount(0x1000, 0xd00c, 12, 4096));
    dobby::setOutstandingChunkMetricsAvailable(false);
}

void testClientPerformanceMetrics() {
    dobby::ClientPerformanceTracker performance;
    for (std::uint64_t frame = 0; frame <= 100; ++frame) {
        const bool memorySampleDue =
                performance.recordPresentation(frame * 10'000);
        require(memorySampleDue == (frame == 0 || frame == 100));
    }
    performance.recordResidentBytes(768ULL * 1024ULL * 1024ULL);
    const auto snapshot = performance.snapshot(1'000'000);
    require(snapshot.framesPerSecond.has_value());
    require(std::fabs(*snapshot.framesPerSecond - 100.0) < 0.001);
    require(snapshot.residentBytes == 768ULL * 1024ULL * 1024ULL);
    require(performance.retainedPresentationSamples() == 101);

    dobby::NetworkMetricsSnapshot disconnected;
    const auto text = dobby::formatNetworkMetrics(disconnected, snapshot);
    require(text.visible);
    require(text.ping.empty());
    require(text.framesPerSecond == "FPS 100");
    require(text.residentMemory == "MEM 768 MB");
    const auto geometry = dobby::buildNetworkMetricsGeometry(
            disconnected, 1280.0F, 720.0F, snapshot);
    require(!geometry.shadowVertices.empty());
    require(!geometry.fpsVertices.empty());
    require(!geometry.memoryVertices.empty());

    dobby::ClientPerformanceSnapshot gigabytes;
    gigabytes.residentBytes = 1536ULL * 1024ULL * 1024ULL;
    require(dobby::formatNetworkMetrics(disconnected, gigabytes)
                    .residentMemory == "MEM 1.5 GB");
    require(!performance.snapshot(1'300'001).framesPerSecond);

    for (std::uint64_t frame = 101; frame < 2'000; ++frame)
        performance.recordPresentation(frame * 1'000);
    require(performance.retainedPresentationSamples() <= 512);
}

void testConfigurationAndPacketCatalog() {
    assert(dobby::parseBoolean("true", false));
    assert(!dobby::parseBoolean("off", true));
    assert(dobby::parseBoolean("invalid", true));
    assert(dobby::parseBoundedSize("0", 100, 1, 1000) == 1);
    assert(dobby::parseBoundedSize("2000", 100, 1, 1000) == 1000);
    assert(dobby::parseBoundedSize("bad", 100, 1, 1000) == 100);

    static_assert(dobby::packetName(50) == "InventorySlot");
    static_assert(dobby::packetName(156) == "PacketViolationWarning");
    static_assert(dobby::packetName(344) == "SyncWorldClocks");
    static_assert(dobby::packetName(9999) == "UnknownPacket");
}

void testPacketTrafficMetrics() {
    static_assert(!dobby::worldRenderIsFresh(0, 1000));
    static_assert(dobby::worldRenderIsFresh(1000, 2000));
    static_assert(!dobby::worldRenderIsFresh(1000, 2001));
    static_assert(!dobby::worldRenderIsFresh(2000, 1000));
    dobby::PacketTrafficTracker traffic;
    traffic.recordPacket(dobby::PacketDirection::incoming, 100, 1000);
    traffic.recordPacket(dobby::PacketDirection::incoming, 120, 1050);
    traffic.recordPacket(dobby::PacketDirection::outgoing, 2048, 1090);

    const auto snapshot = traffic.snapshot(1100);
    require(snapshot.incomingPackets == 2);
    require(snapshot.outgoingPackets == 1);
    require(snapshot.incomingBytes == 220);
    require(snapshot.outgoingBytes == 2048);
    require(snapshot.incomingPacketsPerSecond == 2);
    require(snapshot.outgoingPacketsPerSecond == 1);
    require(snapshot.incomingBytesPerSecond == 220);
    require(snapshot.outgoingBytesPerSecond == 2048);

    const auto text = dobby::formatPacketTraffic(snapshot);
    require(text.incomingSummary == "IN 2/S 220B/S TOTAL 220B");
    require(text.outgoingSummary == "OUT 1/S 2.0KB/S TOTAL 2.0KB");
    dobby::PacketTrafficSnapshot scaledTotals;
    scaledTotals.incomingBytes = 1536ULL * 1024ULL * 1024ULL;
    scaledTotals.outgoingBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    const auto scaledText = dobby::formatPacketTraffic(scaledTotals);
    require(scaledText.incomingSummary == "IN 0/S 0B/S TOTAL 1.5GB");
    require(scaledText.outgoingSummary == "OUT 0/S 0B/S TOTAL 2.0GB");

    const auto geometry = dobby::buildPacketTrafficGeometry(
            snapshot, 1280.0F, 720.0F);
    require(!geometry.shadowVertices.empty());
    require(!geometry.incomingVertices.empty());
    require(!geometry.outgoingVertices.empty());
    float maximumHorizontal = -1.0F;
    float minimumVertical = 1.0F;
    for (std::size_t index = 0; index < geometry.shadowVertices.size();
         index += 2) {
        maximumHorizontal = std::max(
                maximumHorizontal, geometry.shadowVertices[index]);
        minimumVertical = std::min(
                minimumVertical, geometry.shadowVertices[index + 1]);
    }
    require(maximumHorizontal > 0.9F);
    require(minimumVertical < -0.7F);
    require(dobby::buildPacketTrafficGeometry(snapshot, 0.0F, 720.0F)
                    .shadowVertices.empty());

    require(traffic.snapshot(2100).incomingPacketsPerSecond == 0);
    traffic.reset();
    require(traffic.snapshot(5000).incomingPackets == 0);

    static_assert(dobby::target::kPacketObserverVtableOffset == 0x120a5148);
    static_assert(dobby::target::kPacketSentToOffset == 0x0c2a0548);
    static_assert(dobby::target::kPacketSentToVtableSlotOffset == 0x120a5158);
    static_assert(dobby::target::kPacketReceivedFromOffset == 0x0c2a058c);
    static_assert(dobby::target::kPacketReceivedFromVtableSlotOffset ==
                  0x120a5160);
    static_assert(dobby::target::kPacketGetIdVtableSlot == 2);
}

void testDeveloperPreferences() {
    const dobby::DeveloperPreferences defaults{
            .autoPopup = true,
            .entityHitboxes = true,
            .chestEsp = false,
            .oreEsp = true,
            .networkMetricsOverlay = true,
            .packetTrafficOverlay = true,
    };

    const auto parsed = dobby::parseDeveloperPreferences(
            "version=1\n"
            "automatic_popup=off\n"
            "entity_hitboxes=false\n"
            "chest_esp=true\n"
            "ore_esp=false\n"
            "network_metrics=0\n"
            "packet_traffic=false\n",
            defaults);
    require(!parsed.autoPopup);
    require(!parsed.entityHitboxes);
    require(parsed.chestEsp);
    require(!parsed.oreEsp);
    require(!parsed.networkMetricsOverlay);
    require(!parsed.packetTrafficOverlay);

    const auto malformed = dobby::parseDeveloperPreferences(
            "version=1\n"
            "automatic_popup=invalid\n"
            "entity_hitboxes=maybe\n"
            "chest_esp=unknown\n"
            "ore_esp=unknown\n"
            "network_metrics=unknown\n"
            "packet_traffic=unknown\n"
            "future_setting=false\n",
            defaults);
    require(malformed.autoPopup);
    require(malformed.entityHitboxes);
    require(!malformed.chestEsp);
    require(malformed.oreEsp);
    require(malformed.networkMetricsOverlay);
    require(malformed.packetTrafficOverlay);

    const auto unsupported = dobby::parseDeveloperPreferences(
            "version=2\nentity_hitboxes=false\n", defaults);
    require(unsupported == defaults);

    const auto serialized = dobby::serializeDeveloperPreferences(parsed);
    const auto roundTrip = dobby::parseDeveloperPreferences(serialized, defaults);
    require(roundTrip == parsed);

    const auto path = std::filesystem::temp_directory_path() /
            "dobby-preferences-test.conf";
    std::error_code error;
    std::filesystem::remove(path, error);
    require(dobby::saveDeveloperPreferencesFile(path.string(), parsed));
    require(dobby::loadDeveloperPreferencesFile(path.string(), defaults) == parsed);
    std::filesystem::remove(path, error);
}

void testOreEspRegistry() {
    require(dobby::target::kBlockTypeHashedNameOffset +
                    dobby::target::kHashedStringValueOffset ==
            0xd0);
    require(dobby::target::kRenderContextCameraStateOffset == 0xa8);
    require(dobby::target::kRenderCameraStatePositionOffset == 0x34);
    const std::array<std::uint8_t, 9> expectedPaletteWidths{
            0, 1, 2, 3, 4, 5, 6, 8, 16};
    for (std::size_t index = 0; index < expectedPaletteWidths.size(); ++index) {
        require(dobby::target::kSubChunkStorageDispatches[index].bitsPerElement ==
                expectedPaletteWidths[index]);
    }

    const std::array<std::byte, 4> readable{
            std::byte{0x10}, std::byte{0x20},
            std::byte{0x30}, std::byte{0x40}};
    std::array<std::byte, 4> copied{};
    require(dobby::copyReadableMemory(readable.data(), copied));
    require(copied == readable);

    require(dobby::validBlockResourceName("minecraft:diamond_ore"));
    require(dobby::validBlockResourceName("economy:custom/block"));
    require(!dobby::validBlockResourceName("diamond_ore"));
    require(!dobby::validBlockResourceName("Economy:custom_block"));
    const auto unreadableName = dobby::classifyOreBlockNameForCache(
            std::nullopt);
    require(!unreadableName.cacheable);
    require(!unreadableName.ore);
    const auto invalidName = dobby::classifyOreBlockNameForCache(
            std::string_view{"diamond_ore"});
    require(!invalidName.cacheable);
    require(!invalidName.ore);
    const auto cacheableNonOre = dobby::classifyOreBlockNameForCache(
            std::string_view{"minecraft:stone"});
    require(cacheableNonOre.cacheable);
    require(!cacheableNonOre.ore);
    require(dobby::classifyOreBlockName("minecraft:diamond_ore") ==
            dobby::OreKind::diamond);
    require(dobby::classifyOreBlockName("minecraft:diamond_block") ==
            dobby::OreKind::diamond);
    require(dobby::classifyOreBlockName("minecraft:raw_iron_block") ==
            dobby::OreKind::iron);
    require(dobby::classifyOreBlockName("minecraft:netherite_block") ==
            dobby::OreKind::ancientDebris);
    require(dobby::classifyOreBlockName(
                    "minecraft:deepslate_redstone_ore") ==
            dobby::OreKind::redstone);
    require(dobby::classifyOreBlockName("minecraft:quartz_ore") ==
            dobby::OreKind::quartz);
    require(dobby::classifyOreBlockName("minecraft:ancient_debris") ==
            dobby::OreKind::ancientDebris);
    require(!dobby::classifyOreBlockName("minecraft:stone"));

    const auto* scanLevel = reinterpret_cast<const void*>(0x3000);
    const dobby::OreChunkScanTarget distant{
            reinterpret_cast<const void*>(0x4000), scanLevel, {10, 10}};
    const dobby::OreChunkScanTarget nearbyTarget{
            reinterpret_cast<const void*>(0x5000), scanLevel, {0, 0}};
    dobby::OreRescanSchedule schedule(2, 100);
    require(schedule.track(distant) ==
            dobby::OreChunkTrackResult::accepted);
    require(schedule.track(nearbyTarget) ==
            dobby::OreChunkTrackResult::accepted);
    require(schedule.track(nearbyTarget) ==
            dobby::OreChunkTrackResult::accepted);
    require(schedule.size() == 2);
    require(schedule.pendingForLevel(scanLevel) == 2);
    require(schedule.sizeForLevel(scanLevel) == 2);
    require(schedule.sizeForLevel(
                    reinterpret_cast<const void*>(0x9990)) == 0);
    require(schedule.selectNext(scanLevel, {0.0F, 64.0F, 0.0F}, 0) ==
            nearbyTarget);
    schedule.markScanned(nearbyTarget, 0);
    require(schedule.pendingForLevel(scanLevel) == 1);
    require(schedule.track(nearbyTarget) ==
            dobby::OreChunkTrackResult::accepted);
    require(schedule.pendingForLevel(scanLevel) == 1);
    require(schedule.selectNext(scanLevel, {0.0F, 64.0F, 0.0F}, 0) ==
            distant);
    require(schedule.markDirty(nearbyTarget));
    require(schedule.pendingForLevel(scanLevel) == 2);
    require(schedule.selectNext(scanLevel, {0.0F, 64.0F, 0.0F}, 0) ==
            nearbyTarget);
    schedule.markScanned(nearbyTarget, 0);
    schedule.markScanned(distant, 1);
    require(schedule.pendingForLevel(scanLevel) == 0);
    require(!schedule.selectNext(
            scanLevel, {0.0F, 64.0F, 0.0F}, 100));
    require(schedule.selectNext(
                    scanLevel, {0.0F, 64.0F, 0.0F}, 101) == nearbyTarget);
    schedule.markScanned(nearbyTarget, 101);
    schedule.requestFullRescan();
    require(schedule.pendingForLevel(scanLevel) == 2);
    require(schedule.remove(distant));
    require(schedule.size() == 1);

    dobby::OreRescanSchedule replacementSchedule(2, 100);
    const dobby::OreChunkScanTarget oldChunk{
            reinterpret_cast<const void*>(0x6100), scanLevel, {3, 4}};
    const dobby::OreChunkScanTarget replacementChunk{
            reinterpret_cast<const void*>(0x6200), scanLevel, {3, 4}};
    require(replacementSchedule.track(oldChunk) ==
            dobby::OreChunkTrackResult::accepted);
    replacementSchedule.markScanned(oldChunk, 0);
    require(replacementSchedule.track(replacementChunk) ==
            dobby::OreChunkTrackResult::accepted);
    require(replacementSchedule.trackIfAbsent(oldChunk) ==
            dobby::OreChunkTrackResult::existingOwner);
    require(!replacementSchedule.markDirty(oldChunk));
    require(!replacementSchedule.remove(oldChunk));
    require(replacementSchedule.size() == 1);
    require(replacementSchedule.selectNext(
                    scanLevel, {48.0F, 64.0F, 64.0F}, 0) ==
            replacementChunk);
    const auto removedReplacement = replacementSchedule.removeByChunk(
            replacementChunk.chunk);
    require(removedReplacement == replacementChunk);
    require(!replacementSchedule.removeByChunk(replacementChunk.chunk));
    require(!replacementSchedule.removeByChunk(nullptr));

    dobby::OreRescanSchedule evictionSchedule(2, 100);
    const dobby::OreChunkScanTarget oldest{
            reinterpret_cast<const void*>(0x7000), scanLevel, {-2, 0}};
    const dobby::OreChunkScanTarget middle{
            reinterpret_cast<const void*>(0x7100), scanLevel, {-1, 0}};
    const dobby::OreChunkScanTarget frontier{
            reinterpret_cast<const void*>(0x7200), scanLevel, {0, 0}};
    require(evictionSchedule.track(oldest) ==
            dobby::OreChunkTrackResult::accepted);
    require(evictionSchedule.track(middle) ==
            dobby::OreChunkTrackResult::accepted);
    require(evictionSchedule.track(oldest) ==
            dobby::OreChunkTrackResult::accepted);
    std::optional<dobby::OreChunkScanTarget> evicted;
    require(evictionSchedule.track(frontier, &evicted) ==
            dobby::OreChunkTrackResult::acceptedWithEviction);
    require(evicted == oldest);
    require(evictionSchedule.size() == 2);
    require(!evictionSchedule.remove(oldest));
    require(evictionSchedule.selectNext(
                    scanLevel, {8.0F, 64.0F, 0.0F}, 0) == frontier);
    dobby::OreRescanSchedule zeroCapacity(0, 100);
    require(zeroCapacity.track(frontier) ==
            dobby::OreChunkTrackResult::capacityReached);

    dobby::OreRescanSchedule retrySchedule(2, 100);
    const dobby::OreChunkScanTarget retryNearest{
            reinterpret_cast<const void*>(0x8000), scanLevel, {0, 0}};
    const dobby::OreChunkScanTarget retryOther{
            reinterpret_cast<const void*>(0x8100), scanLevel, {2, 0}};
    require(retrySchedule.track(retryNearest) ==
            dobby::OreChunkTrackResult::accepted);
    require(retrySchedule.track(retryOther) ==
            dobby::OreChunkTrackResult::accepted);
    require(retrySchedule.markRetry(retryNearest, 100, 50));
    require(retrySchedule.markDirty(retryNearest));
    require(retrySchedule.markDirty(retryNearest));
    require(retrySchedule.pendingForLevel(scanLevel) == 2);
    require(retrySchedule.selectNext(
                    scanLevel, {0.0F, 64.0F, 0.0F}, 100) == retryOther);
    retrySchedule.markScanned(retryOther, 100);
    require(!retrySchedule.selectNext(
            scanLevel, {0.0F, 64.0F, 0.0F}, 149));
    require(retrySchedule.selectNext(
                    scanLevel, {0.0F, 64.0F, 0.0F}, 150) ==
            retryNearest);

    require(dobby::packedPaletteIndex({}, 0, 4'095, 1) == 0);
    require(!dobby::packedPaletteIndex({}, 0, 0, 2));
    const std::array<std::uint32_t, 1> twoBitWords{0b11'10'01'00U};
    require(dobby::packedPaletteIndex(twoBitWords, 2, 0, 4) == 0);
    require(dobby::packedPaletteIndex(twoBitWords, 2, 1, 4) == 1);
    require(dobby::packedPaletteIndex(twoBitWords, 2, 2, 4) == 2);
    require(dobby::packedPaletteIndex(twoBitWords, 2, 3, 4) == 3);
    require(!dobby::packedPaletteIndex(twoBitWords, 2, 4'096, 4));
    const std::array<std::uint32_t, 1> invalidPaletteWord{3U};
    require(!dobby::packedPaletteIndex(invalidPaletteWord, 2, 0, 3));
    const std::array<std::uint32_t, 1> threeBitWords{0b101'011U};
    require(dobby::packedPaletteIndex(threeBitWords, 3, 0, 8) == 3);
    require(dobby::packedPaletteIndex(threeBitWords, 3, 1, 8) == 5);
    require(!dobby::packedPaletteIndex(threeBitWords, 7, 0, 8));

    dobby::ChunkOreRegistry registry(2, 3);
    const auto* level = reinterpret_cast<const void*>(0x1000);
    const dobby::ChunkPosition chunk{2, -3};
    const std::vector<dobby::OreBlock> ores{
            {{32, 5, -48}, dobby::OreKind::diamond},
            {{33, 6, -47}, dobby::OreKind::iron},
    };
    require(registry.replaceSubChunk(level, chunk, 0, ores) ==
            dobby::ChunkOreUpdateResult::accepted);
    require(registry.size() == 2);
    require(registry.sizeForLevel(level) == 2);
    require(registry.chunkCount() == 1);
    require(registry.chunkCountForLevel(level) == 1);
    const auto nearby = registry.snapshotNear(
            level, {32.0F, 5.0F, -48.0F}, 8.0F, 8);
    require(nearby.size() == 2);
    const auto limited = registry.snapshotNear(
            level, {32.0F, 5.0F, -48.0F}, 8.0F, 1);
    require(limited.size() == 1);

    const std::vector<dobby::OreBlock> invalid{
            {{0, 5, 0}, dobby::OreKind::gold},
    };
    require(registry.replaceSubChunk(level, chunk, 0, invalid) ==
            dobby::ChunkOreUpdateResult::invalidInput);
    registry.removeChunk(level, chunk);
    require(registry.size() == 0);
    require(registry.chunkCount() == 0);

    dobby::ChunkOreRegistry spatialRegistry(4, 8);
    const dobby::ChunkPosition farChunk{5, 0};
    const dobby::ChunkPosition nearChunk{0, 0};
    require(spatialRegistry.replaceSubChunk(
                    level, farChunk, 0,
                    {{{80, 5, 0}, dobby::OreKind::gold}}) ==
            dobby::ChunkOreUpdateResult::accepted);
    require(spatialRegistry.replaceSubChunk(
                    level, nearChunk, 0,
                    {{{0, 5, 0}, dobby::OreKind::diamond}}) ==
            dobby::ChunkOreUpdateResult::accepted);
    const auto nearestLimited = spatialRegistry.snapshotNear(
            level, {0.0F, 5.0F, 0.0F}, 128.0F, 1);
    require(nearestLimited.size() == 1);
    require(nearestLimited.front().position ==
            dobby::BlockPosition{0, 5, 0});
    require(nearestLimited.front().kind == dobby::OreKind::diamond);

    dobby::ChunkOreRegistry viewportRegistry(4, 8);
    const dobby::ChunkPosition behindChunk{0, 0};
    const dobby::ChunkPosition visibleChunk{0, -2};
    require(viewportRegistry.replaceSubChunk(
                    level, behindChunk, 0,
                    {{{0, 5, 0}, dobby::OreKind::iron}}) ==
            dobby::ChunkOreUpdateResult::accepted);
    require(viewportRegistry.replaceSubChunk(
                    level, visibleChunk, 0,
                    {{{0, 5, -32}, dobby::OreKind::diamond}}) ==
            dobby::ChunkOreUpdateResult::accepted);
    const dobby::CameraFrame selectionCamera{
            {0.0F, 5.0F, 0.0F},
            {{1.0F, 0.0F, 0.0F, 0.0F,
              0.0F, 1.0F, 0.0F, 0.0F,
              0.0F, 0.0F, 1.0F, 0.0F,
              0.0F, 0.0F, 0.0F, 1.0F}},
            {{0.5625F, 0.0F, 0.0F, 0.0F,
              0.0F, 1.0F, 0.0F, 0.0F,
              0.0F, 0.0F, -1.0F, -1.0F,
              0.0F, 0.0F, -0.2F, 0.0F}},
    };
    const auto visibleLimited = viewportRegistry.snapshotNear(
            level, selectionCamera.position, 64.0F, 1, &selectionCamera);
    require(visibleLimited.size() == 1);
    require(visibleLimited.front().position ==
            dobby::BlockPosition{0, 5, -32});
    require(visibleLimited.front().kind == dobby::OreKind::diamond);

    dobby::ChunkOreRegistry atomicRegistry(2, 3);
    const dobby::ChunkPosition atomicChunk{0, 0};
    const std::vector<dobby::OreSubChunkSnapshot> initialChunk{{
            0,
            {{{0, 1, 0}, dobby::OreKind::diamond}},
    }, {
            1,
            {{{1, 16, 1}, dobby::OreKind::gold}},
    }};
    require(atomicRegistry.replaceChunk(level, atomicChunk, initialChunk) ==
            dobby::ChunkOreUpdateResult::accepted);
    require(atomicRegistry.size() == 2);
    require(atomicRegistry.chunkCount() == 1);

    const std::vector<dobby::OreSubChunkSnapshot> invalidWholeChunk{{
            0,
            {{{2, 2, 2}, dobby::OreKind::emerald}},
    }, {
            1,
            {{{16, 16, 0}, dobby::OreKind::iron}},
    }};
    require(atomicRegistry.replaceChunk(
                    level, atomicChunk, invalidWholeChunk) ==
            dobby::ChunkOreUpdateResult::invalidInput);
    const auto preservedAfterInvalid = atomicRegistry.snapshotNear(
            level, {0.0F, 8.0F, 0.0F}, 64.0F, 8);
    require(preservedAfterInvalid.size() == 2);
    require(std::find(
                    preservedAfterInvalid.begin(), preservedAfterInvalid.end(),
                    dobby::OreBlock{{0, 1, 0}, dobby::OreKind::diamond}) !=
            preservedAfterInvalid.end());
    require(std::find(
                    preservedAfterInvalid.begin(), preservedAfterInvalid.end(),
                    dobby::OreBlock{{1, 16, 1}, dobby::OreKind::gold}) !=
            preservedAfterInvalid.end());

    const std::vector<dobby::OreSubChunkSnapshot> oversizedReplacement{{
            0,
            {
                    {{3, 3, 3}, dobby::OreKind::coal},
                    {{4, 3, 3}, dobby::OreKind::iron},
                    {{5, 3, 3}, dobby::OreKind::copper},
                    {{6, 3, 3}, dobby::OreKind::emerald},
            },
    }};
    require(atomicRegistry.replaceChunk(
                    level, atomicChunk, oversizedReplacement) ==
            dobby::ChunkOreUpdateResult::positionCapacityReached);
    require(atomicRegistry.size() == 2);

    const std::vector<dobby::OreSubChunkSnapshot> finalChunk{{
            0,
            {{{2, 2, 2}, dobby::OreKind::emerald}},
    }};
    require(atomicRegistry.replaceChunk(level, atomicChunk, finalChunk) ==
            dobby::ChunkOreUpdateResult::accepted);
    const auto finalSnapshot = atomicRegistry.snapshotNear(
            level, {0.0F, 8.0F, 0.0F}, 64.0F, 8);
    require(finalSnapshot == std::vector<dobby::OreBlock>{{
            {2, 2, 2}, dobby::OreKind::emerald}});
    require(atomicRegistry.size() == 1);
    require(atomicRegistry.chunkCountForLevel(level) == 1);
}

void testDobbyWindowPolicy() {
    constexpr std::uint32_t existing = 1U << 3U;
    assert(dobby::dobbyWindowFlags("Other", existing) == existing);
    const auto flags = dobby::dobbyWindowFlags("Dobby##dobby_violation_v3", existing);
    static_cast<void>(flags);
    assert((flags & (1U << 1U)) != 0);
    assert((flags & (1U << 5U)) != 0);
    assert((flags & (1U << 6U)) != 0);
    assert((flags & (1U << 8U)) != 0);
    assert((flags & existing) != 0);
}

} // namespace

int main() {
    testViolationDecoder();
    testStreamProbeAndReport();
    testClientSchemaFieldTrace();
    testProtocolDumpCompilation();
    testRepeatViolationsAreRetained();
    testEntityHitboxState();
    testEntityProjection();
    testChestEspRegistry();
    testNetworkMetrics();
    testClientPerformanceMetrics();
    testConfigurationAndPacketCatalog();
    testPacketTrafficMetrics();
    testDeveloperPreferences();
    testOreEspRegistry();
    testDobbyWindowPolicy();
    std::cout << "Dobby tests passed\n";
}
