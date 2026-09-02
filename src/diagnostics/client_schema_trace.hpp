#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dobby {

void pushClientSchemaMember(std::string_view name);
void pushClientSchemaElement(std::uint64_t index);
void popClientSchemaContext();
std::string currentClientSchemaPath();
void writeCurrentClientSchemaPath(std::string& destination);
void clearClientSchemaTrace();

} // namespace dobby
