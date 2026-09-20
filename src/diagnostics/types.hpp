#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dobby {

enum class DiagnosticKind {
    packetViolation,
    disconnect,
};

struct ViolationRecord {
    std::int32_t type{};
    std::int32_t severity{};
    std::int32_t packetId{};
    std::string context;
    std::string contextStorage;
};

struct StreamReadAttempt {
    std::size_t offset{};
    std::size_t requested{};
    std::size_t available{};
    bool overflow{};
    std::string clientField;
};

struct StreamFailure {
    bool overflowObserved{};
    bool packetEndMismatch{};
    std::size_t viewSize{};
    std::size_t failureOffset{};
    std::size_t requested{};
    std::size_t available{};
    bool overflowBeforeRead{};
    bool rawBytesTruncated{};
    std::vector<std::uint8_t> rawBytes;
    std::vector<StreamReadAttempt> attempts;
};

struct PacketHistoryEntry {
    std::int32_t packetId{};
    std::uint64_t packetSize{};
    std::uint64_t ageMilliseconds{};
};

struct ResourcePackClientResponseEvidence {
    std::int32_t status{-1};
    std::string serializedStatusName;
    std::vector<std::string> resourcePackIds;
    bool rawBytesTruncated{};
    std::vector<std::uint8_t> rawBytes;
    bool decodeComplete{};
    std::string decodeError;
};

struct OutboundPacketHistoryEntry {
    std::int32_t packetId{};
    std::uint64_t ageMilliseconds{};
    std::optional<ResourcePackClientResponseEvidence> resourcePackResponse;
};

struct TransportDisconnectEvidence {
    std::uint32_t messageId{};
    std::string messageName;
    std::uint32_t packetLength{};
    std::uint64_t ageMilliseconds{};
    bool rawBytesTruncated{};
    std::vector<std::uint8_t> rawBytes;
};

struct RakNetKeepaliveEvidence {
    bool sessionStartObserved{};
    std::uint64_t sessionAgeMilliseconds{};
    std::uint64_t connectedPingCount{};
    std::optional<std::uint64_t> lastConnectedPingAgeMilliseconds;
    std::uint64_t connectedPongCount{};
    std::optional<std::uint64_t> lastConnectedPongAgeMilliseconds;
    std::uint64_t detectLostConnectionsCount{};
    std::optional<std::uint64_t> lastDetectLostConnectionsAgeMilliseconds;
};

struct ContentDownloadEvidence {
    std::string contentId;
    std::string productId;
    std::string processState;
    std::string initiatorCategory;
    std::string packType;
    std::string packVersion;
    bool worldPack{};
    bool silent{};
    bool partialFilePresent{};
    std::uint64_t partialBytes{};
    bool completeFilePresent{};
    std::uint64_t completeBytes{};
};

struct ContentDownloadStateEvidence {
    bool decodeComplete{};
    std::string decodeError;
    std::vector<ContentDownloadEvidence> downloads;
};

struct ValidationSourceEvidence {
    std::uint64_t filenameHash{};
    std::string filename;
    std::uint32_t line{};
    std::string context;
};

struct ValidationNestedErrorEvidence {
    std::uint32_t depth{};
    std::int32_t errorValue{};
    std::string errorCategory;
    std::string errorMessage;
    std::vector<ValidationSourceEvidence> sourceFrames;
};

struct ValidationEvidence {
    bool resultSuccess{};
    std::int32_t response{};
    bool newOrUpdated{};
    std::int32_t errorValue{};
    std::string errorCategory;
    std::string errorMessage;
    std::vector<ValidationSourceEvidence> sourceFrames;
    std::vector<ValidationNestedErrorEvidence> nestedErrors;
    bool provenanceTruncated{};
    std::vector<std::uint64_t> nativeStackImageOffsets;
    std::vector<PacketHistoryEntry> recentPackets;
};

struct DisconnectEvidence {
    std::int32_t reason{};
    std::string reasonName;
    std::string codeword;
    std::int32_t stage{};
    bool skipMessage{};
    bool sourcePresent{};
    bool telemetryOverridePresent{};
    std::string messageFromServer;
    std::string messageBodyOverride;
    std::string messageFromServerStorage;
    std::string messageBodyOverrideStorage;
    std::vector<std::uint64_t> nativeStackImageOffsets;
    std::vector<PacketHistoryEntry> recentPackets;
    std::vector<OutboundPacketHistoryEntry> recentOutboundPackets;
    std::optional<TransportDisconnectEvidence> transport;
    std::optional<RakNetKeepaliveEvidence> rakNetKeepalive;
    std::optional<ContentDownloadStateEvidence> contentDownloads;
};

struct Diagnostic {
    DiagnosticKind kind{DiagnosticKind::packetViolation};
    std::string capturedAt;
    std::int32_t type{};
    std::int32_t severity{};
    std::int32_t packetId{};
    std::string context;
    std::string contextStorage;
    std::string intercept;
    std::optional<StreamFailure> streamFailure;
    std::optional<ValidationEvidence> validation;
    std::optional<DisconnectEvidence> disconnect;
    std::string json;
    std::string report;
};

} // namespace dobby
