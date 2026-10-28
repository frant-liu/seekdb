/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_runtime.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/ai_service/ai_column_definition.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/ob_errno.h"
#include <new>
#include <string>
namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace share::schema;
int AiColumnRuntime::check_ready(ObExecContext &ctx, const ObIArray<uint64_t> &references)
{
  auto *session = ctx.get_my_session();
  if (session == nullptr) return OB_ERR_UNEXPECTED;
  if (references.empty() || session->is_ai_column_internal_write()) return OB_SUCCESS;
  ObObj strict;
  int ret = session->get_sys_variable(SYS_VAR_AI_COLUMN_STRICT_READ,strict);
  if (OB_FAIL(ret) || strict.get_int() == 0) return ret;
  if (references.count()%2 != 0 || ctx.get_sql_ctx() == nullptr || ctx.get_sql_ctx()->schema_guard_ == nullptr) return OB_ERR_UNEXPECTED;
  sqlclient::ObISQLConnectionGuard connection;
  auto *raw_connection = session->get_inner_conn();
  if (raw_connection == nullptr) {
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session,connection))) return ret;
    raw_connection = connection.get_ptr();
  }
  auto *inner = as_inner_sql_connection(raw_connection);
  if (inner == nullptr) return OB_ERR_UNEXPECTED;
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_connection;
  uint64_t unfinished_table=0, unfinished_column=0;
  session->enter_ai_column_internal_write();
  const bool started = OB_SUCC(ret = inner->begin_nested_session(saved_session,saved_connection,true));
  if (started) {
    try {
      auto &guard = *ctx.get_sql_ctx()->schema_guard_;
      for (int64_t i = 0; OB_SUCC(ret) && i < references.count(); i+=2) {
        const uint64_t tid = references.at(i), cid = references.at(i+1);
        const ObTableSchema *table = nullptr;
        const ObDatabaseSchema *database = nullptr;
        if (OB_FAIL(guard.get_table_schema(tid,table))) break;
        if (table == nullptr) { ret = OB_SCHEMA_EAGAIN; break; }
        if (OB_FAIL(guard.get_database_schema(table->get_database_id(),database))) break;
        if (database == nullptr) { ret = OB_SCHEMA_EAGAIN; break; }
        AiColumnDefinition definition;
        bool found = false;
        for (auto it=table->column_begin(); OB_SUCC(ret) && it!=table->column_end(); ++it) {
          if (*it != nullptr && ((*it)->get_column_flags() & AI_RESULT_SPEC_COLUMN_FLAG) != 0) {
            if (OB_FAIL(definition.decode((*it)->get_cur_default_value()))) break;
            if (definition.column_id == cid) { found=true; break; }
          }
        }
        if (OB_FAIL(ret)) break;
        if (!found) { ret=OB_SCHEMA_EAGAIN; break; }
        auto *state = table->get_column_schema(definition.state_column_id);
        if (state == nullptr) { ret=OB_SCHEMA_EAGAIN; break; }
        auto ident=[](const ObString &s) { return ai_column_sql_identifier(std::string(s.ptr(),s.length())); };
        const std::string name=ident(state->get_column_name_str());
        const std::string query="SELECT /*+ USE_PLAN_CACHE(NONE) */ 1 FROM "+ident(database->get_database_name_str())+"."+
            ident(table->get_table_name_str())+" WHERE "+name+" IS NULL OR "+name+" <> 'READY' LIMIT 1";
        ObMySQLProxy::MySQLResult result;
        if (OB_FAIL(inner->execute_read(query.c_str(),result))) break;
        if (result.get_result() == nullptr) ret=OB_ERR_UNEXPECTED;
        else {
          ret=result.get_result()->next();
          if (ret==OB_ITER_END) ret=OB_SUCCESS;
          else if (OB_SUCC(ret)) {
            ret=OB_AI_COLUMN_NOT_READY;
            unfinished_table=tid; unfinished_column=cid;
          }
        }
      }
    } catch (const std::bad_alloc &) { ret=OB_ALLOCATE_MEMORY_FAILED; }
    const int end=inner->end_nested_session(saved_session,saved_connection);
    if (OB_SUCC(ret)) ret=end;
  }
  session->leave_ai_column_internal_write();
  if (ret==OB_AI_COLUMN_NOT_READY) LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY,unfinished_table,unfinished_column);
  return ret;
}
} }
