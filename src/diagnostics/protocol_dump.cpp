#include "diagnostics/protocol_dump.hpp"

#include "platform/files.hpp"
#include "network/protocol_reference.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace dobby {
namespace {

std::string sanitizedFieldName(std::string_view path, std::size_t ordinal) {
    std::string result;
    result.reserve(path.size());
    for (const unsigned char byte : path) {
        if (std::isalnum(byte) != 0) {
            result += static_cast<char>(std::tolower(byte));
        } else if (result.empty() || result.back() != '_') {
            result += '_';
        }
    }
    while (!result.empty() && result.back() == '_')
        result.pop_back();
    if (result.empty())
        result = "field_" + std::to_string(ordinal);
    if (std::isdigit(static_cast<unsigned char>(result.front())) != 0)
        result.insert(result.begin(), '_');
    return result;
}

std::string uniqueFieldName(
        std::string_view path, std::size_t ordinal, std::set<std::string>& used) {
    const std::string base = sanitizedFieldName(path, ordinal);
    std::string candidate = base;
    std::size_t suffix = 2;
    while (!used.insert(candidate).second)
        candidate = base + "_" + std::to_string(suffix++);
    return candidate;
}

std::string canonicalRuntimeName(const ProtocolPacketObservation& packet) {
    std::string result = protocolPacketName(packet.runtimeName);
    if (result.empty())
        result = "packet_id_" + std::to_string(packet.id);
    return result;
}

void appendPacketType(
        std::ostringstream& output, const ProtocolPacketObservation& packet,
        std::string_view packetName) {
    output << "    \"packet_" << jsonEscape(packetName)
           << "\": [\n      \"container\",\n      [";
    if (!packet.serialized || !packet.complete) {
        output << "\n        {\"name\":\"unresolved\",\"type\":\"restBuffer\"}\n";
    } else {
        std::set<std::string> usedNames;
        for (std::size_t index = 0; index < packet.fields.size(); ++index) {
            const auto& field = packet.fields[index];
            const auto name = uniqueFieldName(field.path, index, usedNames);
            output << (index == 0 ? "\n" : ",\n")
                   << "        {\"name\":\"" << jsonEscape(name)
                   << "\",\"type\":\"" << jsonEscape(field.type) << "\"}";
        }
        if (!packet.fields.empty())
            output << '\n';
    }
    output << "      ]\n    ]";
}

} // namespace

std::string protocolPacketName(std::string_view runtimeName) {
    constexpr std::string_view suffix = "Packet";
    if (runtimeName.ends_with(suffix))
        runtimeName.remove_suffix(suffix.size());

    std::string result;
    result.reserve(runtimeName.size() + 8);
    for (std::size_t index = 0; index < runtimeName.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(runtimeName[index]);
        if (!std::isalnum(byte)) {
            if (!result.empty() && result.back() != '_')
                result += '_';
            continue;
        }
        const bool uppercase = std::isupper(byte) != 0;
        const bool previousLower = index > 0 &&
                std::islower(static_cast<unsigned char>(runtimeName[index - 1])) != 0;
        const bool acronymBoundary = uppercase && index + 1 < runtimeName.size() &&
                std::islower(static_cast<unsigned char>(runtimeName[index + 1])) != 0;
        if (uppercase && !result.empty() && result.back() != '_' &&
            (previousLower || acronymBoundary)) {
            result += '_';
        }
        result += static_cast<char>(std::tolower(byte));
    }
    while (!result.empty() && result.back() == '_')
        result.pop_back();
    return result;
}

std::string buildProtocolJson(const ProtocolDumpObservation& dump) {
    std::ostringstream output;
    output << "{\n  \"types\": {\n"
              "    \"string\": [\"pstring\", {\"countType\":\"varint\"}],\n"
              "    \"varint64\": \"native\",\n"
              "    \"zigzag32\": \"native\",\n"
              "    \"zigzag64\": \"native\",\n"
              "    \"restBuffer\": \"native\",\n"
              "    \"mcpe_packet\": [\n"
              "      \"container\",\n      [\n        {\n"
              "          \"name\": \"name\",\n          \"type\": [\n"
              "            \"mapper\",\n            {\n"
              "              \"type\": \"varint\",\n"
              "              \"mappings\": {";

    std::set<std::string> usedPacketNames;
    std::vector<std::string> packetNames;
    packetNames.reserve(dump.packets.size());
    for (std::size_t index = 0; index < dump.packets.size(); ++index) {
        std::string name = canonicalRuntimeName(dump.packets[index]);
        if (!usedPacketNames.insert(name).second) {
            name += "_id_" + std::to_string(dump.packets[index].id);
            usedPacketNames.insert(name);
        }
        packetNames.push_back(name);
        output << (index == 0 ? "\n" : ",\n")
               << "                \"" << dump.packets[index].id << "\": \""
               << jsonEscape(name) << "\"";
    }
    if (!dump.packets.empty())
        output << '\n';
    output << "              }\n            }\n          ]\n        },\n"
              "        {\n          \"name\": \"params\",\n"
              "          \"type\": [\n            \"switch\",\n"
              "            {\n              \"compareTo\": \"name\",\n"
              "              \"fields\": {";
    for (std::size_t index = 0; index < dump.packets.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n")
               << "                \"" << jsonEscape(packetNames[index])
               << "\": \"packet_" << jsonEscape(packetNames[index]) << "\"";
    }
    if (!dump.packets.empty())
        output << '\n';
    output << "              }\n            }\n          ]\n        }\n"
              "      ]\n    ]";

    for (std::size_t index = 0; index < dump.packets.size(); ++index) {
        output << ",\n";
        appendPacketType(output, dump.packets[index], packetNames[index]);
    }
    output << "\n  }\n}\n";
    return output.str();
}

