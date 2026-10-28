/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/resolver/ddl/ai_column_binder.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/parser/ob_parser.h"
#include "sql/ob_sql_init.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_table_schema.h"
#include "lib/allocator/page_arena.h"
#include <cstdio>
#include <cstring>

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;
using namespace oceanbase::sql;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

namespace {
ParseNode *find_ai_definition(ParseNode *node)
{
  if (node == nullptr) return nullptr;
  if (node->type_ == T_COLUMN_DEFINITION && node->num_child_ == 6) return node;
  for (int i = 0; i < node->num_child_; ++i) {
    ParseNode *found = find_ai_definition(node->children_[i]);
    if (found != nullptr) return found;
  }
  return nullptr;
}
int bind_sql(const char *sql, ObTableSchema &table, ObSQLSessionInfo &session,
    AiColumnDefinition &definition)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, session.get_sql_mode());
  ParseResult parsed;
  int ret = parser.parse(ObString::make_string(sql), parsed);
  if (ret != OB_SUCCESS) {
    std::fprintf(stderr, "parse returned %d: %s\n", ret, sql);
    return ret;
  }
  ParseNode *node = find_ai_definition(parsed.result_tree_);
  if (node == nullptr) return OB_ERR_UNEXPECTED;
  ObRawExprFactory factory(allocator);
  ret = AiColumnBinder::bind(factory, session, table, *table.get_column_schema("result"),
      *node->children_[4], node->children_[5], definition);
  if (ret != OB_SUCCESS) std::fprintf(stderr, "bind returned %d: %s\n", ret, sql);
  return ret;
}
int add_column(ObTableSchema &table, uint64_t id, const char *name, ObObjType type, bool ai = false)
{
  ObColumnSchemaV2 column;
  column.set_table_id(table.get_table_id());
  column.set_column_id(id);
  column.set_data_type(type);
  column.set_data_length(1024);
  column.set_nullable(true);
  column.set_charset_type(CHARSET_UTF8MB4);
  column.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  if (ai) column.add_column_flag(AI_RESULT_COLUMN_FLAG);
  int ret = column.set_column_name(name);
  if (ret == OB_SUCCESS) ret = table.add_column(column);
  return ret;
}
}

