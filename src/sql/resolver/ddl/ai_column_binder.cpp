/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ai_column_binder.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "query/api/query/vector/ob_vector_index_util.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace {
int unsupported(const char *message)
{
  LOG_USER_ERROR(OB_NOT_SUPPORTED, message);
  return OB_NOT_SUPPORTED;
}
std::string copy_string(const ObString &s) { return std::string(s.ptr(), s.length()); }
int print_expression(ObRawExprFactory &factory, ObRawExpr *expr, std::string &text)
{
  int ret = OB_SUCCESS;
  const int64_t capacity = 1024 * 1024;
  char *buffer = static_cast<char *>(factory.get_allocator().alloc(capacity));
  int64_t position = 0;
  if (buffer == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
  ObObjPrintParams params;
  params.character_hex_safe_represent_ = true;
  ObRawExprPrinter printer(buffer, capacity, &position, nullptr, params);
  if (OB_SUCC(ret = printer.do_print(expr, T_NONE_SCOPE, true))) {
    text.assign(buffer, position);
  }
  factory.get_allocator().free(buffer);
  return ret;
}
int check_inner_expression(ObRawExpr *expr, int depth = 0)
{
  if (expr == nullptr) return OB_ERR_UNEXPECTED;
  if (depth > 256) return OB_SIZE_OVERFLOW;
  const ObItemType type = expr->get_expr_type();
  if (type == T_FUN_SYS_AI_COMPLETE || type == T_FUN_SYS_AI_EMBED ||
      type == T_FUN_SYS_AI_RERANK) {
    return unsupported("nested model calls in AI column expressions");
  }
  if (type != T_FUN_SYS_AI_PROMPT && ObRawExprUtils::check_deterministic_single(
      expr, ObResolverUtils::CHECK_FOR_GENERATED_COLUMN) != OB_SUCCESS) {
    return unsupported("nondeterministic AI column input expressions");
  }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
    ret = check_inner_expression(expr->get_param_expr(i), depth + 1);
  }
  return ret;
}
int resolve_options(const ParseNode *options, int64_t &retries)
{
  if (options == nullptr) return OB_SUCCESS;
  bool seen_sync = false, seen_retries = false;
  for (int i = 0; i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    if (option == nullptr || option->type_ != T_OP_EQ || option->num_child_ != 2 ||
        option->children_ == nullptr || option->children_[0] == nullptr ||
        option->children_[1] == nullptr) {
      return unsupported("AI column options must be name = literal");
    }
    const ParseNode *key = option->children_[0];
    const ParseNode *value = option->children_[1];
    if (key->type_ == T_COLUMN_REF && key->num_child_ == 3 && key->children_ != nullptr &&
        key->children_[0] == nullptr && key->children_[1] == nullptr) key = key->children_[2];
    if (key == nullptr || key->type_ != T_IDENT) return unsupported("qualified AI column option names");
    const ObString name(key->str_len_, key->str_value_);
    if (name.case_compare("sync_mode") == 0) {
      if (seen_sync || value->type_ != T_VARCHAR ||
          ObString(value->str_len_, value->str_value_).case_compare("async") != 0) {
        return unsupported("AI columns require sync_mode='async'");
      }
      seen_sync = true;
    } else if (name.case_compare("max_retries") == 0) {
      if (seen_retries || value->type_ != T_INT || value->value_ < 0 || value->value_ > 100) {
        return unsupported("AI column max_retries must be an integer between 0 and 100");
      }
      seen_retries = true;
      retries = value->value_;
    } else {
      return unsupported("unknown AI column option");
    }
  }
  return OB_SUCCESS;
}
}