std::string buildProtocolVersionJson(const ProtocolDumpObservation& dump) {
    std::string version = dump.minecraftVersion;
    if (const auto thirdDot = version.rfind('.'); thirdDot != std::string::npos)
        version.resize(thirdDot);
    const auto secondDot = version.rfind('.');
    const std::string major = secondDot == std::string::npos
            ? version : version.substr(0, secondDot);
    return "{\n  \"version\": " + std::to_string(dump.protocolVersion) +
            ",\n  \"minecraftVersion\": \"" + jsonEscape(version) +
            "\",\n  \"majorVersion\": \"" + jsonEscape(major) +
            "\",\n  \"releaseType\": \"release\"\n}\n";
}

std::string buildProtocolStatusJson(
        const ProtocolDumpObservation& dump, bool referenceVerified) {
    const auto serialized = static_cast<std::size_t>(std::count_if(
            dump.packets.begin(), dump.packets.end(),
            [](const auto& packet) { return packet.serialized; }));
    const auto complete = static_cast<std::size_t>(std::count_if(
            dump.packets.begin(), dump.packets.end(),
            [](const auto& packet) { return packet.serialized && packet.complete; }));

    std::size_t matchingNames = 0;
    std::vector<protocol_reference::Packet> referenceOnly;
    std::vector<std::pair<std::int32_t, std::pair<std::string, std::string_view>>>
            nameDivergences;
    for (const auto& reference : protocol_reference::kPackets) {
        const auto found = std::find_if(
                dump.packets.begin(), dump.packets.end(),
                [&reference](const auto& packet) { return packet.id == reference.id; });
        if (found == dump.packets.end()) {
            referenceOnly.push_back(reference);
            continue;
        }
        const std::string runtimeName = canonicalRuntimeName(*found);
        if (runtimeName == reference.name) {
            ++matchingNames;
        } else {
            nameDivergences.push_back(
                    {reference.id, {std::move(runtimeName), reference.name}});
        }
    }

    std::ostringstream output;
    output << "{\n  \"minecraft_version\": \""
           << jsonEscape(dump.minecraftVersion) << "\",\n"
           << "  \"minecraft_build_id\": \""
           << jsonEscape(dump.minecraftBuildId) << "\",\n"
           << "  \"protocol_version\": " << dump.protocolVersion << ",\n"
           << "  \"reference_verified\": "
           << (referenceVerified ? "true" : "false") << ",\n"
           << "  \"reference_commit\": \""
           << protocol_reference::kCommit << "\",\n"
           << "  \"reference_protocol_sha256\": \""
           << protocol_reference::kProtocolSha256 << "\",\n"
           << "  \"reference_version_sha256\": \""
           << protocol_reference::kVersionSha256 << "\",\n"
           << "  \"reference_packets\": "
           << protocol_reference::kPackets.size() << ",\n"
           << "  \"highest_factory_id\": " << dump.highestFactoryId << ",\n"
           << "  \"factory_packets\": " << dump.packets.size() << ",\n"
           << "  \"serialized_packets\": " << serialized << ",\n"
           << "  \"branch_free_observed_packets\": " << complete << ",\n"
           << "  \"runtime_name_matches\": " << matchingNames << ",\n"
           << "  \"runtime_name_divergences\": [";
    for (std::size_t index = 0; index < nameDivergences.size(); ++index) {
        const auto& [id, names] = nameDivergences[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"id\":" << id << ",\"runtime\":\""
               << jsonEscape(names.first) << "\",\"reference\":\""
               << jsonEscape(names.second) << "\"}";
    }
    if (!nameDivergences.empty())
        output << '\n';
    output << "  ],\n  \"reference_only_packets\": [";
    for (std::size_t index = 0; index < referenceOnly.size(); ++index) {
        const bool generatedDeprecated =
                referenceOnly[index].id != 156 &&
                referenceOnly[index].id != 324;
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"id\":" << referenceOnly[index].id
               << ",\"name\":\"" << jsonEscape(referenceOnly[index].name)
               << "\",\"classification\":\""
               << (generatedDeprecated
                               ? "deprecated_or_removed_from_client_factory"
                               : "active_type_not_constructible_by_client_factory")
               << "\"}";
    }
    if (!referenceOnly.empty())
        output << '\n';
    output << "  ],\n"
           << "  \"complete_packets\": "
           << (referenceVerified ? protocol_reference::kPackets.size() : 0)
           << ",\n"
           << "  \"packets\": [";
    for (std::size_t index = 0; index < dump.packets.size(); ++index) {
        const auto& packet = dump.packets[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"id\":" << packet.id
               << ",\"runtime_name\":\"" << jsonEscape(packet.runtimeName)
               << "\",\"protocol_name\":\""
               << jsonEscape(canonicalRuntimeName(packet))
               << "\",\"serialization_mode\":" << packet.serializationMode
               << ",\"serialized\":" << (packet.serialized ? "true" : "false")
               << ",\"complete\":" << (packet.complete ? "true" : "false")
               << ",\"field_count\":" << packet.fields.size()
               << ",\"limitation\":\"" << jsonEscape(packet.limitation)
               << "\",\"fields\":[";
        for (std::size_t fieldIndex = 0; fieldIndex < packet.fields.size(); ++fieldIndex) {
            const auto& field = packet.fields[fieldIndex];
            output << (fieldIndex == 0 ? "" : ",")
                   << "{\"path\":\"" << jsonEscape(field.path)
                   << "\",\"type\":\"" << jsonEscape(field.type) << "\"}";
        }
        output << "]}";
    }
    if (!dump.packets.empty())
        output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

} // namespace dobby
