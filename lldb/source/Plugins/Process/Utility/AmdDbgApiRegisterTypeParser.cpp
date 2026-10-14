//===-- AmdDbgApiRegisterTypeParser.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AmdDbgApiRegisterTypeParser.h"

#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FormatVariadic.h"
#include <algorithm>
#include <optional>
#include <utility>

using namespace lldb_private;

static llvm::StringRef ConsumeToken(llvm::StringRef &type) {
  type = type.ltrim();
  llvm::StringRef token = type.take_while(
      [](char c) { return llvm::isAlnum(c) || c == '_' || c == '.'; });
  type = type.drop_front(token.size());
  return token;
}

static bool ConsumeChar(llvm::StringRef &type, char c) {
  type = type.ltrim();
  return type.consume_front(llvm::StringRef(&c, 1));
}

namespace {
struct ScalarType {
  unsigned byte_size;
  lldb::Encoding encoding;
  lldb::Format format;
  // Format for an array of this type.
  lldb::Format vector_format;
};
} // namespace

// amd-dbgapi types SGPRs and VGPR lanes as int32_t, but they often hold
// addresses and masks, so integers show as unsigned hex. Unsigned also lets
// "register write" take any bit pattern.
static std::optional<ScalarType> LookupScalar(llvm::StringRef name) {
  return llvm::StringSwitch<std::optional<ScalarType>>(name)
      .Cases({"int32_t", "uint32_t"},
             ScalarType{4, lldb::eEncodingUint, lldb::eFormatHex,
                        lldb::eFormatVectorOfUInt32})
      .Cases({"int64_t", "uint64_t"},
             ScalarType{8, lldb::eEncodingUint, lldb::eFormatHex,
                        lldb::eFormatVectorOfUInt64})
      .Case("float", ScalarType{4, lldb::eEncodingIEEE754, lldb::eFormatFloat,
                                lldb::eFormatVectorOfFloat32})
      .Case("double", ScalarType{8, lldb::eEncodingIEEE754, lldb::eFormatFloat,
                                 lldb::eFormatVectorOfFloat64})
      .Default(std::nullopt);
}

// Parses the part after "enum": either a definition such as
// "fp_round { NEAREST_EVEN = 0, PLUS_INF = 1 }", or a reference to an enum
// defined earlier, such as "fp_round". As in ROCgdb, the ',' after an
// enumerator is optional.
llvm::Expected<const FieldEnum *>
AmdDbgApiRegisterTypeParser::ParseEnum(llvm::StringRef &type) {
  llvm::StringRef name = ConsumeToken(type);
  if (name.empty())
    return llvm::createStringError(
        llvm::formatv("expected an enum name before \"{0}\"", type));

  auto existing = m_enums.find(name);
  if (!ConsumeChar(type, '{')) {
    if (existing == m_enums.end())
      return llvm::createStringError(
          llvm::formatv("reference to unknown enum \"{0}\"", name));
    return existing->second.get();
  }
  if (existing != m_enums.end())
    return llvm::createStringError(
        llvm::formatv("redefinition of enum \"{0}\"", name));

  FieldEnum::Enumerators enumerators;
  while (!ConsumeChar(type, '}')) {
    llvm::StringRef enumerator = ConsumeToken(type);
    uint64_t value;
    if (enumerator.empty() || !ConsumeChar(type, '=') ||
        !llvm::to_integer(ConsumeToken(type), value, 10))
      return llvm::createStringError(
          llvm::formatv("malformed enumerator at \"{0}\"", type));
    enumerators.emplace_back(value, enumerator.str());
    ConsumeChar(type, ',');
  }

  auto inserted = m_enums.try_emplace(
      name, std::make_unique<FieldEnum>(name.str(), enumerators));
  return inserted.first->second.get();
}

