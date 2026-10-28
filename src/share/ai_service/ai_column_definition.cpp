/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SHARE
#include "share/ai_service/ai_column_definition.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/allocator/page_arena.h"
#include "share/json/ob_json_helper.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_lob_access_utils.h"
#include <algorithm>
#include <new>

namespace oceanbase { namespace share {
using namespace common;
namespace {
constexpr size_t MAX_DEFINITION_BYTES = 1024 * 1024;
constexpr size_t MAX_DEPENDENCIES = 1024;
std::string json_text(const std::string &value)
{
  static const char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += char(c);
  }
  out += '"';
  return out;
}
bool integer(ObJsonObject &object, const char *key, int64_t &value)
{
  ObJsonNode *node = object.get_value(key);
  if (!node) return false;
  if (node->json_type() == ObJsonNodeType::J_INT) value = node->get_int();
  else if (node->json_type() == ObJsonNodeType::J_UINT && node->get_uint() <= INT64_MAX) value = node->get_uint();
  else return false;
  return true;
}
bool identifier(ObJsonObject &object, const char *key, uint64_t &value)
{
  ObJsonNode *node = object.get_value(key);
  if (!node) return false;
  if (node->json_type() == ObJsonNodeType::J_UINT) value = node->get_uint();
  else if (node->json_type() == ObJsonNodeType::J_INT && node->get_int() > 0) value = node->get_int();
  else return false;
  return value != 0 && value != UINT64_MAX;
}
bool text(ObJsonObject &object, const char *key, std::string &value)
{
  ObJsonNode *node = object.get_value(key);
  if (!node || node->json_type() != ObJsonNodeType::J_STRING) return false;
  const ObString s = static_cast<ObJsonString *>(node)->get_str();
  value.assign(s.ptr(), s.length());
  return true;
}
bool valid(const AiColumnDefinition &d)
{
  const uint64_t ids[] = {d.column_id, d.state_column_id, d.request_column_id, d.result_spec_column_id};
  for (size_t i = 0; i < 4; ++i) {
    if (!ids[i] || ids[i] == UINT64_MAX) return false;
    for (size_t j = 0; j < i; ++j) if (ids[i] == ids[j]) return false;
  }
  if (d.column_name.empty() || d.expression.empty() || d.normalized_expr.empty() ||
      d.model_name.empty() || d.input_expr.empty() || d.config_expr.empty() ||
      d.max_retries < 0 || d.max_retries > 100 || d.dimension < 0 || d.dimension > 16000 || d.dependencies.size() > MAX_DEPENDENCIES ||
      (d.function != AiColumnDefinition::COMPLETE && d.function != AiColumnDefinition::EMBED) ||
      (d.function == AiColumnDefinition::EMBED && (d.dimension <= 0 || d.dimension > 16000))) return false;
  for (size_t i = 0; i < d.dependencies.size(); ++i) {
    const auto &dep = d.dependencies[i];
    if (!dep.column_id || dep.column_id == UINT64_MAX || dep.column_id == d.column_id || dep.column_name.empty()) return false;
    for (size_t j = 0; j < i; ++j) if (dep.column_id == d.dependencies[j].column_id) return false;
  }
  return true;
}
} // namespace

std::string ai_column_sql_text(const std::string &value)
{
  static const char hex[] = "0123456789abcdef";
  std::string output = "convert(X'";
  output.reserve(value.size() * 2 + 32);
  for (unsigned char c : value) { output += hex[c >> 4]; output += hex[c & 15]; }
  return output + "' using utf8mb4)";
}
std::string ai_column_sql_identifier(const std::string &value)
{
  std::string output = "`";
  for (char c : value) { if (c == '`') output += '`'; output += c; }
  return output + '`';
}

int AiColumnDefinition::encode(std::string &output) const
{
  if (!valid(*this)) return OB_INVALID_ARGUMENT;
  try {
    std::string value = "{\"format_version\":1";
    auto number = [&](const char *key, uint64_t n) { value += "," + json_text(key) + ":" + std::to_string(n); };
    auto string = [&](const char *key, const std::string &s) { value += "," + json_text(key) + ":" + json_text(s); };
    number("column_id", column_id); number("state_column_id", state_column_id);
    number("request_column_id", request_column_id); number("result_spec_column_id", result_spec_column_id);
    number("function", function); number("dimension", dimension); number("max_retries", max_retries);
    number("input_is_json", input_is_json ? 1 : 0);
    string("column_name", column_name); string("expression", expression); string("normalized_expr", normalized_expr);
    string("input_expr", input_expr); string("config_expr", config_expr); string("model_name", model_name);
    string("system_prompt", system_prompt);
    value += ",\"dependencies\":[";
    for (size_t i = 0; i < dependencies.size(); ++i) {
      if (i) value += ',';
      value += "{\"column_id\":" + std::to_string(dependencies[i].column_id) +
          ",\"column_name\":" + json_text(dependencies[i].column_name) + "}";
    }
    value += "]}";
    if (value.size() > MAX_DEFINITION_BYTES) return OB_SIZE_OVERFLOW;
    output.swap(value);
    return OB_SUCCESS;
  } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
}

int AiColumnDefinition::decode(const ObString &input)
{
  if (input.empty() || input.length() > MAX_DEFINITION_BYTES) return OB_INVALID_ARGUMENT;
  try {
    ObArenaAllocator allocator;
    ObIJsonBase *base = nullptr;
    int ret = ObJsonBaseFactory::get_json_base(&allocator, input,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base);
    if (OB_FAIL(ret)) return ret;
    if (!base || base->json_type() != ObJsonNodeType::J_OBJECT) return OB_INVALID_ARGUMENT;
    auto &object = *static_cast<ObJsonObject *>(base);
    AiColumnDefinition d;
    int64_t version = 0, type = 0;
    if (!integer(object, "format_version", version) || version != 1 ||
        !integer(object, "function", type) || type < COMPLETE || type > EMBED ||
        !integer(object, "dimension", d.dimension) || !integer(object, "max_retries", d.max_retries) ||
        !identifier(object, "column_id", d.column_id) || !identifier(object, "state_column_id", d.state_column_id) ||
        !identifier(object, "request_column_id", d.request_column_id) || !identifier(object, "result_spec_column_id", d.result_spec_column_id) ||
        !text(object, "column_name", d.column_name) || !text(object, "expression", d.expression) ||
        !text(object, "normalized_expr", d.normalized_expr) || !text(object, "input_expr", d.input_expr) ||
        !text(object, "config_expr", d.config_expr) || !text(object, "model_name", d.model_name) ||
        !text(object, "system_prompt", d.system_prompt)) return OB_INVALID_ARGUMENT;
    d.function = static_cast<Function>(type);
    if (object.get_value("input_is_json") != nullptr) {
      int64_t input_is_json = 0;
      if (!integer(object, "input_is_json", input_is_json) || input_is_json < 0 || input_is_json > 1) return OB_INVALID_ARGUMENT;
      d.input_is_json = input_is_json != 0;
    }
    ObJsonNode *deps = object.get_value("dependencies");
    if (!deps || deps->json_type() != ObJsonNodeType::J_ARRAY) return OB_INVALID_ARGUMENT;
    auto &array = *static_cast<ObJsonArray *>(deps);
    if (array.element_count() > MAX_DEPENDENCIES) return OB_INVALID_ARGUMENT;
    for (uint64_t i = 0; i < array.element_count(); ++i) {
      ObJsonNode *node = array.get_value(i);
      if (!node || node->json_type() != ObJsonNodeType::J_OBJECT) return OB_INVALID_ARGUMENT;
      AiColumnDependency dependency;
      auto &entry = *static_cast<ObJsonObject *>(node);
      if (!identifier(entry, "column_id", dependency.column_id) || !text(entry, "column_name", dependency.column_name)) return OB_INVALID_ARGUMENT;
      d.dependencies.push_back(std::move(dependency));
    }
    if (!valid(d)) return OB_INVALID_ARGUMENT;
    *this = std::move(d);
    return OB_SUCCESS;
  } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
}

int AiColumnDefinition::decode(const ObObj &input)
{
  if (!input.is_string_type()) return OB_INVALID_ARGUMENT;
  if (!input.has_lob_header()) return decode(input.get_string());
  ObTextStringIter iterator(input);
  ObString text;
  int ret = iterator.init(MAX_DEFINITION_BYTES);
  if (OB_SUCC(ret)) ret = iterator.get_full_data(text);
  return OB_SUCC(ret) ? decode(text) : ret;
}

int ai_column_rebind_spec(const schema::ObTableSchema &table, schema::ObColumnSchemaV2 &spec)
{
  if ((spec.get_column_flags() & AI_RESULT_SPEC_COLUMN_FLAG) == 0) return OB_SUCCESS;
  try {
    if (!spec.get_cur_default_value().is_string_type()) return OB_ERR_UNEXPECTED;
    AiColumnDefinition definition;
    int ret = definition.decode(spec.get_cur_default_value());
    if (OB_FAIL(ret)) return ret;
    const std::string prefix = "__ai_" + std::to_string(definition.column_id);
    const schema::ObColumnSchemaV2 *result = table.get_column_schema(ObString(definition.column_name.size(), definition.column_name.data()));
    const schema::ObColumnSchemaV2 *state = table.get_column_schema((prefix + "_state").c_str());
    const schema::ObColumnSchemaV2 *request = table.get_column_schema((prefix + "_request").c_str());
    if (result == nullptr || state == nullptr || request == nullptr || !state->is_hidden() || !request->is_hidden() ||
        (result->get_column_flags() & AI_RESULT_COLUMN_FLAG) == 0) return OB_ERR_UNEXPECTED;
    definition.column_id = result->get_column_id();
    definition.state_column_id = state->get_column_id();
    definition.request_column_id = request->get_column_id();
    definition.result_spec_column_id = spec.get_column_id();
    for (auto &dependency : definition.dependencies) {
      const schema::ObColumnSchemaV2 *column = table.get_column_schema(ObString(dependency.column_name.size(), dependency.column_name.data()));
      if (column == nullptr || column->is_hidden() || (column->get_column_flags() & AI_RESULT_COLUMN_FLAG) != 0) {
        return OB_ERR_UNEXPECTED;
      }
      dependency.column_id = column->get_column_id();
    }
    std::string encoded;
    if (OB_FAIL(definition.encode(encoded))) return ret;
    ObObj value;
    value.set_varchar(ObString(encoded.size(), encoded.data()));
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    value.set_collation_level(CS_LEVEL_COERCIBLE);
    if (OB_FAIL(spec.set_cur_default_value(value, false))) {
    } else if (OB_FAIL(spec.set_orig_default_value(value))) {
    }
    return ret;
  } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
}

} } // namespace oceanbase::share
