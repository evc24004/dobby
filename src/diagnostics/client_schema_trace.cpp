#include "diagnostics/client_schema_trace.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <system_error>

namespace dobby {
namespace {

constexpr std::size_t kMaximumSchemaDepth = 32;
constexpr std::size_t kMaximumMemberLength = 128;
constexpr std::size_t kMaximumPathLength = 512;

struct ClientSchemaContext {
    std::array<std::string, kMaximumSchemaDepth> components;
    std::size_t depth{};
};

thread_local ClientSchemaContext context;

} // namespace

void pushClientSchemaMember(std::string_view name) {
    if (context.depth >= context.components.size())
        return;
    context.components[context.depth++].assign(
            name.substr(0, kMaximumMemberLength));
}

void pushClientSchemaElement(std::uint64_t index) {
    if (context.depth >= context.components.size())
        return;
    auto& component = context.components[context.depth++];
    component.clear();
    component.push_back('[');
    std::array<char, 24> digits{};
    const auto converted = std::to_chars(
            digits.data(), digits.data() + digits.size(), index);
    if (converted.ec == std::errc{})
        component.append(digits.data(), converted.ptr);
    component.push_back(']');
}

void popClientSchemaContext() {
    if (context.depth == 0)
        return;
    context.components[--context.depth].clear();
}

std::string currentClientSchemaPath() {
    std::string result;
    writeCurrentClientSchemaPath(result);
    return result;
}

void writeCurrentClientSchemaPath(std::string& destination) {
    destination.clear();
    for (std::size_t index = 0; index < context.depth; ++index) {
        const auto& component = context.components[index];
        if (component.empty())
            continue;
        if (!destination.empty() && component.front() != '[')
            destination += '.';
        if (destination.size() + component.size() > kMaximumPathLength)
            break;
        destination += component;
    }
}

void clearClientSchemaTrace() {
    context = {};
}

} // namespace dobby