int AiColumnBinder::bind(ObRawExprFactory &factory, const ObSQLSessionInfo &session,
    const ObTableSchema &table, const ObColumnSchemaV2 &column,
    const ParseNode &expression, const ParseNode *options, AiColumnDefinition &definition)
{
  int ret = OB_SUCCESS;
  try {
    AiColumnDefinition value;
    value.column_id = column.get_column_id();
    value.column_name = copy_string(column.get_column_name_str());
    value.expression.assign(expression.str_value_, expression.str_len_);
    ObRawExpr *expr = nullptr;
    ObSEArray<ObQualifiedName, 8> columns;
    if (OB_FAIL(resolve_options(options, value.max_retries))) {
    } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(factory, session,
        expression, expr, columns, nullptr, nullptr))) {
    } else if (expr == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if ((expr->get_expr_type() != T_FUN_SYS_AI_COMPLETE &&
                expr->get_expr_type() != T_FUN_SYS_AI_EMBED) ||
               expr->get_param_count() < 2 || expr->get_param_count() > 3) {
      ret = unsupported("AI column requires AI_COMPLETE or AI_EMBED");
    } else {
      value.function = expr->get_expr_type() == T_FUN_SYS_AI_EMBED
          ? AiColumnDefinition::EMBED : AiColumnDefinition::COMPLETE;
      ObRawExpr *model = expr->get_param_expr(0);
      if (!model->is_const_expr() || !static_cast<ObConstRawExpr *>(model)->get_value().is_string_type() ||
          static_cast<ObConstRawExpr *>(model)->get_value().get_string().empty()) {
        ret = unsupported("AI column model name must be a nonempty string literal");
      } else {
        value.model_name = copy_string(static_cast<ObConstRawExpr *>(model)->get_value().get_string());
      }
      for (int64_t i = 1; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
        ret = check_inner_expression(expr->get_param_expr(i));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      ObQualifiedName &name = columns.at(i);
      const ObColumnSchemaV2 *dependency = table.get_column_schema(name.col_name_);
      if (!name.database_name_.empty() || !name.tbl_name_.empty() || name.is_star_) {
        ret = unsupported("qualified references in AI columns");
      } else if (dependency == nullptr) {
        ret = OB_ERR_BAD_FIELD_ERROR;
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.col_name_.length(), name.col_name_.ptr(),
            "AI column expression");
      } else if (dependency->get_column_id() == column.get_column_id() ||
          dependency->is_hidden() ||
          (dependency->get_column_flags() & AI_RESULT_COLUMN_FLAG) != 0) {
        ret = OB_NOT_SUPPORTED;
      } else if (name.ref_expr_ == nullptr || !name.ref_expr_->is_column_ref_expr()) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObRawExprUtils::init_column_expr(*dependency, &session,
          *static_cast<ObColumnRefRawExpr *>(name.ref_expr_)))) {
      } else if (std::none_of(value.dependencies.begin(), value.dependencies.end(),
          [&](const AiColumnDependency &d) { return d.column_id == dependency->get_column_id(); })) {
        value.dependencies.push_back({dependency->get_column_id(), copy_string(dependency->get_column_name_str())});
      }
    }
    if (OB_SUCC(ret)) {
      // Only inputs execute in the business transaction. The model call is
      // deliberately left untyped here and executed by the asynchronous worker.
      ObRawExpr *input = expr->get_param_expr(1);
      ret = input->formalize(&session, true);
      if (OB_SUCC(ret)) {
        value.input_is_json = input->get_result_type().is_json();
        if (!value.input_is_json && !ob_is_string_type(input->get_data_type()) && !input->get_result_type().is_null()) {
          ret = unsupported("AI column input must be text or AI_PROMPT");
        }
      }
      if (OB_SUCC(ret) && value.function == AiColumnDefinition::COMPLETE && expr->get_param_count() == 3) {
        ret = expr->get_param_expr(2)->formalize(&session, true);
      }
    }
    if (OB_SUCC(ret) && value.function == AiColumnDefinition::EMBED) {
      const auto &type_info = column.get_extended_type_info();
      if (!column.is_collection() || type_info.count() != 1 || type_info.at(0).length() < 7 ||
          std::memcmp(type_info.at(0).ptr(), "VECTOR(", 7) != 0) {
        ret = unsupported("AI_EMBED result column must have VECTOR type");
      } else if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
          column.get_extended_type_info(), value.dimension))) {
      } else if (value.dimension <= 0 || value.dimension > 16000) {
        ret = OB_INVALID_ARGUMENT;
      } else if (expr->get_param_count() == 3) {
        const ObRawExpr *dim = expr->get_param_expr(2);
        if (!dim->is_const_expr()) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          const ObObj &obj = static_cast<const ObConstRawExpr *>(dim)->get_value();
          if (!obj.is_integer_type() || (obj.is_unsigned_integer() && obj.get_uint64() > INT64_MAX)) {
            ret = OB_INVALID_ARGUMENT;
          } else if (obj.get_int() != value.dimension) {
            ret = OB_ERR_INVALID_VECTOR_DIM;
            LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, value.dimension, obj.get_int());
          }
        }
      }
      if (ret == OB_INVALID_ARGUMENT) LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI column embedding dimension mismatch");
    } else if (OB_SUCC(ret)) {
      if (column.is_json()) {
        value.system_prompt = "Return exactly one valid JSON value. Do not include markdown fences or explanations.";
      } else if (ob_is_integer_type(column.get_data_type())) {
        value.system_prompt = "Return exactly one base-10 integer. Do not include explanations.";
      } else if (!ob_is_string_type(column.get_data_type())) {
        ret = unsupported("AI_COMPLETE result type must be JSON, integer, or text");
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(print_expression(factory, expr->get_param_expr(1), value.input_expr))) {
    } else if (OB_SUCC(ret) && value.function == AiColumnDefinition::COMPLETE && expr->get_param_count() == 3 &&
        OB_FAIL(print_expression(factory, expr->get_param_expr(2), value.config_expr))) {
    } else if (OB_SUCC(ret) && OB_FAIL(print_expression(factory, expr, value.normalized_expr))) {
    } else if (OB_SUCC(ret)) {
      if (!value.system_prompt.empty()) value.normalized_expr += " /* " + value.system_prompt + " */";
      definition = std::move(value);
    }
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  return ret;
}