// Parses one field, including its ';', such as "bool EXCP.DIV0 @2;",
// "uint32_t EXCP_CYCLE @16-21;" or "enum dp_rate { ... } DP_RATE @29-31;".
llvm::Expected<RegisterFlags::Field>
AmdDbgApiRegisterTypeParser::ParseField(llvm::StringRef &type,
                                        unsigned register_bits) {
  llvm::StringRef field_type = ConsumeToken(type);
  bool is_bool = field_type == "bool";
  const FieldEnum *enum_type = nullptr;
  if (field_type == "enum") {
    llvm::Expected<const FieldEnum *> parsed = ParseEnum(type);
    if (!parsed)
      return parsed.takeError();
    enum_type = *parsed;
  } else if (!is_bool &&
             field_type != (register_bits == 32 ? "uint32_t" : "uint64_t")) {
    // As in ROCgdb, an integer field has the size of its flags type.
    return llvm::createStringError(
        llvm::formatv("unsupported field type \"{0}\"", field_type));
  }

  // A bool is a single bit, "@start"; other fields are "@start-end".
  llvm::StringRef name = ConsumeToken(type);
  uint64_t start = 0;
  uint64_t end = 0;
  if (name.empty() || !ConsumeChar(type, '@') ||
      !llvm::to_integer(ConsumeToken(type), start, 10) ||
      (!is_bool && (!ConsumeChar(type, '-') ||
                    !llvm::to_integer(ConsumeToken(type), end, 10))) ||
      !ConsumeChar(type, ';'))
    return llvm::createStringError(
        llvm::formatv("malformed field at \"{0}\"", type));
  if (is_bool)
    end = start;

  if (start > end || end >= register_bits)
    return llvm::createStringError(llvm::formatv(
        "field \"{0}\" has invalid bit range {1}-{2}", name, start, end));
  if (enum_type) {
    // Like ProcessGDBRemote's XML parser, keep the field but drop an enum that
    // does not fit it.
    uint64_t max_value = RegisterFlags::Field::GetMaxValue(start, end);
    for (const FieldEnum::Enumerator &enumerator :
         enum_type->GetEnumerators()) {
      if (enumerator.m_value > max_value) {
        LLDB_LOG(GetLog(LLDBLog::Process),
                 "Ignoring enum {0} of field {1}: enumerator {2} does not fit",
                 enum_type->GetID(), name, enumerator.m_name);
        enum_type = nullptr;
        break;
      }
    }
  }
  return RegisterFlags::Field(name.str(), start, end, enum_type);
}

// Parses the part of a flags type after "flags32_t" or "flags64_t" into a
// RegisterFlags object.
llvm::Expected<const RegisterFlags *> AmdDbgApiRegisterTypeParser::ParseFlags(
    llvm::StringRef rest, unsigned flags_size, unsigned byte_size) {
  if (flags_size != byte_size)
    return llvm::createStringError(
        llvm::formatv("flags type is {0} bytes but the register is {1} bytes",
                      flags_size, byte_size));

  llvm::StringRef name = ConsumeToken(rest);
  if (name.empty())
    return llvm::createStringError(
        llvm::formatv("expected a flags type name before \"{0}\"", rest));

  auto existing = m_flags.find(name);
  // No body: a reference such as "flags32_t mode".
  if (!ConsumeChar(rest, '{')) {
    if (!rest.trim().empty())
      return llvm::createStringError(llvm::formatv("unexpected \"{0}\"", rest));
    if (existing == m_flags.end())
      return llvm::createStringError(
          llvm::formatv("reference to unknown flags type \"{0}\"", name));
    if (existing->second->GetSize() != byte_size)
      return llvm::createStringError(
          llvm::formatv("flags type \"{0}\" has a different size", name));
    return existing->second.get();
  }
  if (existing != m_flags.end())
    return llvm::createStringError(
        llvm::formatv("redefinition of flags type \"{0}\"", name));

  // A definition: parse fields until the closing '}'.
  std::vector<RegisterFlags::Field> fields;
  while (!ConsumeChar(rest, '}')) {
    if (rest.trim().empty())
      return llvm::createStringError("missing '}'");
    llvm::Expected<RegisterFlags::Field> field =
        ParseField(rest, flags_size * 8);
    if (!field)
      return field.takeError();
    fields.push_back(std::move(*field));
  }
  if (!rest.trim().empty())
    return llvm::createStringError(llvm::formatv("unexpected \"{0}\"", rest));
  if (fields.empty())
    return llvm::createStringError(
        llvm::formatv("flags type \"{0}\" has no fields", name));

  // RegisterFlags wants fields from the most significant bit down, and
  // asserts that they do not overlap.
  std::sort(fields.begin(), fields.end(),
            [](const auto &lhs, const auto &rhs) { return rhs < lhs; });
  for (size_t i = 1; i < fields.size(); ++i)
    if (fields[i].Overlaps(fields[i - 1]))
      return llvm::createStringError(
          llvm::formatv("fields \"{0}\" and \"{1}\" overlap",
                        fields[i].GetName(), fields[i - 1].GetName()));

  auto inserted = m_flags.try_emplace(
      name, std::make_unique<RegisterFlags>(name.str(), flags_size, fields));
  return inserted.first->second.get();
}

