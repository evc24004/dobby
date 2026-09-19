#include "hooks/packet_traffic_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "hooks/minecraft_image.hpp"
#include "metrics/packet_traffic.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace dobby {
namespace {

using ObservePacketFn = void (*)(void* observer, const void* identifier,
                                 const void* packet, std::uint32_t bytes);
using GetPacketIdFn = std::int32_t (*)(const void* packet);

std::atomic_bool trafficHooksReady{false};
std::atomic_bool firstIncomingLogged{false};
std::atomic_bool firstOutgoingLogged{false};
ObservePacketFn originalPacketSentTo = nullptr;
ObservePacketFn originalPacketReceivedFrom = nullptr;
MinecraftImage minecraftImage{};

bool patchSlot(std::uintptr_t slotAddress, std::uintptr_t replacement) {
    auto value = replacement;
    auto* slot = reinterpret_cast<void*>(slotAddress);
    if (mcpelauncher_patch == nullptr ||
        mcpelauncher_patch(slot, &value, sizeof(value)) == nullptr) {
        return false;
    }
    std::uintptr_t installed{};
    std::memcpy(&installed, slot, sizeof(installed));
    return installed == replacement;
}

bool validateTarget(std::uintptr_t functionOffset, std::uintptr_t slotOffset,
                    const auto& signature) {
    const auto function = minecraftImage.base + functionOffset;
    const auto slot = minecraftImage.base + slotOffset;
    if (!addressIsExecutable(minecraftImage, function) ||
        !addressIsInImage(minecraftImage, slot) ||
        !matchesSignature(reinterpret_cast<const void*>(function), signature)) {
        return false;
    }
    std::uintptr_t current{};
    std::memcpy(&current, reinterpret_cast<const void*>(slot), sizeof(current));
    return current == function;
}

bool packetId(const void* packet, std::int32_t& result) {
    if (packet == nullptr)
        return false;
    std::uintptr_t vtable{};
    std::memcpy(&vtable, packet, sizeof(vtable));
    const auto slot = vtable +
            target::kPacketGetIdVtableSlot * sizeof(std::uintptr_t);
    if (!addressIsInImage(minecraftImage, vtable) ||
        !addressIsInImage(minecraftImage, slot)) {
        return false;
    }
    std::uintptr_t function{};
    std::memcpy(&function, reinterpret_cast<const void*>(slot), sizeof(function));
    if (!addressIsExecutable(minecraftImage, function))
        return false;
    result = reinterpret_cast<GetPacketIdFn>(function)(packet);
    return true;
}

void recordObservedPacket(PacketDirection direction, const void* packet,
                          std::uint32_t bytes) {
    std::int32_t id{};
    if (packetId(packet, id)) {
        recordPacketTraffic(direction, bytes);
        auto& firstLogged = direction == PacketDirection::incoming
                ? firstIncomingLogged : firstOutgoingLogged;
        if (!firstLogged.exchange(true, std::memory_order_acq_rel)) {
            logLine(std::string("packet traffic: first ") +
                    (direction == PacketDirection::incoming
                             ? "incoming" : "outgoing") +
                    " packet id=" + std::to_string(id) +
                    " bytes=" + std::to_string(bytes));
        }
    }
}

void packetSentToDetour(void* observer, const void* identifier,
                        const void* packet, std::uint32_t bytes) {
    recordObservedPacket(PacketDirection::outgoing, packet, bytes);
    if (originalPacketSentTo != nullptr)
        originalPacketSentTo(observer, identifier, packet, bytes);
}

void packetReceivedFromDetour(void* observer, const void* identifier,
                              const void* packet, std::uint32_t bytes) {
    recordObservedPacket(PacketDirection::incoming, packet, bytes);
    if (originalPacketReceivedFrom != nullptr)
        originalPacketReceivedFrom(observer, identifier, packet, bytes);
}

} // namespace

void installPacketTrafficHooks() {
    if (trafficHooksReady.load(std::memory_order_acquire))
        return;
    minecraftImage = findMinecraftImage();
    if (minecraftImage.base == 0 || mcpelauncher_patch == nullptr) {
        logLine("ERROR: packet traffic unavailable; patch API or image missing");
        return;
    }
    // The two concrete observer slots independently identify the live object
    // and both are relocation-verified against their functions. A separate
    // RTTI-derived vtable address point adds no safety to those exact checks.
    if (!validateTarget(target::kPacketSentToOffset,
                        target::kPacketSentToVtableSlotOffset,
                        target::kPacketSentToSignature) ||
        !validateTarget(target::kPacketReceivedFromOffset,
                        target::kPacketReceivedFromVtableSlotOffset,
                        target::kPacketReceivedFromSignature)) {
        logLine("ERROR: packet traffic unavailable; observer target mismatch");
        return;
    }

    originalPacketSentTo = reinterpret_cast<ObservePacketFn>(
            minecraftImage.base + target::kPacketSentToOffset);
    originalPacketReceivedFrom = reinterpret_cast<ObservePacketFn>(
            minecraftImage.base + target::kPacketReceivedFromOffset);
    const bool sentInstalled = patchSlot(
            minecraftImage.base + target::kPacketSentToVtableSlotOffset,
            reinterpret_cast<std::uintptr_t>(packetSentToDetour));
    const bool receivedInstalled = sentInstalled && patchSlot(
            minecraftImage.base + target::kPacketReceivedFromVtableSlotOffset,
            reinterpret_cast<std::uintptr_t>(packetReceivedFromDetour));
    if (!sentInstalled || !receivedInstalled) {
        if (sentInstalled) {
            static_cast<void>(patchSlot(
                    minecraftImage.base + target::kPacketSentToVtableSlotOffset,
                    minecraftImage.base + target::kPacketSentToOffset));
        }
        originalPacketSentTo = nullptr;
        originalPacketReceivedFrom = nullptr;
        logLine("ERROR: packet traffic unavailable; observer patch failed");
        return;
    }
    trafficHooksReady.store(true, std::memory_order_release);
    logLine("packet traffic: incoming and outgoing observers hooked");
}

bool packetTrafficHooksInstalled() {
    return trafficHooksReady.load(std::memory_order_acquire);
}

} // namespace dobby

#else

namespace dobby {

void installPacketTrafficHooks() {}
bool packetTrafficHooksInstalled() { return false; }

} // namespace dobby

#endif