int AiColumnBinder::validate_model(ObIAllocator &allocator, ObSchemaGetterGuard &guard,
    const AiColumnDefinition &definition)
{
  ObAIFuncExprInfo *info = nullptr;
  const ObString name(definition.model_name.size(), definition.model_name.data());
  int ret = ObAIFuncUtils::get_ai_func_info(allocator, name, guard, info);
  if (OB_SUCC(ret) && (info == nullptr ||
      (definition.function == AiColumnDefinition::COMPLETE && info->type_ != share::EndpointType::COMPLETION) ||
      (definition.function == AiColumnDefinition::EMBED && info->type_ != share::EndpointType::DENSE_EMBEDDING))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI column model type does not match its function");
  }
  return ret;
}

int AiColumnBinder::add_hidden_columns(ObTableSchema &table,
    AiColumnDefinition &definition, uint64_t &last_column_id)
{
  int ret = OB_SUCCESS;
  if (last_column_id >= OB_MIN_SHADOW_COLUMN_ID - 3) return OB_SIZE_OVERFLOW;
  try {
    definition.state_column_id = last_column_id + 1;
    definition.request_column_id = last_column_id + 2;
    definition.result_spec_column_id = last_column_id + 3;
    std::string encoded;
    if (OB_FAIL(definition.encode(encoded))) return ret;
    const uint64_t ids[] = {definition.state_column_id, definition.request_column_id,
        definition.result_spec_column_id};
    const char *suffixes[] = {"state", "request", "spec"};
    for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
      ObColumnSchemaV2 hidden;
      hidden.set_table_id(table.get_table_id());
      hidden.set_column_id(ids[i]);
      hidden.set_is_hidden(true);
      hidden.set_nullable(i == 1);
      hidden.set_data_type(i == 2 ? ObLongTextType : ObVarcharType);
      hidden.set_data_length(i == 2 ? 1024 * 1024 : 64);
      hidden.set_charset_type(CHARSET_UTF8MB4);
      hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      const std::string name = "__ai_" + std::to_string(definition.column_id) + "_" + suffixes[i];
      ObObj default_value;
      if (i == 0) default_value.set_varchar(ObString::make_string("PENDING"));
      else if (i == 2) default_value.set_varchar(ObString(encoded.size(), encoded.data()));
      if (i != 1) {
        default_value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        default_value.set_collation_level(CS_LEVEL_COERCIBLE);
      }
      if (i == 2) hidden.add_column_flag(AI_RESULT_SPEC_COLUMN_FLAG);
      if (OB_FAIL(hidden.set_column_name(ObString(name.size(), name.data())))) {
      } else if (OB_FAIL(hidden.set_cur_default_value(default_value, false))) {
      } else if (OB_FAIL(hidden.set_orig_default_value(default_value))) {
      } else if (OB_FAIL(table.add_column(hidden))) {
      }
    }
    if (OB_SUCC(ret)) last_column_id += 3;
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  return ret;
}
} }
