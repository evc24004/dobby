#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dobby {

struct ProtocolFieldObservation {
    std::string path;
    std::string type;
};

struct ProtocolPacketObservation {
    std::int32_t id{};
    std::string runtimeName;
    std::int32_t serializationMode{};
    bool serialized{};
    bool complete{};
    std::string limitation;
    std::vector<ProtocolFieldObservation> fields;
};

struct ProtocolDumpObservation {
    std::string minecraftVersion;
    std::string minecraftBuildId;
    std::int32_t protocolVersion{};
    std::int32_t highestFactoryId{};
    std::vector<ProtocolPacketObservation> packets;
};

std::string protocolPacketName(std::string_view runtimeName);
std::string buildProtocolJson(const ProtocolDumpObservation& dump);
std::string buildProtocolVersionJson(const ProtocolDumpObservation& dump);
std::string buildProtocolStatusJson(
        const ProtocolDumpObservation& dump, bool referenceVerified = false);

} // namespace dobby
