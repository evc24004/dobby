#include "hooks/protocol_dump_hook.hpp"

#if defined(__ANDROID__)

#include "core/constants.hpp"
#include "diagnostics/protocol_dump.hpp"
#include "hooks/minecraft_image.hpp"
#include "platform/files.hpp"
#include "platform/launcher.hpp"
#include "platform/log.hpp"
#include "network/protocol_reference.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dobby {
namespace {

constexpr std::size_t kMaximumFieldsPerPacket = 4096;
constexpr std::size_t kMaximumFieldNameLength = 192;
constexpr std::size_t kMaximumFieldPathLength = 768;
// These factory defaults expose owning connection-request pointers that are
// empty even when SubClientLoginPacket::isValid() reports true. Both failures
// were proven at the exact serializer boundary by the startup progress trace.
constexpr std::array<std::int32_t, 2> kDefaultStateUnsafePacketIds{1, 94};

struct ActiveCapture {
    ProtocolPacketObservation* packet{};
    std::vector<std::string> context;
};

struct PatchRecord {
    void** slot{};
    void* original{};
};

thread_local ActiveCapture* activeCapture = nullptr;
// BinaryStream's public primitive writers are layered. For example,
// writeVarInt() calls writeUnsignedVarInt(), which eventually calls
// writeByte(). All three functions are virtual and therefore all three pass
// through the temporary probe. Only the outermost call describes a protocol
// field; the nested calls are implementation details for the same bytes.
thread_local std::size_t primitiveWriteDepth = 0;
std::atomic_bool dumpStarted{false};

extern "C" const unsigned char
        dobby_protocol_reference_start[];
extern "C" const unsigned char
        dobby_protocol_reference_end[];
extern "C" const unsigned char
        dobby_protocol_version_reference_start[];
extern "C" const unsigned char
        dobby_protocol_version_reference_end[];

std::uint64_t fnv1a64(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string_view embeddedReference(
        const unsigned char* start, const unsigned char* end) {
    return {reinterpret_cast<const char*>(start),
            static_cast<std::size_t>(end - start)};
}

using ByteWriterFn = void (*)(void*, std::uint8_t, const char*, const char*);
using SignedByteWriterFn = void (*)(void*, std::int8_t, const char*, const char*);
using UnsignedShortWriterFn = void (*)(void*, std::uint16_t, const char*, const char*);
using SignedShortWriterFn = void (*)(void*, std::int16_t, const char*, const char*);
using UnsignedIntWriterFn = void (*)(void*, std::uint32_t, const char*, const char*);
using SignedIntWriterFn = void (*)(void*, std::int32_t, const char*, const char*);
using UnsignedInt64WriterFn = void (*)(void*, std::uint64_t, const char*, const char*);
using SignedInt64WriterFn = void (*)(void*, std::int64_t, const char*, const char*);
using BoolWriterFn = void (*)(void*, bool, const char*, const char*);
using FloatWriterFn = void (*)(void*, float, const char*, const char*);
using DoubleWriterFn = void (*)(void*, double, const char*, const char*);
using FixedFloatWriterFn = void (*)(void*, float, const char*, const char*, double);
using StringWriterFn = void (*)(void*, std::string_view, const char*, const char*);
using WriteIfFn = void (*)(void*, bool, const char*, const void*, const void*);
using WriteConditionalFn = void (*)(void*, const char*, const void*, const void*);
using BranchRangeFn = void (*)(void*, void*, std::int32_t, std::int32_t,
                               std::int32_t, const char*);
using BranchVectorFn = void (*)(void*, void*, std::int32_t, const void*, const char*);
using TypeBeginFn = bool (*)(void*, const char*, const char*, const char*);
using TypeEndFn = void (*)(void*);
using EnumFn = void (*)(void*, const char*, const char*);
using ArrayFn = void (*)(void*, void*, void*, const char*, const char*);

BoolWriterFn originalWriteBool{};
ByteWriterFn originalWriteByte{};
SignedByteWriterFn originalWriteSignedByte{};
UnsignedShortWriterFn originalWriteUnsignedShort{};
SignedShortWriterFn originalWriteSignedShort{};
UnsignedIntWriterFn originalWriteUnsignedInt{};
SignedIntWriterFn originalWriteSignedBigEndianInt{};
SignedIntWriterFn originalWriteSignedInt{};
UnsignedInt64WriterFn originalWriteUnsignedInt64{};
SignedInt64WriterFn originalWriteSignedInt64{};
UnsignedIntWriterFn originalWriteUnsignedVarInt{};
UnsignedInt64WriterFn originalWriteUnsignedVarInt64{};
SignedIntWriterFn originalWriteVarInt{};
SignedInt64WriterFn originalWriteVarInt64{};
DoubleWriterFn originalWriteDouble{};
FloatWriterFn originalWriteFloat{};
FixedFloatWriterFn originalWriteFixedFloat{};
FloatWriterFn originalWriteNormalizedFloat{};
StringWriterFn originalWriteString{};
WriteIfFn originalWriteIf{};
WriteConditionalFn originalWriteConditional{};
BranchRangeFn originalBranchRange{};
BranchVectorFn originalBranchVector{};
TypeBeginFn originalTypeBegin{};
TypeEndFn originalTypeEnd{};
EnumFn originalEnum{};
ArrayFn originalArray{};

using SchemaNullFn = bool (*)(void*);
using SchemaBoolFn = bool (*)(void*, bool);
using SchemaSignedByteFn = bool (*)(void*, std::int8_t);
using SchemaByteFn = bool (*)(void*, std::uint8_t);
using SchemaShortFn = bool (*)(void*, std::int16_t);
using SchemaUnsignedShortFn = bool (*)(void*, std::uint16_t);
using SchemaIntFn = bool (*)(void*, std::int32_t);
using SchemaUnsignedIntFn = bool (*)(void*, std::uint32_t);
using SchemaInt64Fn = bool (*)(void*, std::int64_t);
using SchemaUnsignedInt64Fn = bool (*)(void*, std::uint64_t);
using SchemaFloatFn = bool (*)(void*, float);
using SchemaDoubleFn = bool (*)(void*, double);
using SchemaStringFn = bool (*)(void*, std::string_view);
using SchemaRawFn = bool (*)(void*, std::span<const std::uint8_t>);
using SchemaAdditionalBoolFn = void (*)(void*, bool);
using SchemaAdditionalIntFn = void (*)(void*, std::uint32_t);
using SchemaAdditionalStringFn = void (*)(void*, std::string_view);
using SchemaPushFn = bool (*)(void*, std::string_view);
using SchemaPopFn = void (*)(void*);
using SchemaOpenObjectFn = std::uint8_t (*)(void*);
using SchemaOpenArrayFn = std::uint8_t (*)(void*, bool, std::uint64_t);
using SchemaCloseFn = void (*)(void*);

SchemaNullFn originalSchemaNull{};
SchemaBoolFn originalSchemaBool{};
SchemaSignedByteFn originalSchemaSignedByte{};
SchemaByteFn originalSchemaByte{};
SchemaShortFn originalSchemaShort{};
SchemaUnsignedShortFn originalSchemaUnsignedShort{};
SchemaIntFn originalSchemaInt{};
SchemaUnsignedIntFn originalSchemaUnsignedInt{};
SchemaInt64Fn originalSchemaInt64{};
SchemaUnsignedInt64Fn originalSchemaUnsignedInt64{};
SchemaFloatFn originalSchemaFloat{};
SchemaDoubleFn originalSchemaDouble{};
SchemaStringFn originalSchemaString{};
SchemaRawFn originalSchemaRaw{};
SchemaAdditionalBoolFn originalSchemaAdditionalBool{};
SchemaAdditionalIntFn originalSchemaAdditionalInt{};
SchemaAdditionalStringFn originalSchemaAdditionalString{};
SchemaPushFn originalSchemaPush{};
SchemaPopFn originalSchemaPop{};
SchemaOpenObjectFn originalSchemaOpenObject{};
SchemaOpenArrayFn originalSchemaOpenArray{};
SchemaCloseFn originalSchemaClose{};

std::string boundedName(const char* value) {
    if (value == nullptr)
        return {};
    return std::string(value, strnlen(value, kMaximumFieldNameLength));
}

std::string capturePath(const char* fallback) {
    if (activeCapture == nullptr)
        return boundedName(fallback);
    std::string result;
    for (const auto& component : activeCapture->context) {
        if (!result.empty())
            result += '.';
        if (result.size() + component.size() > kMaximumFieldPathLength)
            break;
        result += component;
    }
    if (result.empty())
        result = boundedName(fallback);
    return result;
}

void markIncomplete(std::string_view reason) {
    if (activeCapture == nullptr || activeCapture->packet == nullptr)
        return;
    auto& packet = *activeCapture->packet;
    packet.complete = false;
    if (packet.limitation.empty())
        packet.limitation = reason;
}

void observeField(const char* fallback, std::string_view type) {
    if (activeCapture == nullptr || activeCapture->packet == nullptr)
        return;
    auto& fields = activeCapture->packet->fields;
    if (fields.size() >= kMaximumFieldsPerPacket) {
        markIncomplete("field_limit_reached");
        return;
    }
    std::string path = capturePath(fallback);
    if (path.empty())
        path = "field_" + std::to_string(fields.size());
    fields.push_back({std::move(path), std::string(type)});
}

class PrimitiveWriteScope {
public:
    PrimitiveWriteScope(const char* field, std::string_view type)
    : outermost_(primitiveWriteDepth++ == 0) {
        if (outermost_)
            observeField(field, type);
    }

    ~PrimitiveWriteScope() { --primitiveWriteDepth; }

    PrimitiveWriteScope(const PrimitiveWriteScope&) = delete;
    PrimitiveWriteScope& operator=(const PrimitiveWriteScope&) = delete;

private:
    bool outermost_{};
};

#define DOBBY_INTEGER_WRITER(name, original, value_type, wire_type)                 \
    void name(void* stream, value_type value, const char* field, const char* notes) { \
        PrimitiveWriteScope scope(field, wire_type);                                  \
        original(stream, value, field, notes);                                        \
    }

DOBBY_INTEGER_WRITER(writeBoolDetour, originalWriteBool, bool, "bool")
DOBBY_INTEGER_WRITER(writeByteDetour, originalWriteByte, std::uint8_t, "u8")
DOBBY_INTEGER_WRITER(writeSignedByteDetour, originalWriteSignedByte, std::int8_t, "i8")
DOBBY_INTEGER_WRITER(writeUnsignedShortDetour, originalWriteUnsignedShort, std::uint16_t, "lu16")
DOBBY_INTEGER_WRITER(writeSignedShortDetour, originalWriteSignedShort, std::int16_t, "li16")
DOBBY_INTEGER_WRITER(writeUnsignedIntDetour, originalWriteUnsignedInt, std::uint32_t, "lu32")
DOBBY_INTEGER_WRITER(writeSignedBigEndianIntDetour, originalWriteSignedBigEndianInt, std::int32_t, "i32")
DOBBY_INTEGER_WRITER(writeSignedIntDetour, originalWriteSignedInt, std::int32_t, "li32")
DOBBY_INTEGER_WRITER(writeUnsignedInt64Detour, originalWriteUnsignedInt64, std::uint64_t, "lu64")
DOBBY_INTEGER_WRITER(writeSignedInt64Detour, originalWriteSignedInt64, std::int64_t, "li64")
DOBBY_INTEGER_WRITER(writeUnsignedVarIntDetour, originalWriteUnsignedVarInt, std::uint32_t, "varint")
DOBBY_INTEGER_WRITER(writeUnsignedVarInt64Detour, originalWriteUnsignedVarInt64, std::uint64_t, "varint64")
DOBBY_INTEGER_WRITER(writeVarIntDetour, originalWriteVarInt, std::int32_t, "zigzag32")
DOBBY_INTEGER_WRITER(writeVarInt64Detour, originalWriteVarInt64, std::int64_t, "zigzag64")

#undef DOBBY_INTEGER_WRITER

void writeDoubleDetour(void* stream, double value, const char* field, const char* notes) {
    PrimitiveWriteScope scope(field, "lf64");
    originalWriteDouble(stream, value, field, notes);
}

void writeFloatDetour(void* stream, float value, const char* field, const char* notes) {
    PrimitiveWriteScope scope(field, "lf32");
    originalWriteFloat(stream, value, field, notes);
}

void writeFixedFloatDetour(
        void* stream, float value, const char* field, const char* notes, double size) {
    PrimitiveWriteScope scope(field, "lf32");
    originalWriteFixedFloat(stream, value, field, notes, size);
}

void writeNormalizedFloatDetour(
        void* stream, float value, const char* field, const char* notes) {
    PrimitiveWriteScope scope(field, "lf32");
    originalWriteNormalizedFloat(stream, value, field, notes);
}

void writeStringDetour(
        void* stream, std::string_view value, const char* field, const char* notes) {
    PrimitiveWriteScope scope(field, "string");
    originalWriteString(stream, value, field, notes);
}

void writeIfDetour(
        void* stream, bool control, const char* field, const void* trueWriter,
        const void* falseWriter) {
    markIncomplete("default_value_conditional_branch");
    originalWriteIf(stream, control, field, trueWriter, falseWriter);
}

void writeConditionalDetour(
        void* stream, const char* conditions, const void* blocks,
        const void* defaultWriter) {
    markIncomplete("default_value_conditional_branch");
    originalWriteConditional(stream, conditions, blocks, defaultWriter);
}

void branchRangeDetour(
        void* stream, void* writer, std::int32_t control, std::int32_t minimum,
        std::int32_t maximum, const char* field) {
    markIncomplete("default_value_branch");
    originalBranchRange(stream, writer, control, minimum, maximum, field);
}

void branchVectorDetour(
        void* stream, void* writer, std::int32_t control, const void* choices,
        const char* field) {
    markIncomplete("default_value_branch");
    originalBranchVector(stream, writer, control, choices, field);
}

bool typeBeginDetour(
        void* stream, const char* type, const char* field, const char* notes) {
    return originalTypeBegin(stream, type, field, notes);
}

void typeEndDetour(void* stream) { originalTypeEnd(stream); }

void enumDetour(void* stream, const char* type, const char* value) {
    originalEnum(stream, type, value);
}

void arrayDetour(
        void* stream, void* sizeWriter, void* elementWriter, const char* field,
        const char* notes) {
    markIncomplete("default_collection_does_not_prove_element_schema");
    originalArray(stream, sizeWriter, elementWriter, field, notes);
}

bool schemaNullDetour(void* writer) { return originalSchemaNull(writer); }
bool schemaBoolDetour(void* writer, bool value) { return originalSchemaBool(writer, value); }
bool schemaSignedByteDetour(void* writer, std::int8_t value) { return originalSchemaSignedByte(writer, value); }
bool schemaByteDetour(void* writer, std::uint8_t value) { return originalSchemaByte(writer, value); }
bool schemaShortDetour(void* writer, std::int16_t value) { return originalSchemaShort(writer, value); }
bool schemaUnsignedShortDetour(void* writer, std::uint16_t value) { return originalSchemaUnsignedShort(writer, value); }
bool schemaIntDetour(void* writer, std::int32_t value) { return originalSchemaInt(writer, value); }
bool schemaUnsignedIntDetour(void* writer, std::uint32_t value) { return originalSchemaUnsignedInt(writer, value); }
bool schemaInt64Detour(void* writer, std::int64_t value) { return originalSchemaInt64(writer, value); }
bool schemaUnsignedInt64Detour(void* writer, std::uint64_t value) { return originalSchemaUnsignedInt64(writer, value); }
bool schemaFloatDetour(void* writer, float value) { return originalSchemaFloat(writer, value); }
bool schemaDoubleDetour(void* writer, double value) { return originalSchemaDouble(writer, value); }
bool schemaStringDetour(void* writer, std::string_view value) { return originalSchemaString(writer, value); }

bool schemaRawDetour(void* writer, std::span<const std::uint8_t> value) {
    observeField(nullptr, "restBuffer");
    markIncomplete("raw_span_length_not_expressed_by_runtime_schema");
    return originalSchemaRaw(writer, value);
}

void schemaAdditionalBoolDetour(void* writer, bool value) { originalSchemaAdditionalBool(writer, value); }
void schemaAdditionalIntDetour(void* writer, std::uint32_t value) { originalSchemaAdditionalInt(writer, value); }
void schemaAdditionalStringDetour(void* writer, std::string_view value) { originalSchemaAdditionalString(writer, value); }

bool schemaPushDetour(void* writer, std::string_view name) {
    if (activeCapture != nullptr) {
        activeCapture->context.emplace_back(
                name.substr(0, std::min(name.size(), kMaximumFieldNameLength)));
    }
    return originalSchemaPush(writer, name);
}

void schemaPopDetour(void* writer) {
    originalSchemaPop(writer);
    if (activeCapture != nullptr && !activeCapture->context.empty())
        activeCapture->context.pop_back();
}

std::uint8_t schemaOpenObjectDetour(void* writer) {
    return originalSchemaOpenObject(writer);
}

std::uint8_t schemaOpenArrayDetour(
        void* writer, bool dynamicExtent, std::uint64_t length) {
    static_cast<void>(dynamicExtent);
    static_cast<void>(length);
    markIncomplete("default_collection_does_not_prove_element_schema");
    return originalSchemaOpenArray(writer, dynamicExtent, length);
}

void schemaCloseDetour(void* writer) { originalSchemaClose(writer); }

const std::array<void*, 27> kBinaryReplacements{
        reinterpret_cast<void*>(writeBoolDetour),
        reinterpret_cast<void*>(writeByteDetour),
        reinterpret_cast<void*>(writeSignedByteDetour),
        reinterpret_cast<void*>(writeUnsignedShortDetour),
        reinterpret_cast<void*>(writeSignedShortDetour),
        reinterpret_cast<void*>(writeUnsignedIntDetour),
        reinterpret_cast<void*>(writeSignedBigEndianIntDetour),
        reinterpret_cast<void*>(writeSignedIntDetour),
        reinterpret_cast<void*>(writeUnsignedInt64Detour),
        reinterpret_cast<void*>(writeSignedInt64Detour),
        reinterpret_cast<void*>(writeUnsignedVarIntDetour),
        reinterpret_cast<void*>(writeUnsignedVarInt64Detour),
        reinterpret_cast<void*>(writeVarIntDetour),
        reinterpret_cast<void*>(writeVarInt64Detour),
        reinterpret_cast<void*>(writeDoubleDetour),
        reinterpret_cast<void*>(writeFloatDetour),
        reinterpret_cast<void*>(writeFixedFloatDetour),
        reinterpret_cast<void*>(writeNormalizedFloatDetour),
        reinterpret_cast<void*>(writeStringDetour),
        reinterpret_cast<void*>(writeIfDetour),
        reinterpret_cast<void*>(writeConditionalDetour),
        reinterpret_cast<void*>(branchRangeDetour),
        reinterpret_cast<void*>(branchVectorDetour),
        reinterpret_cast<void*>(typeBeginDetour),
        reinterpret_cast<void*>(typeEndDetour),
        reinterpret_cast<void*>(enumDetour),
        reinterpret_cast<void*>(arrayDetour),
};

const std::array<void*, 22> kSchemaReplacements{
        reinterpret_cast<void*>(schemaNullDetour),
        reinterpret_cast<void*>(schemaBoolDetour),
        reinterpret_cast<void*>(schemaSignedByteDetour),
        reinterpret_cast<void*>(schemaByteDetour),
        reinterpret_cast<void*>(schemaShortDetour),
        reinterpret_cast<void*>(schemaUnsignedShortDetour),
        reinterpret_cast<void*>(schemaIntDetour),
        reinterpret_cast<void*>(schemaUnsignedIntDetour),
        reinterpret_cast<void*>(schemaInt64Detour),
        reinterpret_cast<void*>(schemaUnsignedInt64Detour),
        reinterpret_cast<void*>(schemaFloatDetour),
        reinterpret_cast<void*>(schemaDoubleDetour),
        reinterpret_cast<void*>(schemaStringDetour),
        reinterpret_cast<void*>(schemaRawDetour),
        reinterpret_cast<void*>(schemaAdditionalBoolDetour),
        reinterpret_cast<void*>(schemaAdditionalIntDetour),
        reinterpret_cast<void*>(schemaAdditionalStringDetour),
        reinterpret_cast<void*>(schemaPushDetour),
        reinterpret_cast<void*>(schemaPopDetour),
        reinterpret_cast<void*>(schemaOpenObjectDetour),
        reinterpret_cast<void*>(schemaOpenArrayDetour),
        reinterpret_cast<void*>(schemaCloseDetour),
};

void assignOriginalBinary(std::size_t index, void* value) {
    switch (index) {
    case 0: originalWriteBool = reinterpret_cast<BoolWriterFn>(value); break;
    case 1: originalWriteByte = reinterpret_cast<ByteWriterFn>(value); break;
    case 2: originalWriteSignedByte = reinterpret_cast<SignedByteWriterFn>(value); break;
    case 3: originalWriteUnsignedShort = reinterpret_cast<UnsignedShortWriterFn>(value); break;
    case 4: originalWriteSignedShort = reinterpret_cast<SignedShortWriterFn>(value); break;
    case 5: originalWriteUnsignedInt = reinterpret_cast<UnsignedIntWriterFn>(value); break;
    case 6: originalWriteSignedBigEndianInt = reinterpret_cast<SignedIntWriterFn>(value); break;
    case 7: originalWriteSignedInt = reinterpret_cast<SignedIntWriterFn>(value); break;
    case 8: originalWriteUnsignedInt64 = reinterpret_cast<UnsignedInt64WriterFn>(value); break;
    case 9: originalWriteSignedInt64 = reinterpret_cast<SignedInt64WriterFn>(value); break;
    case 10: originalWriteUnsignedVarInt = reinterpret_cast<UnsignedIntWriterFn>(value); break;
    case 11: originalWriteUnsignedVarInt64 = reinterpret_cast<UnsignedInt64WriterFn>(value); break;
    case 12: originalWriteVarInt = reinterpret_cast<SignedIntWriterFn>(value); break;
    case 13: originalWriteVarInt64 = reinterpret_cast<SignedInt64WriterFn>(value); break;
    case 14: originalWriteDouble = reinterpret_cast<DoubleWriterFn>(value); break;
    case 15: originalWriteFloat = reinterpret_cast<FloatWriterFn>(value); break;
    case 16: originalWriteFixedFloat = reinterpret_cast<FixedFloatWriterFn>(value); break;
    case 17: originalWriteNormalizedFloat = reinterpret_cast<FloatWriterFn>(value); break;
    case 18: originalWriteString = reinterpret_cast<StringWriterFn>(value); break;
    case 19: originalWriteIf = reinterpret_cast<WriteIfFn>(value); break;
    case 20: originalWriteConditional = reinterpret_cast<WriteConditionalFn>(value); break;
    case 21: originalBranchRange = reinterpret_cast<BranchRangeFn>(value); break;
    case 22: originalBranchVector = reinterpret_cast<BranchVectorFn>(value); break;
    case 23: originalTypeBegin = reinterpret_cast<TypeBeginFn>(value); break;
    case 24: originalTypeEnd = reinterpret_cast<TypeEndFn>(value); break;
    case 25: originalEnum = reinterpret_cast<EnumFn>(value); break;
    case 26: originalArray = reinterpret_cast<ArrayFn>(value); break;
    default: break;
    }
}

void assignOriginalSchema(std::size_t index, void* value) {
    switch (index) {
    case 0: originalSchemaNull = reinterpret_cast<SchemaNullFn>(value); break;
    case 1: originalSchemaBool = reinterpret_cast<SchemaBoolFn>(value); break;
    case 2: originalSchemaSignedByte = reinterpret_cast<SchemaSignedByteFn>(value); break;
    case 3: originalSchemaByte = reinterpret_cast<SchemaByteFn>(value); break;
    case 4: originalSchemaShort = reinterpret_cast<SchemaShortFn>(value); break;
    case 5: originalSchemaUnsignedShort = reinterpret_cast<SchemaUnsignedShortFn>(value); break;
    case 6: originalSchemaInt = reinterpret_cast<SchemaIntFn>(value); break;
    case 7: originalSchemaUnsignedInt = reinterpret_cast<SchemaUnsignedIntFn>(value); break;
    case 8: originalSchemaInt64 = reinterpret_cast<SchemaInt64Fn>(value); break;
    case 9: originalSchemaUnsignedInt64 = reinterpret_cast<SchemaUnsignedInt64Fn>(value); break;
    case 10: originalSchemaFloat = reinterpret_cast<SchemaFloatFn>(value); break;
    case 11: originalSchemaDouble = reinterpret_cast<SchemaDoubleFn>(value); break;
    case 12: originalSchemaString = reinterpret_cast<SchemaStringFn>(value); break;
    case 13: originalSchemaRaw = reinterpret_cast<SchemaRawFn>(value); break;
    case 14: originalSchemaAdditionalBool = reinterpret_cast<SchemaAdditionalBoolFn>(value); break;
    case 15: originalSchemaAdditionalInt = reinterpret_cast<SchemaAdditionalIntFn>(value); break;
    case 16: originalSchemaAdditionalString = reinterpret_cast<SchemaAdditionalStringFn>(value); break;
    case 17: originalSchemaPush = reinterpret_cast<SchemaPushFn>(value); break;
    case 18: originalSchemaPop = reinterpret_cast<SchemaPopFn>(value); break;
    case 19: originalSchemaOpenObject = reinterpret_cast<SchemaOpenObjectFn>(value); break;
    case 20: originalSchemaOpenArray = reinterpret_cast<SchemaOpenArrayFn>(value); break;
    case 21: originalSchemaClose = reinterpret_cast<SchemaCloseFn>(value); break;
    default: break;
    }
}

bool validateTargets(
        const MinecraftImage& image, std::uintptr_t vtableOffset,
        std::span<const target::ProtocolVirtualTarget> targets) {
    const auto vtable = image.base + vtableOffset;
    for (const auto& target : targets) {
        const auto function = image.base + target.functionOffset;
        const auto slotAddress = vtable + target.slot * sizeof(void*);
        if (!addressIsExecutable(image, function) ||
            !addressIsInImage(image, slotAddress) ||
            !matchesSignature(reinterpret_cast<const void*>(function), target.signature)) {
            return false;
        }
        void* current{};
        std::memcpy(&current, reinterpret_cast<const void*>(slotAddress), sizeof(current));
        if (current != reinterpret_cast<void*>(function))
            return false;
    }
    return true;
}

bool installTargets(
        const MinecraftImage& image, std::uintptr_t vtableOffset,
        std::span<const target::ProtocolVirtualTarget> targets,
        std::span<void* const> replacements, std::vector<PatchRecord>& installed,
        void (*assignOriginal)(std::size_t, void*)) {
    for (std::size_t index = 0; index < targets.size(); ++index) {
        auto* slot = reinterpret_cast<void**>(
                image.base + vtableOffset + targets[index].slot * sizeof(void*));
        void* original = *slot;
        assignOriginal(index, original);
        void* replacement = replacements[index];
        if (mcpelauncher_patch(slot, &replacement, sizeof(replacement)) == nullptr ||
            *slot != replacement) {
            return false;
        }
        installed.push_back({slot, original});
    }
    return true;
}

bool restoreTargets(std::vector<PatchRecord>& installed) {
    bool restored = true;
    for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
        void* original = it->original;
        if (mcpelauncher_patch(it->slot, &original, sizeof(original)) == nullptr ||
            *it->slot != original) {
            restored = false;
        }
    }
    installed.clear();
    return restored;
}

bool packetVirtual(
        const MinecraftImage& image, const void* packet, std::size_t slot,
        void*& result) {
    if (packet == nullptr)
        return false;
    void** vtable{};
    std::memcpy(&vtable, packet, sizeof(vtable));
    if (!addressIsInImage(image, reinterpret_cast<std::uintptr_t>(vtable)) ||
        !addressIsInImage(
                image, reinterpret_cast<std::uintptr_t>(vtable + slot))) {
        return false;
    }
    result = vtable[slot];
    return addressIsExecutable(image, reinterpret_cast<std::uintptr_t>(result));
}

ProtocolDumpObservation sweepPackets(const MinecraftImage& image) {
    using PacketFactoryFn = std::shared_ptr<void> (*)(std::int32_t);
    using GetIdFn = std::int32_t (*)(const void*);
    using GetNameFn = std::string_view (*)(const void*);
    using GetSerializationModeFn = std::uint8_t (*)(const void*);
    using IsValidFn = bool (*)(const void*);
    using WriteFn = void (*)(const void*, void*);
    using StreamConstructorFn = void* (*)(void*);
    using StreamDestructorFn = void (*)(void*);

    ProtocolDumpObservation dump{
            kMinecraftVersion, kMinecraftBuildId, kMinecraftProtocolVersion,
            target::kPacketFactoryHighestId, {}};
    const auto factory = reinterpret_cast<PacketFactoryFn>(
            image.base + target::kPacketFactoryOffset);
    const auto constructStream = reinterpret_cast<StreamConstructorFn>(
            image.base + target::kBinaryStreamConstructorOffset);

    for (std::int32_t requestedId = 0;
         requestedId <= target::kPacketFactoryHighestId; ++requestedId) {
        std::shared_ptr<void> packet = factory(requestedId);
        if (!packet)
            continue;

        void* getIdAddress{};
        void* getNameAddress{};
        void* modeAddress{};
        void* isValidAddress{};
        void* writeAddress{};
        if (!packetVirtual(image, packet.get(), target::kPacketGetIdVtableSlot, getIdAddress) ||
            !packetVirtual(image, packet.get(), target::kPacketGetNameVtableSlot, getNameAddress) ||
            !packetVirtual(image, packet.get(), target::kPacketIsValidVtableSlot, isValidAddress) ||
            !packetVirtual(image, packet.get(), target::kPacketSerializationModeVtableSlot, modeAddress) ||
            !packetVirtual(image, packet.get(), target::kPacketWriteVtableSlot, writeAddress)) {
            continue;
        }

        ProtocolPacketObservation observation;
        observation.id = reinterpret_cast<GetIdFn>(getIdAddress)(packet.get());
        const auto runtimeName = reinterpret_cast<GetNameFn>(getNameAddress)(packet.get());
        observation.runtimeName.assign(runtimeName.substr(
                0, std::min(runtimeName.size(), kMaximumFieldNameLength)));
        observation.serializationMode = static_cast<std::int32_t>(
                reinterpret_cast<GetSerializationModeFn>(modeAddress)(packet.get()));
        observation.complete = true;

        if (std::ranges::find(kDefaultStateUnsafePacketIds, observation.id) !=
            kDefaultStateUnsafePacketIds.end()) {
            observation.complete = false;
            observation.limitation = "required_pointer_state_missing";
            dump.packets.push_back(std::move(observation));
            continue;
        }

        if (!reinterpret_cast<IsValidFn>(isValidAddress)(packet.get())) {
            observation.complete = false;
            observation.limitation = "default_packet_is_invalid";
            dump.packets.push_back(std::move(observation));
            continue;
        }

        alignas(std::max_align_t)
                std::array<std::byte, target::kBinaryStreamObjectSize> streamStorage{};
        void* stream = constructStream(streamStorage.data());
        void* streamVtable{};
        std::memcpy(&streamVtable, stream, sizeof(streamVtable));
        if (streamVtable != reinterpret_cast<void*>(
                                    image.base + target::kBinaryStreamVtableOffset)) {
            observation.complete = false;
            observation.limitation = "binary_stream_vtable_mismatch";
        } else {
            ActiveCapture capture{&observation, {}};
            activeCapture = &capture;
            primitiveWriteDepth = 0;
            reinterpret_cast<WriteFn>(writeAddress)(packet.get(), stream);
            activeCapture = nullptr;
            primitiveWriteDepth = 0;
            observation.serialized = true;
            if (observation.complete)
                observation.limitation.clear();
        }

        void* destructorAddress{};
        std::memcpy(&destructorAddress, streamVtable, sizeof(destructorAddress));
        if (addressIsExecutable(image, reinterpret_cast<std::uintptr_t>(destructorAddress)))
            reinterpret_cast<StreamDestructorFn>(destructorAddress)(stream);
        dump.packets.push_back(std::move(observation));
    }
    return dump;
}

bool persistDump(const ProtocolDumpObservation& dump) {
    const std::string observedProtocol = buildProtocolJson(dump);
    const auto protocol = embeddedReference(
            dobby_protocol_reference_start, dobby_protocol_reference_end);
    const auto version = embeddedReference(
            dobby_protocol_version_reference_start,
            dobby_protocol_version_reference_end);
    const bool referenceVerified =
            protocol.size() == protocol_reference::kProtocolSize &&
            version.size() == protocol_reference::kVersionSize &&
            fnv1a64(protocol) == protocol_reference::kProtocolFnv1a64 &&
            fnv1a64(version) == protocol_reference::kVersionFnv1a64;
    const std::string status = buildProtocolStatusJson(dump, referenceVerified);
    if (!referenceVerified)
        return false;
    return writeFileAtomically(observedProtocolPath(), observedProtocol) &&
            writeFileAtomically(protocolReferencePath(), protocol) &&
            writeFileAtomically(protocolVersionReferencePath(), version) &&
            writeFileAtomically(protocolPath(), protocol) &&
            writeFileAtomically(protocolVersionPath(), version) &&
            writeFileAtomically(protocolStatusPath(), status);
}

} // namespace

