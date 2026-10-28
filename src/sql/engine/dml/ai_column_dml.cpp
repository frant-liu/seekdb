/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/ob_sql_context.h"
#include "share/ai_service/ai_column_definition.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace ai_column_dml_internal {

struct Definition {
  AiColumnDefinition value;
  int64_t version;
};
class InternalGuard {
public:
  explicit InternalGuard(ObSQLSessionInfo &session) : session_(session) { session_.enter_ai_column_internal_write(); }
  ~InternalGuard() { session_.leave_ai_column_internal_write(); }
private:
  ObSQLSessionInfo &session_;
};
std::string text(const ObString &s) { return std::string(s.ptr(), s.length()); }
int column_position(const ObDMLBaseCtDef &ctdef, uint64_t id)
{
  for (int64_t i = 0; i < ctdef.column_ids_.count(); ++i) if (ctdef.column_ids_.at(i) == id) return i;
  return -1;
}
// UPDATE buffers one full row containing both old and new expressions. The
// separate row pointers are populated only for split updates and row locks.
const ObDatum *stored_datum(const ObDMLModifyRowNode &node, bool old, int64_t position)
{
  const auto &ctdef = *node.dml_ctdef_;
  const auto &expressions = old ? ctdef.old_row_ : ctdef.new_row_;
  const auto *row = old ? node.old_row_ : node.new_row_;
  if (position < 0 || position >= expressions.count()) return nullptr;
  if (row != nullptr) return position < row->cnt_ ? &row->cells()[position] : nullptr;
  if (node.full_row_ != nullptr) {
    for (int64_t i = 0; i < ctdef.full_row_.count() && i < node.full_row_->cnt_; ++i) {
      if (ctdef.full_row_.at(i) == expressions.at(position)) return &node.full_row_->cells()[i];
    }
  }
  return nullptr;
}
int rowkey_predicate(ObTableModifyOp &op, const ObTableSchema &table,
    const ObDMLBaseCtDef &ctdef, const ObDMLModifyRowNode &node, bool old,
    const ExprFixedArray &expressions, std::string &predicate)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &keys = table.get_rowkey_info();
  if (keys.get_size() == 0) return OB_NOT_SUPPORTED;
  std::string result;
  ObObjPrintParams params = op.get_obj_print_params();
  params.character_hex_safe_represent_ = true;
  params.need_cast_expr_ = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.get_size(); ++i) {
    uint64_t id = 0;
    if (OB_FAIL(keys.get_column_id(i, id))) break;
    const int position = column_position(ctdef, id);
    const ObColumnSchemaV2 *column = table.get_column_schema(id);
    const ObDatum *datum = stored_datum(node, old, position);
    if (column == nullptr || datum == nullptr || position >= expressions.count()) return OB_ERR_UNEXPECTED;
    const ObExpr *expr = expressions.at(position);
    ObObj value;
    if (expr == nullptr || datum->is_null()) return OB_ERR_UNEXPECTED;
    if (OB_FAIL(datum->to_obj(value, expr->obj_meta_, expr->obj_datum_map_))) break;
    const int64_t capacity = 64 * 1024;
    std::string buffer(capacity, '\0');
    int64_t written = 0;
    ret = value.print_sql_literal(&buffer[0], capacity, written, params);
    if (OB_SUCC(ret)) {
      if (i != 0) result += " AND ";
      result += ai_column_sql_identifier(text(column->get_column_name_str())) + " = ";
      result.append(buffer.data(), written);
    }
  }
  if (OB_SUCC(ret)) predicate.swap(result);
  return ret;
}
int dependency_changed(ObTableModifyOp &op, const ObDMLModifyRowNode &row, uint64_t id, bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const ObDMLBaseCtDef &ctdef = *row.dml_ctdef_;
  const int position = column_position(ctdef, id);
  if (position < 0) return OB_SUCCESS; // This column is outside the UPDATE projection.
  const ObDatum *old_datum = stored_datum(row, true, position);
  const ObDatum *new_datum = stored_datum(row, false, position);
  if (old_datum == nullptr || new_datum == nullptr) return OB_ERR_UNEXPECTED;
  const ObDatum &old_value = *old_datum;
  const ObDatum &new_value = *new_datum;
  changed = !ObDatum::binary_equal(old_value, new_value);
  const ObExpr *old_expr = ctdef.old_row_.at(position);
  const ObExpr *new_expr = ctdef.new_row_.at(position);
  if (changed && !old_value.is_null() && !new_value.is_null() && old_expr != nullptr && new_expr != nullptr &&
      ob_is_string_type(old_expr->obj_meta_.get_type()) && ob_is_string_type(new_expr->obj_meta_.get_type())) {
    ObArenaAllocator allocator;
    ObString old_string, new_string;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(op.get_exec_ctx(), allocator,
        old_value, old_expr->datum_meta_, old_expr->obj_meta_.has_lob_header(), old_string))) {
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(op.get_exec_ctx(), allocator,
        new_value, new_expr->datum_meta_, new_expr->obj_meta_.has_lob_header(), new_string))) {
    } else {
      changed = old_string != new_string;
    }
  }
  return ret;
}
int load_definitions(const ObTableSchema &table, std::vector<Definition> &definitions)
{
  int ret = OB_SUCCESS;
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    if (*it == nullptr) return OB_ERR_UNEXPECTED;
    if (((*it)->get_column_flags() & AI_RESULT_SPEC_COLUMN_FLAG) == 0) continue;
    if (!(*it)->get_cur_default_value().is_string_type()) return OB_ERR_UNEXPECTED;
    Definition definition;
    if (OB_FAIL(definition.value.decode((*it)->get_cur_default_value()))) break;
    definition.version = (*it)->get_schema_version();
    definitions.push_back(std::move(definition));
  }
  return ret;
}
int process_row(ObTableModifyOp &op, const ObDMLModifyRowNode &row,
    const ObTableSchema &table, const std::string &qualified, const std::vector<Definition> &definitions)
{
  int ret = OB_SUCCESS;
  const bool deleting = row.dml_event_ == ObDmlEventType::DE_DELETING;
  const bool inserting = row.dml_event_ == ObDmlEventType::DE_INSERTING;
  const bool updating = row.dml_event_ == ObDmlEventType::DE_UPDATING;
  if (!deleting && !inserting && !updating) return OB_ERR_UNEXPECTED;
  const ObDMLBaseCtDef &ctdef = *row.dml_ctdef_;
  std::string old_key, new_key;
  if (!inserting && OB_FAIL(rowkey_predicate(op, table, ctdef, row, true, ctdef.old_row_, old_key))) return ret;
  if (!deleting && OB_FAIL(rowkey_predicate(op, table, ctdef, row, false, ctdef.new_row_, new_key))) return ret;
  const std::string task_base = "DELETE /*+ USE_PLAN_CACHE(NONE) */ FROM oceanbase.__all_ai_column_task WHERE table_id = " + std::to_string(table.get_table_id());
  if (deleting || (updating && old_key != new_key)) {
    const std::string sql = task_base + " AND rowkey_predicate = " + ai_column_sql_text(old_key);
    if (OB_FAIL(op.execute_write(sql.c_str()))) return ret;
  }
  if (deleting) return ret;
  std::vector<const Definition *> affected;
  for (const auto &definition : definitions) {
    if (ctdef.ai_retry_column_id_ != 0 && definition.value.column_id != ctdef.ai_retry_column_id_) continue;
    bool changed = ctdef.ai_retry_column_id_ != 0 || inserting || old_key != new_key;
    for (const auto &dependency : definition.value.dependencies) {
      if (changed || OB_FAIL(ret)) break;
      ret = dependency_changed(op, row, dependency.column_id, changed);
    }
    if (OB_FAIL(ret)) return ret;
    if (changed) affected.push_back(&definition);
  }
  if (affected.empty()) return OB_SUCCESS;
  std::string request;
  {
    ObMySQLProxy::MySQLResult result;
    ObString uuid;
    if (OB_FAIL(op.execute_read("SELECT UUID() AS request_id", result))) {
    } else if (result.get_result() == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result.get_result()->next())) {
    } else if (OB_FAIL(result.get_result()->get_varchar("request_id", uuid))) {
    } else {
      request = text(uuid);
    }
  }
  if (OB_FAIL(ret)) return ret;
  std::string assignments;
  for (const Definition *definition : affected) {
    const auto &d = definition->value;
    const ObColumnSchemaV2 *state = table.get_column_schema(d.state_column_id);
    const ObColumnSchemaV2 *request_column = table.get_column_schema(d.request_column_id);
    if (state == nullptr || request_column == nullptr) return OB_ERR_UNEXPECTED;
    if (!assignments.empty()) assignments += ", ";
    assignments += ai_column_sql_identifier(d.column_name) + " = NULL, " +
        ai_column_sql_identifier(text(state->get_column_name_str())) + " = 'PENDING', " +
        ai_column_sql_identifier(text(request_column->get_column_name_str())) + " = " + ai_column_sql_text(request);
    const std::string sql = task_base + " AND column_id = " + std::to_string(d.column_id) +
        " AND rowkey_predicate = " + ai_column_sql_text(new_key);
    if (OB_FAIL(op.execute_write(sql.c_str()))) return ret;
  }
  const std::string invalidate = "UPDATE /*+ USE_PLAN_CACHE(NONE) */ " + qualified + " SET " + assignments + " WHERE " + new_key;
  if (OB_FAIL(op.execute_write(invalidate.c_str()))) return ret;
  for (const Definition *definition : affected) {
    const auto &d = definition->value;
    const std::string sql = "INSERT /*+ USE_PLAN_CACHE(NONE) */ INTO oceanbase.__all_ai_column_task "
        "(table_id,column_id,request_id,definition_version,rowkey_predicate,model_input,model_config,state,retry_count,eligible_ts) SELECT " +
        std::to_string(table.get_table_id()) + "," + std::to_string(d.column_id) + "," + ai_column_sql_text(request) + "," +
        std::to_string(definition->version) + "," + ai_column_sql_text(new_key) + ",(" + d.input_expr + "),(" +
        d.config_expr + "),0,0,NOW(6) FROM " + qualified + " WHERE " + new_key;
    if (OB_FAIL(op.execute_write(sql.c_str()))) return ret;
  }
  return ret;
}
} // namespace ai_column_dml_internal

