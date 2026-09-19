#include "diagnostics/resource_pack_response.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace dobby {
namespace {

constexpr std::size_t maximumStatusNameLength = 128;
constexpr std::size_t maximumResourcePackIds = 64;
constexpr std::size_t maximumResourcePackIdLength = 1024;

bool readUnsignedVarInt(
        std::span<const std::uint8_t> bytes, std::size_t& cursor,
        std::uint32_t& result) {
    result = 0;
    for (std::uint32_t shift = 0; shift < 35; shift += 7) {
        if (cursor >= bytes.size())
            return false;
        const auto byte = bytes[cursor++];
        if (shift == 28 && (byte & 0xf0U) != 0)
            return false;
        result |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0)
            return true;
    }
    return false;
}

bool readString(
        std::span<const std::uint8_t> bytes, std::size_t& cursor,
        std::size_t maximumLength, std::string& result) {
    std::uint32_t length = 0;
    if (!readUnsignedVarInt(bytes, cursor, length) ||
        length > maximumLength || length > bytes.size() - cursor) {
        return false;
    }
    result.assign(
            reinterpret_cast<const char*>(bytes.data() + cursor), length);
    cursor += length;
    return true;
}

} // namespace

const char* resourcePackResponseStatusName(std::int32_t status) {
    switch (status) {
    case 0: return "refused";
    case 1: return "send_packs";
    case 2: return "have_all_packs";
    case 3: return "completed";
    default: return "unknown";
    }
}

ResourcePackClientResponseEvidence decodeResourcePackClientResponse(
        std::span<const std::uint8_t> bytes, bool rawBytesTruncated) {
    ResourcePackClientResponseEvidence result;
    result.rawBytes.assign(bytes.begin(), bytes.end());
    result.rawBytesTruncated = rawBytesTruncated;

    std::size_t cursor = 0;
    std::uint32_t status = 0;
    if (!readUnsignedVarInt(bytes, cursor, status) ||
        status > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        result.decodeError = "invalid or incomplete response status varint";
        return result;
    }
    result.status = static_cast<std::int32_t>(status);
    if (!readString(bytes, cursor, maximumStatusNameLength, result.serializedStatusName)) {
        result.decodeError = "invalid or incomplete response status name";
        return result;
    }

    if (result.status == 1 && cursor < bytes.size()) {
        std::uint32_t count = 0;
        if (!readUnsignedVarInt(bytes, cursor, count) ||
            count > maximumResourcePackIds) {
            result.decodeError = "invalid resource-pack ID count";
            return result;
        }
        result.resourcePackIds.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            std::string id;
            if (!readString(bytes, cursor, maximumResourcePackIdLength, id)) {
                result.decodeError = "invalid or incomplete resource-pack ID";
                return result;
            }
            result.resourcePackIds.push_back(std::move(id));
        }
    }

    if (cursor != bytes.size()) {
        result.decodeError = "unparsed trailing response bytes";
        return result;
    }
    if (rawBytesTruncated) {
        result.decodeError = "serialized response capture was truncated";
        return result;
    }
    result.decodeComplete = true;
    return result;
}

} // namespace dobby
