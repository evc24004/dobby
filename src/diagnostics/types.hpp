#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dobby {

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

struct Diagnostic {
    std::string capturedAt;
    std::int32_t type{};
    std::int32_t severity{};
    std::int32_t packetId{};
    std::string context;
    std::string contextStorage;
    std::string intercept;
    std::optional<StreamFailure> streamFailure;
    std::optional<ValidationEvidence> validation;
    std::string json;
    std::string report;
};

} // namespace dobby