int AiColumnDml::process_rows(ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = op.get_exec_ctx().get_my_session();
  if (session == nullptr) return OB_ERR_UNEXPECTED;
  if (session->is_ai_column_internal_write()) return OB_SUCCESS;
  bool needed = false;
  auto &rows = op.get_dml_modify_row_list();
  for (auto it = rows.begin(); it != rows.end(); ++it) {
    if ((*it).dml_ctdef_ != nullptr && (*it).dml_ctdef_->has_ai_columns_) needed = true;
  }
  if (!needed) return OB_SUCCESS;
  ai_column_dml_internal::InternalGuard internal(*session);
  auto &das = op.get_exec_ctx().get_das_ctx();
  struct ReadOwnWritesGuard {
    ObDASCtx &context;
    bool previous;
    explicit ReadOwnWritesGuard(ObDASCtx &value) : context(value), previous(value.iter_uncommitted_row_) { context.iter_uncommitted_row_ = true; }
    ~ReadOwnWritesGuard() { context.iter_uncommitted_row_ = previous; }
  } read_own_writes(das);
  if (OB_FAIL(op.open_inner_conn())) return ret;
  if (OB_FAIL(op.begin_nested_session(true))) return ret;
  try {
    uint64_t previous_table_id = OB_INVALID_ID;
    const ObTableSchema *table = nullptr;
    std::string qualified;
    std::vector<ai_column_dml_internal::Definition> definitions;
    ObSqlCtx *sql_ctx = op.get_exec_ctx().get_sql_ctx();
    if (sql_ctx == nullptr || sql_ctx->schema_guard_ == nullptr) ret = OB_ERR_UNEXPECTED;
    for (auto it = rows.begin(); OB_SUCC(ret) && it != rows.end(); ++it) {
      const ObDMLModifyRowNode &row = *it;
      if (row.dml_ctdef_ == nullptr) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      if (!row.dml_ctdef_->has_ai_columns_ || !row.dml_ctdef_->is_primary_index_) continue;
      const uint64_t table_id = row.dml_ctdef_->das_base_ctdef_.index_tid_;
      if (table_id != previous_table_id) {
        const ObDatabaseSchema *database = nullptr;
        definitions.clear();
        if (OB_FAIL(sql_ctx->schema_guard_->get_table_schema(table_id, table))) {
        } else if (table == nullptr) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(sql_ctx->schema_guard_->get_database_schema(table->get_database_id(), database))) {
        } else if (database == nullptr) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ai_column_dml_internal::load_definitions(*table, definitions))) {
        } else {
          qualified = ai_column_sql_identifier(ai_column_dml_internal::text(database->get_database_name_str())) + "." +
              ai_column_sql_identifier(ai_column_dml_internal::text(table->get_table_name_str()));
          previous_table_id = table_id;
        }
      }
      if (OB_SUCC(ret)) ret = ai_column_dml_internal::process_row(op, row, *table, qualified, definitions);
    }
  } catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  const int end_ret = op.end_nested_session();
  if (OB_SUCC(ret)) ret = end_ret;
  return ret;
}
} }