void dumpProtocolOnStartup() {
    if (dumpStarted.exchange(true, std::memory_order_acq_rel))
        return;
    const auto image = findMinecraftImage();
    if (image.base == 0 || mcpelauncher_patch == nullptr) {
        logLine("ERROR: protocol startup dump unavailable; image or patch API missing");
        recordLifecycleEvent("protocol_dump_error", "image or patch API missing");
        return;
    }

    const auto factoryAddress = image.base + target::kPacketFactoryOffset;
    const auto constructorAddress = image.base + target::kBinaryStreamConstructorOffset;
    const bool valid =
            addressIsExecutable(image, factoryAddress) &&
            addressIsExecutable(image, constructorAddress) &&
            matchesSignature(reinterpret_cast<const void*>(factoryAddress),
                             target::kPacketFactorySignature) &&
            matchesSignature(reinterpret_cast<const void*>(constructorAddress),
                             target::kBinaryStreamConstructorSignature) &&
            validateTargets(image, target::kBinaryStreamVtableOffset,
                            target::kBinaryStreamProtocolTargets) &&
            validateTargets(image, target::kPacketSchemaWriterVtableOffset,
                            target::kPacketSchemaWriterTargets);
    if (!valid) {
        logLine("ERROR: protocol startup dump target mismatch; no dump produced");
        recordLifecycleEvent("protocol_dump_error", "target validation failed");
        return;
    }

    std::vector<PatchRecord> installed;
    installed.reserve(kBinaryReplacements.size() + kSchemaReplacements.size());
    const bool patched =
            installTargets(image, target::kBinaryStreamVtableOffset,
                           target::kBinaryStreamProtocolTargets,
                           kBinaryReplacements, installed, assignOriginalBinary) &&
            installTargets(image, target::kPacketSchemaWriterVtableOffset,
                           target::kPacketSchemaWriterTargets,
                           kSchemaReplacements, installed, assignOriginalSchema);
    if (!patched) {
        const bool restored = restoreTargets(installed);
        logLine(std::string("ERROR: protocol startup dump probe install failed; restore ") +
                (restored ? "succeeded" : "failed"));
        recordLifecycleEvent("protocol_dump_error", "temporary probe install failed");
        return;
    }

    ProtocolDumpObservation dump = sweepPackets(image);
    activeCapture = nullptr;
    const bool restored = restoreTargets(installed);
    if (!restored) {
        logLine("ERROR: protocol startup dump probe restore failed; output suppressed");
        recordLifecycleEvent("protocol_dump_error", "temporary probe restore failed");
        return;
    }

    if (!persistDump(dump)) {
        logLine("ERROR: protocol startup dump could not write all output files");
        recordLifecycleEvent("protocol_dump_error", "atomic output write failed");
        return;
    }
    const auto serialized = std::count_if(
            dump.packets.begin(), dump.packets.end(),
            [](const auto& packet) { return packet.serialized; });
    const std::string detail =
            "factory=" + std::to_string(dump.packets.size()) +
            " serialized=" + std::to_string(serialized) +
            " protocol=" + protocolPath() + " status=" + protocolStatusPath();
    logLine("protocol startup dump complete: " + detail);
    recordLifecycleEvent("protocol_dump_complete", detail);
}

} // namespace dobby

#else

namespace dobby {
void dumpProtocolOnStartup() {}
} // namespace dobby

#endif
