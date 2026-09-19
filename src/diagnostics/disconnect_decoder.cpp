#include "diagnostics/disconnect_decoder.hpp"

#include "diagnostics/violation_decoder.hpp"
#include "platform/safe_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace dobby {
namespace {

struct DisconnectIdentity {
    std::string_view name;
    std::string_view codeword;
};

// Connection::DisconnectFailReason is a contiguous enum in Minecraft
// 1.26.51.1. Names come from libminecraftpe's enum schema and codewords come
// from the matching disconnection_error_messaging.json asset. The array index
// is the observed integer passed to ClientNetworkHandler::onDisconnect.
constexpr std::array<DisconnectIdentity, 150> identities{{
    {"Unknown", "Creeper"},
    {"CantConnectNoInternet", "NetherNet"},
    {"NoPermissions", "Spyglass"},
    {"UnrecoverableError", "Chest"},
    {"ThirdPartyBlocked", "Fox"},
    {"ThirdPartyNoInternet", "NetherNet"},
    {"ThirdPartyBadIP", "Squid"},
    {"ThirdPartyNoServerOrServerLocked", "Silverfish"},
    {"VersionMismatch", "Chain"},
    {"SkinIssue", "Armor"},
    {"InviteSessionNotFound", "NetherNet"},
    {"EduLevelSettingsMissing", "Gold"},
    {"LocalServerNotFound", "Panda"},
    {"LegacyDisconnect", "NetherNet"},
    {"INTERNAL_UserLeaveGameAttempted", "Terracotta"},
    {"PlatformLockedSkinsError", "Armor"},
    {"RealmsWorldUnassigned", "Arrow"},
    {"RealmsServerCantConnect", "Arrow"},
    {"RealmsServerHidden", "Arrow"},
    {"RealmsServerDisabledBeta", "Arrow"},
    {"RealmsServerDisabled", "Arrow"},
    {"CrossPlatformDisabled", "Spyglass"},
    {"TESTONLY_CantConnect", "Terracotta"},
    {"SessionNotFound", "Goat"},
    {"ClientSettingsIncompatibleWithServer", "Terracotta"},
    {"ServerFull", "Hopper"},
    {"InvalidPlatformSkin", "Armor"},
    {"EditionVersionMismatch", "Chain"},
    {"EditionMismatch", "Chain"},
    {"LevelNewerThanExeVersion", "Breeze"},
    {"INTERNAL_NoFailOccurred", "Terracotta"},
    {"BannedSkin", "Armor"},
    {"Timeout", "Kelp"},
    {"ServerNotFound", "Clay"},
    {"OutdatedServer", "Chain"},
    {"OutdatedClient", "Chain"},
    {"NoPremiumPlatform", "Terracotta"},
    {"MultiplayerDisabled", "Spyglass"},
    {"NoWiFi", "NetherNet"},
    {"WorldCorruption", "Chest"},
    {"NoReason", "Creeper"},
    {"Disconnected", "Bat"},
    {"InvalidPlayer", "Guardian"},
    {"LoggedInOtherLocation", "Guardian"},
    {"ServerIdConflict", "Blaze"},
    {"NotAllowed", "Spyglass"},
    {"NotAuthenticated", "Guardian"},
    {"InvalidTenant", "Gold"},
    {"UnknownPacket", "Block"},
    {"UnexpectedPacket", "Block"},
    {"InvalidCommandRequestPacket", "Block"},
    {"HostSuspended", "Boat"},
    {"LoginPacketNoRequest", "Guardian"},
    {"LoginPacketNoCert", "Guardian"},
    {"MissingClient", "Snowball"},
    {"Kicked", "Evoker"},
    {"KickedForExploit", "Evoker"},
    {"KickedForIdle", "Evoker"},
    {"ResourcePackProblem", "Rabbit"},
    {"IncompatiblePack", "Rabbit"},
    {"OutOfStorage", "Chest"},
    {"InvalidLevel", "Boat"},
    {"DisconnectPacket", "Terracotta"},
    {"BlockMismatch", "Boat"},
    {"InvalidHeights", "Lead"},
    {"InvalidWidths", "Ladder"},
    {"ConnectionLost", "Terracotta"},
    {"ZombieConnection", "Terracotta"},
    {"Shutdown", "Cobweb"},
    {"ReasonNotSet", "Terracotta"},
    {"LoadingStateTimeout", "Boat"},
    {"ResourcePackLoadingFailed", "Rabbit"},
    {"SearchingForSessionLoadingScreenFailed", "Compass"},
    {"NetherNetProtocolVersion", "NetherNet"},
    {"SubsystemStatusError", "Echo Shard"},
    {"EmptyAuthFromDiscovery", "Guardian"},
    {"EmptyUrlFromDiscovery", "TNT"},
    {"ExpiredAuthFromDiscovery", "Guardian"},
    {"UnknownSignalServiceSignInFailure", "Door"},
    {"XBLJoinLobbyFailure", "Boat"},
    {"UnspecifiedClientInstanceDisconnection", "Feather"},
    {"NetherNetSessionNotFound", "NetherNet"},
    {"NetherNetCreatePeerConnection", "NetherNet"},
    {"NetherNetICE", "Door"},
    {"NetherNetConnectRequest", "NetherNet"},
    {"NetherNetConnectResponse", "NetherNet"},
    {"NetherNetNegotiationTimeout", "Door"},
    {"NetherNetInactivityTimeout", "NetherNet"},
    {"StaleConnectionBeingReplaced", "NetherNet"},
    {"RealmsSessionNotFound", "Terracotta"},
    {"BadPacket", "Block"},
    {"NetherNetFailedToCreateOffer", "NetherNet"},
    {"NetherNetFailedToCreateAnswer", "NetherNet"},
    {"NetherNetFailedToSetLocalDescription", "NetherNet"},
    {"NetherNetFailedToSetRemoteDescription", "NetherNet"},
    {"NetherNetNegotiationTimeoutWaitingForResponse", "NetherNet"},
    {"NetherNetNegotiationTimeoutWaitingForAccept", "NetherNet"},
    {"NetherNetIncomingConnectionIgnored", "NetherNet"},
    {"NetherNetSignalingParsingFailure", "Door"},
    {"NetherNetSignalingUnknownError", "Door"},
    {"NetherNetSignalingUnicastDeliveryFailed", "Door"},
    {"NetherNetSignalingBroadcastDeliveryFailed", "Door"},
    {"NetherNetSignalingGenericDeliveryFailed", "Door"},
    {"EditorMismatchEditorWorld", "Emerald"},
    {"EditorMismatchVanillaWorld", "Emerald"},
    {"WorldTransferNotPrimaryClient", "Block"},
    {"INTERNAL_RequestServerShutdown", "Terracotta"},
    {"ClientGameSetupCancelled", "Boat"},
    {"ClientGameSetupFailed", "Boat"},
    {"NoVenue", "Terracotta"},
    {"NetherNetSignalingSigninFailed", "Door"},
    {"SessionAccessDenied", "Guardian"},
    {"ServiceSigninIssue", "Guardian"},
    {"NetherNetNoSignalingChannel", "Door"},
    {"NetherNetNotLoggedIn", "NetherNet"},
    {"NetherNetClientSignalingError", "Door"},
    {"SubClientLoginDisabled", "Coal"},
    {"DeepLinkTryingToOpenDemoWorldWhileSignedIn", "Honeycomb"},
    {"AsyncJoinTaskDenied", "Arrow"},
    {"RealmsTimelineRequired", "Arrow"},
    {"GuestWithoutHost", "Bamboo"},
    {"FailedToJoinExperience", "Terracotta"},
    {"NetherNetDataChannelClosed", "NetherNet"},
    {"DiscoveryEnvironmentMismatch", "Guardian"},
    {"HostWithoutKeys", "Guardian"},
    {"HostSignedOut", "Terracotta"},
    {"ScriptWatchdogException", "Terracotta"},
    {"ScriptMemoryLimitExceeded", "Terracotta"},
    {"StorageLowDuringGameplay", "Chest"},
    {"StorageFullDuringGameplay", "Chest"},
    {"LevelStorageCorruption", "Chest"},
    {"EditionMismatchVanillaToEdu", "Chain"},
    {"EditionMismatchEduToVanilla", "Chain"},
    {"EditorMismatchEditorToVanilla", "Emerald"},
    {"EditorMismatchVanillaToEditor", "Emerald"},
    {"DenyListed", "Spyglass"},
    {"NonceMissing", "Spyglass"},
    {"NonceNotFound", "Spyglass"},
    {"NonceExpired", "Spyglass"},
    {"NonceNotValid", "Spyglass"},
    {"HostDisconnected", "Goat"},
    {"EditorJoinIntentPolicyFailure", "Emerald"},
    {"NetherNetIdentityNotAllowed", "Door"},
    {"InvalidName", "Guardian"},
    {"ExpiredToken", "Guardian"},
    {"HostAcceptsNoTypeOfAuth", "Boat"},
    {"NotAuthenticatedFastFail", "Guardian"},
    {"EditorNotAllowed", "Emerald"},
    {"MissingStructureData", "Boat"},
    {"UnsupportedTransport", "Terracotta"},
}};

struct DecodedString {
    std::string value;
    std::string storage;
};

template <class T>
bool copyValue(const void* source, T& value) {
    return copyReadableMemory(
            source, std::span<std::byte>(
                    reinterpret_cast<std::byte*>(&value), sizeof(value)));
}

std::optional<DecodedString> readAndroidString(const void* object) {
    if (object == nullptr)
        return DecodedString{};

    const auto* bytes = static_cast<const std::byte*>(object);
    std::uint8_t tag = 0;
    if (!copyValue(bytes, tag))
        return std::nullopt;
    if ((tag & 1U) == 0U) {
        const auto length = static_cast<std::size_t>(tag >> 1U);
        if (length > 22)
            return std::nullopt;
        std::string value(length, '\0');
        if (length != 0 && !copyReadableMemory(
                                   bytes + 1,
                                   std::span<std::byte>(
                                           reinterpret_cast<std::byte*>(value.data()),
                                           value.size()))) {
            return std::nullopt;
        }
        return DecodedString{std::move(value), "short"};
    }

    std::size_t length = 0;
    const char* data = nullptr;
    if (!copyValue(bytes + 8, length) || !copyValue(bytes + 16, data) ||
        data == nullptr || length > kMaximumContextLength) {
        return std::nullopt;
    }
    std::string value(length, '\0');
    if (length != 0 && !copyReadableMemory(
                               data,
                               std::span<std::byte>(
                                       reinterpret_cast<std::byte*>(value.data()),
                                       value.size()))) {
        return std::nullopt;
    }
    return DecodedString{std::move(value), "long"};
}

} // namespace