// See the documentation of AMD_DBGAPI_REGISTER_INFO_TYPE in amd-dbgapi.h for
// the format:
// https://github.com/ROCm/rocm-systems/blob/529f578/projects/rocdbgapi/include/amd-dbgapi.h.in#L5396
// amd-dbgapi also returns int32_t, which that grammar omits, and "void (*)()"
// where it says "void(void)". ROCgdb parses the same strings in
// parse_amd_dbgapi_register_type:
// https://github.com/ROCm/ROCgdb/blob/4d8ffef/gdb/amdgpu-tdep.c#L1007
llvm::Expected<AmdDbgApiRegisterType>
AmdDbgApiRegisterTypeParser::Parse(llvm::StringRef type, unsigned byte_size) {
  llvm::StringRef rest = type;
  llvm::StringRef keyword = ConsumeToken(rest);
  if (keyword == "flags32_t" || keyword == "flags64_t") {
    llvm::Expected<const RegisterFlags *> flags =
        ParseFlags(rest, keyword == "flags32_t" ? 4 : 8, byte_size);
    if (!flags)
      return flags.takeError();
    return AmdDbgApiRegisterType{lldb::eEncodingUint, lldb::eFormatHex, *flags};
  }

  llvm::StringRef trimmed = type.trim();
  if (trimmed == "void (*)()" || trimmed == "void(void)")
    return AmdDbgApiRegisterType{lldb::eEncodingUint, lldb::eFormatHex};

  // An array such as "int32_t[64]", one element per lane.
  if (trimmed.consume_back("]")) {
    auto [element, count_str] = trimmed.rsplit('[');
    std::optional<ScalarType> scalar = LookupScalar(element.trim());
    uint64_t count;
    if (!scalar || !llvm::to_integer(count_str.trim(), count, 10))
      return llvm::createStringError(
          llvm::formatv("unsupported type \"{0}\"", type));
    if (scalar->byte_size * count != byte_size)
      return llvm::createStringError(
          llvm::formatv("\"{0}\" does not match the register size of {1} bytes",
                        type, byte_size));
    return AmdDbgApiRegisterType{lldb::eEncodingVector, scalar->vector_format};
  }

  std::optional<ScalarType> scalar = LookupScalar(trimmed);
  if (!scalar)
    return llvm::createStringError(
        llvm::formatv("unsupported type \"{0}\"", type));
  if (scalar->byte_size != byte_size)
    return llvm::createStringError(
        llvm::formatv("\"{0}\" does not match the register size of {1} bytes",
                      type, byte_size));
  return AmdDbgApiRegisterType{scalar->encoding, scalar->format};
}