int main()
{
  OB_LOGGER.set_file_name("ai-column-resolver.log", true);
  OB_LOGGER.set_enable_async_log(false);
  OB_LOGGER.set_disable_logging(false);
  OB_LOGGER.set_log_level("INFO");
  CHECK(init_sql_factories() == OB_SUCCESS);
  CHECK(init_sql_expr_static_var() == OB_SUCCESS);
  CHECK(ObSysVariables::init_default_values() == OB_SUCCESS);
  CHECK(ObBasicSessionInfo::init_sys_vars_cache_base_values() == OB_SUCCESS);
  ObArenaAllocator allocator;
  ObSQLSessionInfo session;
  CHECK(session.test_init(1, 1, &allocator) == OB_SUCCESS);
  const int session_ret = session.load_essential_sys_vars_only(false, false);
  if (session_ret != OB_SUCCESS) std::fprintf(stderr, "session defaults: %d\n", session_ret);
  CHECK(session_ret == OB_SUCCESS);
  ObExecContext exec_context(allocator);
  exec_context.set_my_session(&session);
  ObSQLSessionInfo::ExecCtxSessionRegister registered(session, &exec_context);
  CHECK(exec_context.create_physical_plan_ctx() == OB_SUCCESS);
  {
    const char *retry_sql[] = {
      "alter table t retry ai column result",
      "alter table db.t retry ai column result where status='FAILED' and id in(1,3)",
      "alter table t retry ai column result where `status`='active'",
    };
    for (const char *text:retry_sql) {
      ObParser parser(allocator,session.get_sql_mode());
      ParseResult parsed;
      const int parse_ret=parser.parse(ObString::make_string(text),parsed);
      if (parse_ret!=OB_SUCCESS) std::fprintf(stderr,"parse %s: %d %s\n",text,parse_ret,parsed.error_msg_);
      CHECK(parse_ret==OB_SUCCESS);
      CHECK(parsed.result_tree_->num_child_==1);
      const auto *statement=parsed.result_tree_->children_[0];
      CHECK(statement->type_==T_UPDATE && statement->reserved_==AI_COLUMN_RETRY_UPDATE);
    }
    ObParser parser(allocator,session.get_sql_mode());
    ParseResult parsed;
    CHECK(parser.parse(ObString::make_string("show ai columns from db.t"),parsed)==OB_SUCCESS);
    CHECK(parsed.result_tree_->children_[0]->type_==T_SHOW_COLUMNS);
    CHECK(parser.parse(ObString::make_string("alter table t retry ai column result, add x int"),parsed)==OB_SUCCESS);
    CHECK(parsed.result_tree_->children_[0]->reserved_ == AI_COLUMN_RETRY_UPDATE + 2);
  }
  ObTableSchema table;
  table.set_table_id(50001);
  CHECK(add_column(table, 16, "body", ObVarcharType) == OB_SUCCESS);
  CHECK(add_column(table, 17, "other", ObVarcharType) == OB_SUCCESS);
  CHECK(add_column(table, 18, "result", ObJsonType, true) == OB_SUCCESS);
  CHECK(add_column(table, 19, "prior", ObVarcharType, true) == OB_SUCCESS);

  AiColumnDefinition definition;
  CHECK(bind_sql("create table t(result json AI COLUMN(AI_COMPLETE('model',concat(body,body))) "
      "WITH(sync_mode='async',max_retries=2))", table, session, definition) == OB_SUCCESS);
  CHECK(definition.dependencies.size() == 1);
  CHECK(definition.dependencies[0].column_id == 16);
  CHECK(definition.max_retries == 2);
  CHECK(definition.model_name == "model");
  CHECK(definition.system_prompt.find("Return exactly one valid JSON value") != std::string::npos);
  CHECK(definition.input_expr.find("body") != std::string::npos);
  CHECK(definition.config_expr == "NULL");

  const char *invalid[] = {
      "create table t(result json AI COLUMN(AI_COMPLETE('model',prior)))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',result)))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',missing)))",
      "create table t(result json AI COLUMN(AI_COMPLETE(body,body)))",
      "create table t(result json AI COLUMN(AI_COMPLETE('',body)))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',AI_COMPLETE('model',body))))",
      "create table t(result json AI COLUMN(concat(body,other)))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',body)) WITH(sync_mode='sync'))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',body)) WITH(max_retries=-1))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',body)) WITH(max_retries=1.5))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',body)) WITH(max_retries=101))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',body)) WITH(max_retries=1,max_retries=2))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',body)) WITH(unknown_option=1))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',@input)))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',(select 1))))",
      "create table t(result json AI COLUMN(AI_COMPLETE('model',concat(body,rand()))))",
  };
  for (const char *sql : invalid) {
    if (bind_sql(sql, table, session, definition) == OB_SUCCESS) {
      std::fprintf(stderr, "unexpected acceptance: %s\n", sql);
      return 1;
    }
  }

  CHECK(bind_sql("create table t(result json AI COLUMN(AI_COMPLETE('model',"
      "AI_PROMPT('说明: {0}; {1}',body,other))) WITH(SYNC_MODE='ASYNC',MAX_RETRIES=0))",
      table, session, definition) == OB_SUCCESS);
  CHECK(definition.dependencies.size() == 2);
  CHECK(definition.max_retries == 0);
  CHECK(definition.input_is_json);
  definition.state_column_id = 20;
  definition.request_column_id = 21;
  definition.result_spec_column_id = 22;
  std::string encoded;
  CHECK(definition.encode(encoded) == OB_SUCCESS);
  AiColumnDefinition restored;
  CHECK(restored.decode(ObString(encoded.size(), encoded.data())) == OB_SUCCESS);
  CHECK(restored.input_expr == definition.input_expr);
  CHECK(restored.system_prompt == definition.system_prompt);
  CHECK(restored.dependencies.size() == definition.dependencies.size());
  const std::string previous = restored.model_name;
  CHECK(restored.decode(ObString::make_string("{\"format_version\":2}")) != OB_SUCCESS);
  CHECK(restored.model_name == previous);
  definition.state_column_id = definition.column_id;
  CHECK(definition.encode(encoded) != OB_SUCCESS);
  CHECK(ai_column_sql_identifier("a`b") == "`a``b`");
  CHECK(ai_column_sql_text("'\\\n") == "convert(X'275c0a' using utf8mb4)");

  ObColumnSchemaV2 *result = table.get_column_schema("result");
  CHECK(result != nullptr);
  result->set_data_type(ObIntType);
  CHECK(bind_sql("create table t(result bigint AI COLUMN(AI_COMPLETE('model',body)))",
      table, session, definition) == OB_SUCCESS);
  CHECK(definition.system_prompt.find("base-10 integer") != std::string::npos);
  result->set_data_type(ObCollectionSQLType);
  ObSEArray<ObString, 1> type_info;
  CHECK(type_info.push_back(ObString::make_string("VECTOR(4)")) == OB_SUCCESS);
  CHECK(result->set_extended_type_info(type_info) == OB_SUCCESS);
  CHECK(bind_sql("create table t(result vector(4) AI COLUMN(AI_EMBED('model',body)))",
      table, session, definition) == OB_SUCCESS);
  CHECK(definition.dimension == 4);
  CHECK(!definition.input_is_json);
  CHECK(bind_sql("create table t(result vector(4) AI COLUMN(AI_EMBED('model',body,3)))",
      table, session, definition) == OB_ERR_INVALID_VECTOR_DIM);
  CHECK(bind_sql("create table t(result vector(4) AI COLUMN(AI_EMBED('model',body,4)))",
      table, session, definition) == OB_SUCCESS);
  const char *bad_dimensions[] = {"3", "0", "-1", "4.0", "null", "body", "18446744073709551615"};
  for (const char *dimension : bad_dimensions) {
    const std::string sql = std::string("create table t(result vector(4) AI COLUMN(AI_EMBED('model',body,") + dimension + ")))";
    CHECK(bind_sql(sql.c_str(), table, session, definition) != OB_SUCCESS);
  }
  CHECK(bind_sql("create table t(result vector(4) AI COLUMN(AI_EMBED('model',body)))",
      table, session, definition) == OB_SUCCESS);
  uint64_t last_id = 19;
  CHECK(AiColumnBinder::add_hidden_columns(table, definition, last_id) == OB_SUCCESS);
  CHECK(last_id == 22);
  CHECK(table.get_column_schema(20)->is_hidden());
  CHECK(table.get_column_schema(21)->is_hidden());
  CHECK(table.get_column_schema(22)->is_hidden());
  CHECK(table.get_column_schema(20)->get_cur_default_value().get_string() == "PENDING");
  CHECK(table.get_column_schema(21)->get_cur_default_value().is_null());
  CHECK(restored.decode(table.get_column_schema(22)->get_cur_default_value().get_string()) == OB_SUCCESS);
  CHECK(restored.dimension == 4 && restored.column_id == 18 && restored.state_column_id == 20);
  CHECK(ai_column_rebind_spec(table, *table.get_column_schema(22)) == OB_SUCCESS);
  ObTableSchema remapped;
  remapped.set_table_id(table.get_table_id());
  for (int64_t i = 0; i < table.get_column_count(); ++i) {
    ObColumnSchemaV2 copy;
    CHECK(copy.assign(*table.get_column_schema_by_idx(i)) == OB_SUCCESS);
    copy.set_column_id(copy.get_column_id() + 100);
    CHECK(remapped.add_column(copy) == OB_SUCCESS);
  }
  CHECK(ai_column_rebind_spec(remapped, *remapped.get_column_schema(122)) == OB_SUCCESS);
  CHECK(restored.decode(remapped.get_column_schema(122)->get_cur_default_value().get_string()) == OB_SUCCESS);
  CHECK(restored.column_id == 118 && restored.state_column_id == 120 &&
      restored.request_column_id == 121 && restored.result_spec_column_id == 122);
  CHECK(restored.dependencies.size() == 1 && restored.dependencies[0].column_id == 116);
  const ObColumnSchemaV2 &spec = *remapped.get_column_schema(122);
  std::string serialized(spec.get_serialize_size(), '\0');
  int64_t position = 0;
  CHECK(spec.serialize(&serialized[0], serialized.size(), position) == OB_SUCCESS);
  ObColumnSchemaV2 roundtrip;
  position = 0;
  CHECK(roundtrip.deserialize(serialized.data(), serialized.size(), position) == OB_SUCCESS);
  CHECK(roundtrip.is_hidden() && (roundtrip.get_column_flags() & AI_RESULT_SPEC_COLUMN_FLAG) != 0);
  CHECK(restored.decode(roundtrip.get_cur_default_value().get_string()) == OB_SUCCESS);
  CHECK(restored.column_id == 118 && restored.dimension == 4);
  std::puts("AI column binder and definition checks passed");
  return 0;
}