std::string_view disconnectReasonName(std::int32_t reason) {
    if (reason < 0 || static_cast<std::size_t>(reason) >= identities.size())
        return "Unrecognized";
    return identities[static_cast<std::size_t>(reason)].name;
}

std::string_view disconnectCodeword(std::int32_t reason) {
    if (reason < 0 || static_cast<std::size_t>(reason) >= identities.size())
        return "Terracotta";
    return identities[static_cast<std::size_t>(reason)].codeword;
}

std::optional<DisconnectEvidence> decodeDisconnectArguments(
        std::int32_t reason, std::int32_t stage,
        const void* messageFromServer, const void* messageBodyOverride,
        bool skipMessage, bool sourcePresent, bool telemetryOverridePresent) {
    const auto serverMessage = readAndroidString(messageFromServer);
    const auto bodyOverride = readAndroidString(messageBodyOverride);
    if (!serverMessage || !bodyOverride)
        return std::nullopt;

    DisconnectEvidence result;
    result.reason = reason;
    result.reasonName = disconnectReasonName(reason);
    result.codeword = disconnectCodeword(reason);
    result.stage = stage;
    result.skipMessage = skipMessage;
    result.sourcePresent = sourcePresent;
    result.telemetryOverridePresent = telemetryOverridePresent;
    result.messageFromServer = serverMessage->value;
    result.messageBodyOverride = bodyOverride->value;
    result.messageFromServerStorage = serverMessage->storage;
    result.messageBodyOverrideStorage = bodyOverride->storage;
    return result;
}

} // namespace dobby
