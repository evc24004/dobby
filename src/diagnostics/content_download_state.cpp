#include "diagnostics/content_download_state.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace dobby {
namespace {

constexpr std::size_t maximumTrackers = 32;
constexpr std::size_t maximumFieldLength = 512;

std::size_t skipWhitespace(std::string_view text, std::size_t cursor) {
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
        ++cursor;
    }
    return cursor;
}

std::optional<std::size_t> valueStart(
        std::string_view object, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto keyPosition = object.find(needle);
    if (keyPosition == std::string_view::npos)
        return std::nullopt;
    const auto colon = object.find(':', keyPosition + needle.size());
    if (colon == std::string_view::npos)
        return std::nullopt;
    return skipWhitespace(object, colon + 1);
}

std::optional<std::string> stringField(
        std::string_view object, std::string_view key) {
    const auto start = valueStart(object, key);
    if (!start || *start >= object.size() || object[*start] != '"')
        return std::nullopt;
    std::string result;
    result.reserve(64);
    bool escaped = false;
    for (std::size_t cursor = *start + 1; cursor < object.size(); ++cursor) {
        const char value = object[cursor];
        if (escaped) {
            switch (value) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: return std::nullopt;
            }
            escaped = false;
        } else if (value == '\\') {
            escaped = true;
        } else if (value == '"') {
            return result;
        } else {
            result += value;
        }
        if (result.size() > maximumFieldLength)
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<bool> boolField(
        std::string_view object, std::string_view key) {
    const auto start = valueStart(object, key);
    if (!start)
        return std::nullopt;
    if (object.substr(*start, 4) == "true")
        return true;
    if (object.substr(*start, 5) == "false")
        return false;
    return std::nullopt;
}

std::optional<std::string_view> trackersArray(std::string_view json) {
    const auto start = valueStart(json, "trackers");
    if (!start || *start >= json.size() || json[*start] != '[')
        return std::nullopt;
    bool inString = false;
    bool escaped = false;
    std::size_t depth = 0;
    for (std::size_t cursor = *start; cursor < json.size(); ++cursor) {
        const char value = json[cursor];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                inString = false;
            }
            continue;
        }
        if (value == '"') {
            inString = true;
        } else if (value == '[') {
            ++depth;
        } else if (value == ']') {
            if (--depth == 0)
                return json.substr(*start + 1, cursor - *start - 1);
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> nextObject(
        std::string_view array, std::size_t& cursor) {
    cursor = skipWhitespace(array, cursor);
    while (cursor < array.size() && array[cursor] != '{')
        ++cursor;
    if (cursor == array.size())
        return std::nullopt;
    const auto start = cursor;
    bool inString = false;
    bool escaped = false;
    std::size_t depth = 0;
    for (; cursor < array.size(); ++cursor) {
        const char value = array[cursor];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                inString = false;
            }
            continue;
        }
        if (value == '"') {
            inString = true;
        } else if (value == '{') {
            ++depth;
        } else if (value == '}') {
            if (--depth == 0) {
                ++cursor;
                return array.substr(start, cursor - start);
            }
        }
    }
    return std::nullopt;
}

} // namespace

ContentDownloadStateEvidence decodeContentDownloadState(std::string_view json) {
    ContentDownloadStateEvidence result;
    const auto array = trackersArray(json);
    if (!array) {
        result.decodeError = "trackers array was missing or malformed";
        return result;
    }

    std::size_t cursor = 0;
    while (result.downloads.size() < maximumTrackers) {
        const auto object = nextObject(*array, cursor);
        if (!object)
            break;
        ContentDownloadEvidence download;
        download.contentId = stringField(*object, "contentId").value_or("");
        download.productId = stringField(*object, "productId").value_or("");
        download.processState = stringField(*object, "processState").value_or("");
        download.initiatorCategory =
                stringField(*object, "initiatorCategory").value_or("");
        download.packType = stringField(*object, "type").value_or("");
        download.packVersion = stringField(*object, "version").value_or("");
        download.worldPack = boolField(*object, "worldPack").value_or(false);
        download.silent = boolField(*object, "silent").value_or(false);
        if (!download.contentId.empty())
            result.downloads.push_back(std::move(download));
    }
    result.decodeComplete = true;
    return result;
}

} // namespace dobby
