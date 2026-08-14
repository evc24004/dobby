#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dobby {

inline constexpr char kDobbyVersion[] = "2.10.0";
inline constexpr char kMinecraftVersion[] = "1.26.40.5";
inline constexpr char kMinecraftBuildId[] = "5893edc8d56c93cbdb50e0f9436320236b78c89d";
inline constexpr char kMinecraftDataVersion[] = "1.26.40";
inline constexpr std::int32_t kMinecraftProtocolVersion = 2168;
inline constexpr char kAbi[] = "arm64-v8a";

inline constexpr std::size_t kDefaultHistoryLimit = 100;
inline constexpr std::size_t kMaximumHistoryLimit = 1000;
inline constexpr std::size_t kDefaultRawCaptureLimit = 2048;
inline constexpr std::size_t kMaximumRawCaptureLimit = 65536;

namespace target {

// All offsets and signatures are for the exact kMinecraftBuildId image.
inline constexpr std::uintptr_t kViolationGetIdOffset = 0x0cfa1cb4;
inline constexpr std::uintptr_t kViolationGetIdVtableSlotOffset = 0x120f7160;
inline constexpr std::array<std::uint8_t, 8> kViolationGetIdSignature{
        0x80, 0x13, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6};

inline constexpr std::uintptr_t kStreamReadOffset = 0x11a7b044;
inline constexpr std::uintptr_t kStreamReadVtableSlotOffset = 0x1249dac8;
inline constexpr std::array<std::uint8_t, 16> kStreamReadSignature{
        0xff, 0x83, 0x04, 0xd1, 0xfd, 0x7b, 0x0e, 0xa9,
        0xfc, 0x7b, 0x00, 0xf9, 0xf6, 0x57, 0x10, 0xa9};

inline constexpr std::uintptr_t kPacketEndCheckOffset = 0x11a7adb4;
inline constexpr std::array<std::uint8_t, 16> kPacketEndCheckSignature{
        0xff, 0x43, 0x02, 0xd1, 0xfd, 0x7b, 0x06, 0xa9,
        0xf5, 0x3b, 0x00, 0xf9, 0xf4, 0x4f, 0x08, 0xa9};

// PacketSchemaReader virtuals verified against the matching LeviLamina headers.
inline constexpr std::uintptr_t kSchemaPushMemberOffset = 0x0cbca71c;
inline constexpr std::uintptr_t kSchemaPushMemberVtableSlotOffset = 0x120dc6e0;
inline constexpr std::array<std::uint8_t, 8> kSchemaPushMemberSignature{
        0x20, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6};

inline constexpr std::uintptr_t kSchemaPushElementOffset = 0x0cbca730;
inline constexpr std::uintptr_t kSchemaPushElementVtableSlotOffset = 0x120dc6f0;
inline constexpr std::array<std::uint8_t, 4> kSchemaPushElementSignature{
        0xc0, 0x03, 0x5f, 0xd6};

inline constexpr std::uintptr_t kSchemaPopOffset = 0x0cbca734;
inline constexpr std::uintptr_t kSchemaPopVtableSlotOffset = 0x120dc6f8;
inline constexpr std::array<std::uint8_t, 4> kSchemaPopSignature{
        0xc0, 0x03, 0x5f, 0xd6};

// ActorRenderDispatcher::render(BaseActorRenderContext&, Actor&, bool).
inline constexpr std::uintptr_t kActorRenderOffset = 0x0a317de0;
inline constexpr std::array<std::uint8_t, 16> kActorRenderSignature{
        0xff, 0x43, 0x02, 0xd1, 0xeb, 0x2b, 0x03, 0x6d,
        0xe9, 0x23, 0x04, 0x6d, 0xfd, 0x7b, 0x05, 0xa9};

// RenderChunkCoordinator lifecycle hooks expose the client-decoded LevelChunk
// after Bedrock has loaded it. No packet is requested, changed, or suppressed.
inline constexpr std::uintptr_t kChunkCoordinatorOnChunkLoadedOffset = 0x0a169d30;
inline constexpr std::uintptr_t kChunkCoordinatorOnChunkLoadedSlotOffset = 0x11f4bb90;
inline constexpr std::array<std::uint8_t, 16> kChunkCoordinatorOnChunkLoadedSignature{
        0xff, 0x43, 0x02, 0xd1, 0xfd, 0x7b, 0x03, 0xa9,
        0xfc, 0x6f, 0x04, 0xa9, 0xfa, 0x67, 0x05, 0xa9};

inline constexpr std::uintptr_t kChunkCoordinatorOnSubChunkLoadedOffset = 0x0a16a298;
inline constexpr std::uintptr_t kChunkCoordinatorOnSubChunkLoadedSlotOffset = 0x11f4bba0;
inline constexpr std::array<std::uint8_t, 16> kChunkCoordinatorOnSubChunkLoadedSignature{
        0xff, 0x83, 0x01, 0xd1, 0xfd, 0x7b, 0x02, 0xa9,
        0xf7, 0x1b, 0x00, 0xf9, 0xf6, 0x57, 0x04, 0xa9};

inline constexpr std::uintptr_t kChunkCoordinatorOnChunkUnloadedOffset = 0x088d72d4;
inline constexpr std::uintptr_t kChunkCoordinatorOnChunkUnloadedSlotOffset = 0x11f4bba8;
inline constexpr std::array<std::uint8_t, 4> kChunkCoordinatorOnChunkUnloadedSignature{
        0xc0, 0x03, 0x5f, 0xd6};

// ChestBlockActor lifecycle targets. RTTI relocation records in the matching
// Android image identify the primary vtable address point. The constructor and
// base BlockActor initializer prove that the BlockPos argument is x4 and that
// the resulting BlockActor stores its position at object + 0x08.
inline constexpr std::uintptr_t kChestBlockActorConstructorOffset = 0x0fd7cd58;
inline constexpr std::array<std::uint8_t, 16> kChestBlockActorConstructorSignature{
        0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9,
        0xfd, 0x03, 0x00, 0x91, 0xf4, 0x03, 0x03, 0xaa};
inline constexpr std::uintptr_t kChestBlockActorFactoryOffset = 0x0fd7cf7c;
inline constexpr std::array<std::uint8_t, 16> kChestBlockActorFactorySignature{
        0xfd, 0x7b, 0xbd, 0xa9, 0xf6, 0x57, 0x01, 0xa9,
        0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x03, 0x00, 0x91};
inline constexpr std::uintptr_t kChestBlockActorVtableOffset = 0x12371580;
inline constexpr std::uintptr_t kChestBlockActorDestructorOffset = 0x0fd7ce48;
inline constexpr std::array<std::uint8_t, 16> kChestBlockActorDestructorSignature{
        0xfd, 0x7b, 0xbd, 0xa9, 0xf6, 0x57, 0x01, 0xa9,
        0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x03, 0x00, 0x91};
inline constexpr std::uintptr_t kChestBlockActorDeletingDestructorOffset = 0x0fd7cf30;
inline constexpr std::array<std::uint8_t, 16>
        kChestBlockActorDeletingDestructorSignature{
                0xfd, 0x7b, 0xbe, 0xa9, 0xf3, 0x0b, 0x00, 0xf9,
                0xfd, 0x03, 0x00, 0x91, 0xf3, 0x03, 0x00, 0xaa};
inline constexpr std::size_t kChestBlockActorDestructorVtableSlot = 0;
inline constexpr std::size_t kChestBlockActorDeletingDestructorVtableSlot = 1;
inline constexpr std::uintptr_t kBlockActorPositionLayoutProbeOffset = 0x0fdaae64;
inline constexpr std::array<std::uint8_t, 24>
        kBlockActorPositionLayoutProbeSignature{
                0x48, 0x08, 0x40, 0xb9, 0x49, 0x00, 0x40, 0xf9,
                0x01, 0x50, 0x00, 0x39, 0x1f, 0xfc, 0x01, 0xa9,
                0x08, 0x10, 0x00, 0xb9, 0x09, 0x04, 0x00, 0xf9};
inline constexpr std::ptrdiff_t kBlockActorPositionOffset = 0x08;

// Android arm64 layouts for the exact target build. The generated LeviLamina
// headers use host-sized standard-library types, so these values are validated
// independently against instructions in the Android binary before use.
inline constexpr std::uintptr_t kLevelChunkGetPositionOffset = 0x0f9d05d0;
inline constexpr std::array<std::uint8_t, 8> kLevelChunkGetPositionSignature{
        0x00, 0x40, 0x01, 0x91, 0xc0, 0x03, 0x5f, 0xd6};
inline constexpr std::uintptr_t kLevelChunkGetLevelOffset = 0x0f9d4204;
inline constexpr std::array<std::uint8_t, 8> kLevelChunkGetLevelSignature{
        0x00, 0x14, 0x40, 0xf9, 0xc0, 0x03, 0x5f, 0xd6};
inline constexpr std::uintptr_t kLevelChunkSubChunkLayoutProbeOffset = 0x0f9e12a4;
inline constexpr std::array<std::uint8_t, 32> kLevelChunkSubChunkLayoutProbeSignature{
        0x08, 0xa4, 0x50, 0xa9, 0x29, 0x01, 0x08, 0xcb,
        0x29, 0xfd, 0x43, 0x93, 0x2a, 0x7d, 0x0a, 0x9b,
        0x29, 0x1c, 0x40, 0x92, 0x5f, 0x01, 0x09, 0xeb,
        0x49, 0x04, 0x00, 0x54, 0x0a, 0x0d, 0x80, 0x52};
inline constexpr std::uintptr_t kSubChunkAbsoluteIndexAccessorOffset = 0x0f998ea4;
inline constexpr std::array<std::uint8_t, 8> kSubChunkAbsoluteIndexAccessorSignature{
        0x00, 0x84, 0x41, 0x39, 0xc0, 0x03, 0x5f, 0xd6};
inline constexpr std::uintptr_t kSubChunkStorageLayoutProbeOffset = 0x0f998ed4;
inline constexpr std::array<std::uint8_t, 48> kSubChunkStorageLayoutProbeSignature{
        0x28, 0x1c, 0x00, 0x12, 0x1f, 0x09, 0x00, 0x71,
        0xc1, 0x00, 0x00, 0x54, 0x08, 0x24, 0x43, 0xa9,
        0x08, 0x01, 0x09, 0xaa, 0x1f, 0x01, 0x00, 0xf1,
        0xe0, 0x17, 0x9f, 0x1a, 0xc0, 0x03, 0x5f, 0xd6,
        0x08, 0xc0, 0x00, 0x91, 0x29, 0x1c, 0x40, 0x92,
        0x08, 0x79, 0x69, 0xf8, 0x1f, 0x01, 0x00, 0xf1};
inline constexpr std::ptrdiff_t kLevelChunkLevelOffset = 0x28;
inline constexpr std::ptrdiff_t kLevelChunkPositionOffset = 0x50;
inline constexpr std::ptrdiff_t kLevelChunkSubChunksOffset = 0x108;
inline constexpr std::size_t kSubChunkSize = 0x68;
inline constexpr std::ptrdiff_t kSubChunkStandardStorageOffset = 0x30;
inline constexpr std::ptrdiff_t kSubChunkAbsoluteIndexOffset = 0x61;
inline constexpr std::size_t kSubChunkStorageGetElementVtableSlot = 4;
inline constexpr std::size_t kSubChunkStoragePackedElementVtableSlot = 20;
inline constexpr std::size_t kSubChunkStorageBitsPerElementVtableSlot = 21;
inline constexpr std::size_t kSubChunkStoragePaletteSnapshotVtableSlot = 22;

// Block::mBlockType and the HashedString name accessors for the exact Android
// target. BlockType + 0xc8 is a HashedString; its Android libc++ string starts
// eight bytes later. Both boundaries are independently proven by accessors.
inline constexpr std::ptrdiff_t kBlockBlockTypeOffset = 0x68;
inline constexpr std::uintptr_t kBlockTypeGetHashedNameOffset = 0x0f33323c;
inline constexpr std::array<std::uint8_t, 8> kBlockTypeGetHashedNameSignature{
        0x00, 0x20, 0x03, 0x91, 0xc0, 0x03, 0x5f, 0xd6};
inline constexpr std::ptrdiff_t kBlockTypeHashedNameOffset = 0xc8;
inline constexpr std::uintptr_t kHashedStringGetValueOffset = 0x11aa3e60;
inline constexpr std::array<std::uint8_t, 8> kHashedStringGetValueSignature{
        0x00, 0x20, 0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6};
inline constexpr std::ptrdiff_t kHashedStringValueOffset = 0x8;

struct SubChunkStorageDispatch {
    std::uintptr_t vtableAddressPointOffset;
    std::uintptr_t getElementOffset;
    std::uintptr_t packedElementOffset;
    std::uintptr_t bitsPerElementOffset;
    std::uintptr_t paletteSnapshotOffset;
    std::uint8_t bitsPerElement;
};

// Exact Android arm64 dispatch tables for Block palette widths supported by
// this target (uniform, 1, 2, 3, 4, 5, 6, 8, and 16 bits). Unknown storage
// implementations are rejected before any virtual method is called.
inline constexpr std::array<SubChunkStorageDispatch, 9>
        kSubChunkStorageDispatches{{
                {0x12348140, 0x0f9ae45c, 0x0f9aea28, 0x0f9aea34,
                 0x0f9aea3c, 0},
                {0x12348248, 0x0f9aee14, 0x0f9b0fdc, 0x0f9b0fec,
                 0x0f9b0ff4, 1},
                {0x12348328, 0x0f9b1468, 0x0f9b3a40, 0x0f9b3a50,
                 0x0f9b3a58, 2},
                {0x12348408, 0x0f9b3ea4, 0x0f9b6fd8, 0x0f9b6fe8,
                 0x0f9b6ff0, 3},
                {0x123484e8, 0x0f9b7424, 0x0f9b9584, 0x0f9b9594,
                 0x0f9b959c, 4},
                {0x123485c8, 0x0f9b99c0, 0x0f9bb8e8, 0x0f9bb8f8,
                 0x0f9bb900, 5},
                {0x123486a8, 0x0f9bbd00, 0x0f9bdee0, 0x0f9bdef0,
                 0x0f9bdef8, 6},
                {0x12348788, 0x0f9be320, 0x0f9c02b0, 0x0f9c02c0,
                 0x0f9c02c8, 8},
                {0x12348868, 0x0f9c06e4, 0x0f9c2630, 0x0f9c2640,
                 0x0f9c2648, 16},
        }};
// Returns Actor::mBuiltInComponents.mAABBShapeComponent. AABB is the first
// member of that component, so its address is also the collision AABB address.
inline constexpr std::uintptr_t kActorGetAabbOffset = 0x0ec86fb4;
inline constexpr std::array<std::uint8_t, 8> kActorGetAabbSignature{
        0x00, 0x08, 0x41, 0xf9, 0xc0, 0x03, 0x5f, 0xd6};

// ClientLevel::getRuntimeActorList(). The method returns the complete active
// Actor* list and is used once per presented frame, matching Horion's entity
// enumeration model instead of relying on whichever actors a render pass saw.
inline constexpr std::uintptr_t kLevelGetRuntimeActorListOffset = 0x0f226d10;
inline constexpr std::array<std::uint8_t, 8> kLevelGetRuntimeActorListSignature{
        0x00, 0x38, 0x42, 0xf9, 0x87, 0x94, 0x01, 0x14};
inline constexpr std::size_t kLevelGetRuntimeActorListVtableSlot = 326;

// ClientLevel::forEachPlayer(std::function<bool(Player&)>). This supplements
// the general Actor* registry with the client-owned active-player registry.
inline constexpr std::uintptr_t kLevelForEachPlayerOffset = 0x0f225e4c;
inline constexpr std::array<std::uint8_t, 8> kLevelForEachPlayerSignature{
        0xff, 0x83, 0x01, 0xd1, 0xfd, 0x7b, 0x04, 0xa9};
inline constexpr std::size_t kLevelForEachPlayerVtableSlot = 223;

// ClientLevel::getPrimaryLocalPlayer(). The returned Player* is excluded from
// the overlay while every other active player remains visible.
inline constexpr std::uintptr_t kLevelGetPrimaryLocalPlayerOffset = 0x0f225818;
inline constexpr std::array<std::uint8_t, 8> kLevelGetPrimaryLocalPlayerSignature{
        0x00, 0x0c, 0x42, 0xf9, 0xce, 0x9a, 0xfa, 0x17};
inline constexpr std::size_t kLevelGetPrimaryLocalPlayerVtableSlot = 77;

// The ClientLevel primary vtable. The exact vptr check prevents calling the
// list getter through an unexpected ILevel implementation.
inline constexpr std::uintptr_t kClientLevelVtableOffset = 0x11ed28b0;

// ILevel::getCurrentServerTick() on ClientLevel. Bedrock names this clock as
// the server tick, but the overlay labels its measured rate as an estimate.
inline constexpr std::uintptr_t kLevelGetCurrentServerTickOffset = 0x09ad9014;
inline constexpr std::array<std::uint8_t, 16> kLevelGetCurrentServerTickSignature{
        0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91,
        0x08, 0x00, 0x40, 0xf9, 0x08, 0x41, 0x41, 0xf9};
inline constexpr std::size_t kLevelGetCurrentServerTickVtableSlot = 81;

// RakNetConnector::RakNetNetworkPeer::update() refreshes these native RTT
// fields. The vtable slot is patched only after both target and signature
// validation succeed.
inline constexpr std::uintptr_t kRakNetPeerUpdateOffset = 0x0c2bda48;
inline constexpr std::uintptr_t kRakNetPeerUpdateVtableSlotOffset = 0x120a50f0;
inline constexpr std::array<std::uint8_t, 16> kRakNetPeerUpdateSignature{
        0xfd, 0x7b, 0xbc, 0xa9, 0xfc, 0x5f, 0x01, 0xa9,
        0xf6, 0x57, 0x02, 0xa9, 0xf4, 0x4f, 0x03, 0xa9};
inline constexpr std::ptrdiff_t kRakNetPeerLastPingOffset = 0x104;
inline constexpr std::ptrdiff_t kRakNetPeerAveragePingOffset = 0x108;

// NetworkSystem owns a concrete PacketObserver for every serialized packet.
// These two active slots expose the Packet and Bedrock-observed byte count
// without changing, delaying, or replacing network data.
inline constexpr std::uintptr_t kPacketObserverVtableOffset = 0x120a5148;
inline constexpr std::uintptr_t kPacketSentToOffset = 0x0c2a0548;
inline constexpr std::uintptr_t kPacketSentToVtableSlotOffset = 0x120a5158;
inline constexpr std::array<std::uint8_t, 16> kPacketSentToSignature{
        0xfd, 0x7b, 0xbe, 0xa9, 0xf3, 0x0b, 0x00, 0xf9,
        0xfd, 0x03, 0x00, 0x91, 0x20, 0x04, 0x00, 0x0f};
inline constexpr std::uintptr_t kPacketReceivedFromOffset = 0x0c2a058c;
inline constexpr std::uintptr_t kPacketReceivedFromVtableSlotOffset = 0x120a5160;
inline constexpr std::array<std::uint8_t, 16> kPacketReceivedFromSignature{
        0xfd, 0x7b, 0xbe, 0xa9, 0xf3, 0x0b, 0x00, 0xf9,
        0xfd, 0x03, 0x00, 0x91, 0x20, 0x04, 0x00, 0x0f};
inline constexpr std::size_t kPacketGetIdVtableSlot = 2;

// Startup-only protocol discovery targets. Dobby asks the game's own packet
// factory for each accepted ID and serializes the resulting default instance
// into an isolated BinaryStream. Nothing produced by this sweep is sent.
inline constexpr std::uintptr_t kPacketFactoryOffset = 0x0c28e594;
inline constexpr std::array<std::uint8_t, 16> kPacketFactorySignature{
        0x1f, 0x7c, 0x05, 0x71, 0x88, 0x3a, 0x00, 0x54,
        0xe9, 0x03, 0x00, 0x2a, 0xaa, 0x5f, 0xfb, 0xd0};
inline constexpr std::int32_t kPacketFactoryHighestId = 351;
inline constexpr std::size_t kPacketGetNameVtableSlot = 3;
inline constexpr std::size_t kPacketWriteVtableSlot = 8;
inline constexpr std::size_t kPacketIsValidVtableSlot = 12;
inline constexpr std::size_t kPacketSerializationModeVtableSlot = 13;

inline constexpr std::uintptr_t kBinaryStreamConstructorOffset = 0x11a7ab9c;
inline constexpr std::array<std::uint8_t, 16> kBinaryStreamConstructorSignature{
        0x08, 0x51, 0x00, 0xd0, 0x08, 0xa1, 0x21, 0x91,
        0xe9, 0x03, 0x00, 0xaa, 0x0a, 0x24, 0x00, 0x91};
inline constexpr std::uintptr_t kBinaryStreamVtableOffset = 0x1249c868;
inline constexpr std::size_t kBinaryStreamObjectSize = 0x48;

struct ProtocolVirtualTarget {
    std::size_t slot;
    std::uintptr_t functionOffset;
    std::array<std::uint8_t, 8> signature;
};

inline constexpr std::array<ProtocolVirtualTarget, 27>
        kBinaryStreamProtocolTargets{{
                {3, 0x11a7bb08, {0x08, 0x00, 0x40, 0xf9, 0x21, 0x00, 0x00, 0x12}},
                {4, 0x11a7bc6c, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {5, 0x11a7c004, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {6, 0x11a7c734, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {7, 0x11a7c39c, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {8, 0x11a7d204, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {9, 0x11a7cad0, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {10, 0x11a7ce6c, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {11, 0x11a7d934, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {12, 0x11a7d59c, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {13, 0x11a7ebdc, {0xfd, 0x7b, 0xbd, 0xa9, 0xf5, 0x0b, 0x00, 0xf9}},
                {14, 0x11a7f33c, {0xfd, 0x7b, 0xbd, 0xa9, 0xf5, 0x0b, 0x00, 0xf9}},
                {15, 0x11a7ebc4, {0x28, 0x78, 0x1f, 0x53, 0x09, 0x00, 0x40, 0xf9}},
                {16, 0x11a7f39c, {0x28, 0xf8, 0x7f, 0xd3, 0x09, 0x00, 0x40, 0xf9}},
                {17, 0x11a7dcd4, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {18, 0x11a7e06c, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {19, 0x11a7e2ac, {0x00, 0xc0, 0x22, 0x1e, 0xe3, 0x03, 0x02, 0xaa}},
                {20, 0x11a7e2cc, {0xe8, 0x67, 0x6a, 0xb2, 0x00, 0xc0, 0x22, 0x1e}},
                {21, 0x11a80d14, {0xfd, 0x7b, 0xbd, 0xa9, 0xf5, 0x0b, 0x00, 0xf9}},
                {22, 0x11a80dd8, {0xe8, 0x03, 0x00, 0xaa, 0x81, 0x00, 0x00, 0x37}},
                {23, 0x11a80dfc, {0x48, 0x04, 0x40, 0xf9, 0xe1, 0x03, 0x00, 0xaa}},
                {24, 0x11a80e54, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {25, 0x11a80ecc, {0xff, 0xc3, 0x00, 0xd1, 0xfd, 0x7b, 0x01, 0xa9}},
                {26, 0x11a93538, {0xe0, 0x03, 0x1f, 0x2a, 0xc0, 0x03, 0x5f, 0xd6}},
                {27, 0x11a93540, {0xc0, 0x03, 0x5f, 0xd6, 0xc0, 0x03, 0x5f, 0xd6}},
                {28, 0x11a93544, {0xc0, 0x03, 0x5f, 0xd6, 0x08, 0x20, 0x40, 0x39}},
                {29, 0x11a80fec, {0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9}},
        }};

inline constexpr std::uintptr_t kPacketSchemaWriterVtableOffset = 0x120dc558;
inline constexpr std::array<ProtocolVirtualTarget, 22>
        kPacketSchemaWriterTargets{{
                {2, 0x0cbc949c, {0x20, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6}},
                {3, 0x0cbc94a4, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {4, 0x0cbc94d4, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {5, 0x0cbc9504, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {6, 0x0cbc9534, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {7, 0x0cbc9564, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {8, 0x0cbc9594, {0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9}},
                {9, 0x0cbc9610, {0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9}},
                {10, 0x0cbc9678, {0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9}},
                {11, 0x0cbc96e0, {0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9}},
                {12, 0x0cbc9748, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {13, 0x0cbc9778, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {14, 0x0cbc97a8, {0xfd, 0x7b, 0xbf, 0xa9, 0xfd, 0x03, 0x00, 0x91}},
                {15, 0x0cbc97d8, {0xfd, 0x7b, 0xbd, 0xa9, 0xf5, 0x0b, 0x00, 0xf9}},
                {16, 0x0cbc9840, {0x00, 0x08, 0x40, 0xf9, 0x82, 0xb7, 0xfa, 0xf0}},
                {17, 0x0cbc985c, {0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9}},
                {18, 0x0cbc98bc, {0x00, 0x08, 0x40, 0xf9, 0xe3, 0xba, 0xfa, 0xd0}},
                {19, 0x0cbc98d8, {0x20, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6}},
                {20, 0x0cbc98e0, {0xc0, 0x03, 0x5f, 0xd6, 0x60, 0x00, 0x80, 0x52}},
                {21, 0x0cbc98e4, {0x60, 0x00, 0x80, 0x52, 0xc0, 0x03, 0x5f, 0xd6}},
                {22, 0x0cbc98ec, {0xc1, 0x02, 0x00, 0x36, 0xfd, 0x7b, 0xbe, 0xa9}},
                {23, 0x0cbc994c, {0xc0, 0x03, 0x5f, 0xd6, 0xe8, 0x03, 0x01, 0xaa}},
        }};

// Runtime packet dispatchers are used after Bedrock's generated schemas have
// been cached. Their shared_ptr argument contains the fully decoded packet.
inline constexpr std::uintptr_t kLevelChunkDispatcherOffset = 0x0c2b88e4;
inline constexpr std::uintptr_t kLevelChunkDispatcherVtableSlotOffset = 0x1209f3c0;
inline constexpr std::array<std::uint8_t, 16> kLevelChunkDispatcherSignature{
        0xff, 0x43, 0x01, 0xd1, 0xfd, 0x7b, 0x02, 0xa9,
        0xf5, 0x1b, 0x00, 0xf9, 0xf4, 0x4f, 0x04, 0xa9};

// LevelRendererCamera::render(BaseActorRenderContext&, ViewRenderObject const&,
// IClientInstance&) as overridden by LevelRendererPlayer. The inherited slot
// is used instead of a conditional block-entity pass, so every level render
// supplies one live context without retaining any client-owned pointers.
inline constexpr std::uintptr_t kLevelRenderFrameOffset = 0x0ae0aba8;
inline constexpr std::uintptr_t kLevelRenderFrameVtableSlotOffset =
        0x11fbe330;
inline constexpr std::array<std::uint8_t, 16>
        kLevelRenderFrameSignature{
                0xff, 0x83, 0x02, 0xd1, 0xfd, 0x7b, 0x06, 0xa9,
                0xf8, 0x5f, 0x07, 0xa9, 0xf6, 0x57, 0x08, 0xa9};
inline constexpr std::uintptr_t kLevelRendererPlayerVtableOffset =
        0x11fbe270;

// The Android implementation reads the live mce::Camera* directly from its
// BaseActorRenderContext argument at x1 + 0x18 before copying three
// MatrixStacks. Generated Windows headers are not ABI evidence for this
// Android libc++ object layout, so the target instructions are validated.
inline constexpr std::ptrdiff_t kLevelRenderCameraPointerOffset = 0x18;
inline constexpr std::uintptr_t kLevelRenderCameraPointerProbeOffset =
        0x0ae1a1bc;
inline constexpr std::array<std::uint8_t, 16>
        kLevelRenderCameraPointerProbeSignature{
                0xc0, 0x0e, 0x40, 0xf9, 0xbb, 0xb6, 0x66, 0x95,
                0x68, 0xc2, 0x44, 0xf9, 0xd8, 0x0e, 0x40, 0xf9};

// Capture only after Bedrock refreshes the Camera from the render context.
// At this point x19 is the renderer, x22 is BaseActorRenderContext, and x21
// is ViewRenderObject. All four overwritten instructions are replayable.
inline constexpr std::uintptr_t kLevelRenderCameraCaptureOffset =
        0x0ae1a1c4;
inline constexpr std::array<std::uint8_t, 16>
        kLevelRenderCameraCaptureSignature{
                0x68, 0xc2, 0x44, 0xf9, 0xd8, 0x0e, 0x40, 0xf9,
                0x74, 0x22, 0x1c, 0x91, 0x17, 0xe5, 0x41, 0xf9};

// The main render path subtracts this exact renderer-owned Vec3 from world
// coordinates. This is the target-proven world-camera position used by the
// level renderer, independent of generated cross-platform object headers.
inline constexpr std::ptrdiff_t kLevelRendererCameraPositionOffset = 0x6f4;
inline constexpr std::uintptr_t kLevelRendererCameraPositionUseProbeOffset =
        0x0ae0ad30;
inline constexpr std::array<std::uint8_t, 16>
        kLevelRendererCameraPositionUseProbeSignature{
                0x01, 0x08, 0x40, 0x2d, 0x60, 0xf6, 0x46, 0xbd,
                0x63, 0xfa, 0x46, 0xbd, 0x64, 0xfe, 0x46, 0xbd};

// LevelRendererCamera's constructor receives Level& in x2, preserves it in
// x22, and stores it at +0x958. Validate both that exact store and an
// independent load-plus-Level-virtual-call before enabling capture.
inline constexpr std::ptrdiff_t kLevelRendererLevelOffset = 0x958;
inline constexpr std::uintptr_t kLevelRendererLevelLayoutProbeOffset =
        0x0ae2354c;
inline constexpr std::array<std::uint8_t, 16>
        kLevelRendererLevelLayoutProbeSignature{
                0x76, 0xae, 0x04, 0xf9, 0x76, 0x82, 0x25, 0x91,
                0x60, 0x52, 0x82, 0x3d, 0x1f, 0xd1, 0x02, 0xf8};
inline constexpr std::uintptr_t kLevelRendererLevelUseProbeOffset =
        0x0ae23c64;
inline constexpr std::array<std::uint8_t, 16>
        kLevelRendererLevelUseProbeSignature{
                0x60, 0xae, 0x44, 0xf9, 0x08, 0x00, 0x40, 0xf9,
                0x08, 0x35, 0x41, 0xf9, 0x00, 0x01, 0x3f, 0xd6};

inline constexpr std::uintptr_t kLevelChunkVtableOffset = 0x12073298;

inline constexpr std::uintptr_t kSubChunkDispatcherOffset = 0x0c2bb704;
inline constexpr std::uintptr_t kSubChunkDispatcherVtableSlotOffset = 0x120a3080;
inline constexpr std::array<std::uint8_t, 16> kSubChunkDispatcherSignature{
        0x48, 0x00, 0x40, 0xf9, 0xe0, 0x03, 0x02, 0xaa,
        0x62, 0x00, 0x40, 0xf9, 0x03, 0x71, 0x41, 0xf9};
inline constexpr std::uintptr_t kSubChunkVtableOffset = 0x120e9150;

// LoopbackPacketSender::send(Packet&) is the client outbound vtable path. It
// safely delegates to sendToServer without requiring an inline trampoline.
inline constexpr std::uintptr_t kLoopbackSendOffset = 0x0c2de4a4;
inline constexpr std::uintptr_t kLoopbackSendVtableSlotOffset = 0x120a55a8;
inline constexpr std::array<std::uint8_t, 16> kLoopbackSendSignature{
        0xff, 0x43, 0x01, 0xd1, 0xfd, 0x7b, 0x03, 0xa9,
        0xf3, 0x23, 0x00, 0xf9, 0xfd, 0xc3, 0x00, 0x91};

inline constexpr std::uintptr_t kSubChunkRequestVtableOffset = 0x120e8e30;
inline constexpr std::ptrdiff_t kSubChunkRequestVectorBeginOffset = 0x38;
inline constexpr std::ptrdiff_t kSubChunkRequestVectorEndOffset = 0x40;
inline constexpr std::uintptr_t kSubChunkPositionSize = 12;
inline constexpr std::ptrdiff_t kSubChunkResponseVectorBeginOffset = 0x38;
inline constexpr std::ptrdiff_t kSubChunkResponseVectorEndOffset = 0x40;
inline constexpr std::uintptr_t kSubChunkPacketDataSize = 576;

// BaseActorRenderContext::getProjectionMatrix(). This validates the context ->
// ScreenContext -> Camera path used by the passive overlay capture.
inline constexpr std::uintptr_t kProjectionMatrixGetterOffset = 0x0a5d8b94;
inline constexpr std::array<std::uint8_t, 16> kProjectionMatrixGetterSignature{
        0x08, 0x14, 0x40, 0xf9, 0x08, 0x0d, 0x40, 0xf9,
        0x00, 0x41, 0x02, 0x91, 0xc0, 0x03, 0x5f, 0xd6};

// BaseActorRenderContext::getViewMatrix(). It returns the Camera itself because
// the view MatrixStack is the first camera member.
inline constexpr std::uintptr_t kViewMatrixGetterOffset = 0x0a5d8ba4;
inline constexpr std::array<std::uint8_t, 12> kViewMatrixGetterSignature{
        0x08, 0x14, 0x40, 0xf9, 0x00, 0x0d, 0x40, 0xf9,
        0xc0, 0x03, 0x5f, 0xd6};

// BaseActorRenderContext::getCameraPosition(). Actor AABBs use world
// coordinates while Camera::mPosition is render-relative on this target.
inline constexpr std::uintptr_t kCameraPositionGetterOffset = 0x0a5d8b70;
inline constexpr std::array<std::uint8_t, 12> kCameraPositionGetterSignature{
        0x08, 0x54, 0x40, 0xf9, 0x00, 0xd1, 0x00, 0x91,
        0xc0, 0x03, 0x5f, 0xd6};

// Android libc++ gives MatrixStack a 0x48-byte layout. The validated
// projection getter above returns Camera + 0x90, proving two preceding
// MatrixStacks of that size. The camera vectors follow the third stack and
// the 0x40-byte inverse-view matrix.
inline constexpr std::ptrdiff_t kRenderContextScreenContextOffset = 0x28;
inline constexpr std::ptrdiff_t kRenderContextCameraStateOffset = 0xa8;
inline constexpr std::ptrdiff_t kRenderCameraStatePositionOffset = 0x34;
// Actor::getLevel() at image offset 0x0eca7920 loads this exact member.
// Nearby code confirms that 0x1c8 belongs to a different actor field.
inline constexpr std::ptrdiff_t kActorLevelOffset = 0x1d0;
inline constexpr std::uintptr_t kActorGetLevelOffset = 0x0eca7920;
inline constexpr std::array<std::uint8_t, 8> kActorGetLevelSignature{
        0x00, 0xe8, 0x40, 0xf9, 0xc0, 0x03, 0x5f, 0xd6};
inline constexpr std::ptrdiff_t kScreenContextCameraOffset = 0x18;
inline constexpr std::ptrdiff_t kCameraProjectionStackOffset = 0x90;
inline constexpr std::ptrdiff_t kCameraRightOffset = 0x118;
inline constexpr std::ptrdiff_t kCameraUpOffset = 0x124;
inline constexpr std::ptrdiff_t kCameraForwardOffset = 0x130;
inline constexpr std::ptrdiff_t kCameraPositionOffset = 0x13c;

// Android libc++ std::deque<Matrix> fields inside MatrixStack.
inline constexpr std::ptrdiff_t kMatrixStackMapBeginOffset = 0x08;
inline constexpr std::ptrdiff_t kMatrixStackMapEndOffset = 0x10;
inline constexpr std::ptrdiff_t kMatrixStackStartOffset = 0x20;
inline constexpr std::ptrdiff_t kMatrixStackSizeOffset = 0x28;
inline constexpr std::size_t kMatrixBytes = 0x40;
inline constexpr std::size_t kMatricesPerDequeBlock = 64;

} // namespace target

} // namespace dobby
