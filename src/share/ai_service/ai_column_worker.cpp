/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER_OMT
#include "share/ai_service/ai_column_worker.h"
#include "share/ai_service/ai_column_definition.h"
#include "share/ob_server_struct.h"
#include "share/ob_lob_access_utils.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include <charconv>
#include "lib/charset/ob_charset.h"
#include <new>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
namespace oceanbase { namespace share {
using namespace common;
using namespace schema;
namespace ai_column_worker_internal {
ObString str(const std::string &s) { return ObString(s.size(), s.data()); }
std::string copy(const ObString &s) { return std::string(s.ptr(), s.length()); }
class InternalWriteGuard {
public:
  explicit InternalWriteGuard(sql::ObSQLSessionInfo *session):session_(session) {
    if (session_!=nullptr) session_->enter_ai_column_internal_write();
  }
  ~InternalWriteGuard() { reset(); }
  void reset() { if (session_!=nullptr) { session_->leave_ai_column_internal_write(); session_=nullptr; } }
private:
  sql::ObSQLSessionInfo *session_;
};
struct Task {
  uint64_t table = 0, column = 0;
  int64_t version = 0, state = 0, retries = 0;
  bool input_null = false, result_null = false;
  std::string request, predicate, input, config, result;
  AiColumnDefinition definition;
};
int write(ObISQLClient &client, const std::string &sql, int64_t *count = nullptr)
{
  int64_t affected = 0;
  int ret = client.write(sql.c_str(), affected);
  if (count != nullptr) *count = affected;
  return ret;
}
int field(sqlclient::ObMySQLResult &row, const char *name, std::string &out, bool *null = nullptr)
{
  ObObj value;
  int ret = row.get_obj(name, value);
  if (OB_FAIL(ret)) return ret;
  if (null != nullptr) *null = value.is_null();
  if (value.is_null()) { out.clear(); return OB_SUCCESS; }
  if (!value.is_string_type()) return OB_OBJ_TYPE_ERROR;
  if (!value.has_lob_header()) { out = copy(value.get_string()); return OB_SUCCESS; }
  ObTextStringIter iterator(value);
  ObString data;
  ObArenaAllocator allocator;
  auto *reader = server_service<common::ObILobReadService>();
  if (reader == nullptr) return OB_NOT_INIT;
  ObLobReadOptions options(*reader, ObTimeUtility::current_time() + 60000000);
  if (OB_FAIL(iterator.init(16 * 1024 * 1024, &options, &allocator))) {
  } else if (OB_FAIL(iterator.get_full_data(data))) {
  } else out = copy(data);
  return ret;
}
int identifier(sqlclient::ObMySQLResult &row, const char *name, uint64_t &out)
{
  int64_t value = 0;
  int ret = row.get_int(name, value);
  if (OB_SUCC(ret) && value <= 0) ret = OB_INVALID_DATA;
  if (OB_SUCC(ret)) out = static_cast<uint64_t>(value);
  return ret;
}
std::string group(const Task &t)
{
  return "table_id=" + std::to_string(t.table) + " AND request_id=" + ai_column_sql_text(t.request);
}
std::string key(const Task &t)
{
  return group(t) + " AND column_id=" + std::to_string(t.column) + " AND definition_version=" + std::to_string(t.version);
}
// ADD AI COLUMN also covers rows which predate the definition. A row lock,
// request assignment and task snapshots are committed together. Restarting
// simply resumes rows whose hidden request is still NULL.
int seed_table(uint64_t table_id, int64_t expected_version, bool &complete)
{
  complete=false;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table=nullptr;
  const ObDatabaseSchema *database=nullptr;
  int ret=GCTX.schema_service_->get_runtime_schema_guard(guard);
  if (OB_SUCC(ret)) ret=guard.get_table_schema(table_id,table);
  if (OB_SUCC(ret) && table != nullptr) ret=guard.get_database_schema(table->get_database_id(),database);
  if (OB_FAIL(ret)) return ret;
  if (table==nullptr || database==nullptr) return OB_SCHEMA_EAGAIN;
  std::vector<Task> definitions;
  std::string filter, projection;
  auto ident=[](const ObString &s) { return ai_column_sql_identifier(copy(s)); };
  const auto &keys=table->get_rowkey_info();
  for (int64_t i=0;i<keys.get_size();++i) {
    uint64_t id=0;
    if (OB_FAIL(keys.get_column_id(i,id))) return ret;
    const auto *column=table->get_column_schema(id);
    if (column==nullptr) return OB_SCHEMA_EAGAIN;
    if (!projection.empty()) projection+=",";
    projection+=ident(column->get_column_name_str())+" AS seed_key_"+std::to_string(i);
  }
  if (projection.empty()) return OB_NOT_SUPPORTED;
  for (auto it=table->column_begin();it!=table->column_end();++it) {
    if (*it==nullptr || (((*it)->get_column_flags() & AI_RESULT_SPEC_COLUMN_FLAG)==0)) continue;
    Task task;
    task.table=table_id;
    task.version=(*it)->get_schema_version();
    if (OB_FAIL(task.definition.decode((*it)->get_cur_default_value()))) return ret;
    task.column=task.definition.column_id;
    const auto *state=table->get_column_schema(task.definition.state_column_id);
    const auto *request=table->get_column_schema(task.definition.request_column_id);
    if (state==nullptr || request==nullptr) return OB_SCHEMA_EAGAIN;
    const std::string index=std::to_string(definitions.size());
    const std::string state_name=ident(state->get_column_name_str()), request_name=ident(request->get_column_name_str());
    if (!filter.empty()) filter+=" OR ";
    filter+="("+state_name+"='PENDING' AND "+request_name+" IS NULL)";
    projection+=","+state_name+" AS seed_state_"+index+","+request_name+" AS seed_request_"+index;
    definitions.push_back(std::move(task));
  }
  int64_t visible_version=0;
  for (const auto &task:definitions) visible_version=std::max(visible_version,task.version);
  if (definitions.empty() || visible_version<expected_version) return OB_SCHEMA_EAGAIN;
  const std::string qualified=ident(database->get_database_name_str())+"."+ident(table->get_table_name_str());
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) return ret;
  auto *session=query::ObInnerSQLConnectionAccess::get_session(trans.get_connection());
  if (session==nullptr) { trans.end(false); return OB_ERR_UNEXPECTED; }
  InternalWriteGuard internal(session);
  {
    ObMySQLProxy::MySQLResult result;
    const std::string query="SELECT /*+ USE_PLAN_CACHE(NONE) */ "+projection+",UUID() AS seed_id FROM "+qualified+
        " WHERE "+filter+" LIMIT 1 FOR UPDATE";
    std::string request, predicate;
    std::vector<size_t> selected;
    if (OB_FAIL(trans.read(result,query.c_str()))) {
    } else if (result.get_result()==nullptr) ret=OB_ERR_UNEXPECTED;
    else if ((ret=result.get_result()->next())==OB_ITER_END) { complete=true; ret=OB_SUCCESS; }
    else if (OB_SUCC(ret)) {
      auto &row=*result.get_result();
      ret=field(row,"seed_id",request);
      ObObjPrintParams params;
      params.character_hex_safe_represent_=true;
      params.need_cast_expr_=false;
      params.tz_info_=session->get_timezone_info();
      for (int64_t i=0;OB_SUCC(ret) && i<keys.get_size();++i) {
        ObObj value;
        uint64_t id=0;
        std::string buffer(1024*1024,'\0');
        int64_t position=0;
        if (OB_FAIL(row.get_obj(("seed_key_"+std::to_string(i)).c_str(),value))) {
        } else if (value.is_null()) ret=OB_INVALID_DATA;
        else if (OB_FAIL(keys.get_column_id(i,id))) {
        } else if (OB_FAIL(value.print_sql_literal(&buffer[0],buffer.size(),position,params))) {
        } else {
          if (!predicate.empty()) predicate+=" AND ";
          predicate+=ident(table->get_column_schema(id)->get_column_name_str())+"="+buffer.substr(0,position);
        }
      }
      for (size_t i=0;OB_SUCC(ret) && i<definitions.size();++i) {
        std::string state, old_request;
        bool null=false;
        if (OB_FAIL(field(row,("seed_state_"+std::to_string(i)).c_str(),state))) {
        } else if (OB_FAIL(field(row,("seed_request_"+std::to_string(i)).c_str(),old_request,&null))) {
        } else if (state=="PENDING" && null) selected.push_back(i);
      }
    }
    result.reset();
    std::string assignments;
    for (size_t index:selected) {
      if (OB_FAIL(ret)) break;
      const auto &task=definitions[index];
      const auto &d=task.definition;
      if (!assignments.empty()) assignments+=",";
      assignments+=ai_column_sql_identifier(d.column_name)+"=NULL,"+
          ident(table->get_column_schema(d.request_column_id)->get_column_name_str())+"="+ai_column_sql_text(request);
      const std::string insert="INSERT INTO oceanbase.__all_ai_column_task "
          "(table_id,column_id,request_id,definition_version,rowkey_predicate,model_input,model_config,state,retry_count,eligible_ts) SELECT "+
          std::to_string(table_id)+","+std::to_string(d.column_id)+","+ai_column_sql_text(request)+","+
          std::to_string(task.version)+","+ai_column_sql_text(predicate)+",("+d.input_expr+"),("+d.config_expr+"),0,0,NOW(6) FROM "+qualified+" WHERE "+predicate;
      ret=write(trans,insert);
    }
    if (OB_SUCC(ret) && !assignments.empty()) ret=write(trans,"UPDATE /*+ USE_PLAN_CACHE(NONE) */ "+qualified+" SET "+assignments+" WHERE "+predicate);
  }
  internal.reset();
  const int end=trans.end(OB_SUCC(ret));
  return OB_SUCC(ret)?end:ret;
}
int seed_pending(std::map<uint64_t,int64_t> &finished, bool &more)
{
  more=false;
  ObMySQLProxy::MySQLResult result;
  int ret=GCTX.sql_proxy_->read(result,"SELECT table_id,MAX(definition_version) AS version FROM oceanbase.__all_ai_column GROUP BY table_id");
  if (OB_FAIL(ret)) return ret;
  if (result.get_result()==nullptr) return OB_ERR_UNEXPECTED;
  uint64_t tid=0;
  int64_t version=0;
  bool found=false;
  while (OB_SUCC(ret=result.get_result()->next())) {
    if (OB_FAIL(identifier(*result.get_result(),"table_id",tid)) || OB_FAIL(result.get_result()->get_int("version",version))) break;
    auto prior=finished.find(tid);
    if (prior==finished.end() || prior->second!=version) { found=true; break; }
  }
  if (ret==OB_ITER_END) ret=OB_SUCCESS;
  result.reset();
  if (OB_SUCC(ret) && found) {
    bool complete=false;
    ret=seed_table(tid,version,complete);
    if (OB_SUCC(ret)) {
      if (complete) {
        if (finished.size()>=10000) finished.clear();
        finished[tid]=version;
      }
      more=true;
    }
  }
  return ret;
}
int claim(std::vector<Task> &tasks, std::string &owner)
{
  int ret = OB_SUCCESS;
  ObMySQLProxy::MySQLResult selected;
  // Each request owns a whole row generation, including columns awaiting retry.
  const char *query = "SELECT table_id,request_id FROM oceanbase.__all_ai_column_task "
      "GROUP BY table_id,request_id HAVING MAX(IF(lease_expiry>NOW(6),1,0))=0 "
      "AND COALESCE(MIN(IF(state IN(2,4),NULL,eligible_ts)),NOW(6))<=NOW(6) ORDER BY table_id LIMIT 1";
  if (OB_FAIL(GCTX.sql_proxy_->read(selected, query))) return ret;
  auto *row = selected.get_result();
  Task first;
  if (row == nullptr) return OB_ERR_UNEXPECTED;
  if (OB_FAIL(row->next())) return ret;
  if (OB_FAIL(identifier(*row, "table_id", first.table)) || OB_FAIL(field(*row, "request_id", first.request))) return ret;
  selected.reset();
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) return ret;
  ObMySQLProxy::MySQLResult result;
  const std::string sql = "SELECT t.*,a.definition FROM oceanbase.__all_ai_column_task t "
      "JOIN oceanbase.__all_ai_column a ON a.table_id=t.table_id AND a.column_id=t.column_id "
      "AND a.definition_version=t.definition_version WHERE t." + group(first) + " FOR UPDATE";
  if (OB_FAIL(trans.read(result, sql.c_str()))) {
  } else if (result.get_result() == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    while (OB_SUCC(ret) && OB_SUCC(ret = result.get_result()->next())) {
      Task t;
      std::string encoded;
      auto &r = *result.get_result();
      t.table = first.table; t.request = first.request;
      if (OB_FAIL(identifier(r, "column_id", t.column)) || OB_FAIL(r.get_int("definition_version", t.version)) ||
          OB_FAIL(r.get_int("state", t.state)) || OB_FAIL(r.get_int("retry_count", t.retries)) ||
          OB_FAIL(field(r,"rowkey_predicate",t.predicate)) || OB_FAIL(field(r,"model_input",t.input,&t.input_null)) ||
          OB_FAIL(field(r,"model_config",t.config)) || OB_FAIL(field(r,"result",t.result,&t.result_null)) ||
          OB_FAIL(field(r,"definition",encoded)) || OB_FAIL(t.definition.decode(str(encoded)))) {
      } else tasks.push_back(std::move(t));
    }
    if (ret == OB_ITER_END) ret = OB_SUCCESS;
  }
  result.reset();
  if (OB_SUCC(ret) && tasks.empty()) ret = OB_ITER_END;
  if (OB_SUCC(ret)) {
    ObMySQLProxy::MySQLResult uuid;
    if (OB_FAIL(trans.read(uuid,"SELECT UUID() AS owner"))) {
    } else if (uuid.get_result() == nullptr) ret = OB_ERR_UNEXPECTED;
    else if (OB_FAIL(uuid.get_result()->next())) {
    } else ret = field(*uuid.get_result(),"owner",owner);
  }
  if (OB_SUCC(ret)) {
    int64_t affected = 0;
    const std::string lease = "UPDATE oceanbase.__all_ai_column_task SET lease_owner=" + ai_column_sql_text(owner) +
        ",lease_expiry=DATE_ADD(NOW(6), INTERVAL 120 SECOND) WHERE " + group(first) +
        " AND (lease_expiry IS NULL OR lease_expiry<=NOW(6))";
    if (OB_FAIL(write(trans,lease,&affected))) {
    } else if (affected != tasks.size()) ret = OB_EAGAIN;
  }
  const int end = trans.end(OB_SUCC(ret));
  return OB_SUCC(ret) ? end : ret;
}
int infer(Task &t)
{
  if (t.input_null) { t.result_null = true; return OB_SUCCESS; }
  t.result_null = false;
  ObArenaAllocator allocator;
  ObAIFuncExprInfo *info = nullptr;
  ObAiModelEndpointInfo endpoint;
  auto *resolver = server_service<query::ObIAiEndpointResolver>();
  ObString name = str(t.definition.model_name), input = str(t.input), config_text = str(t.config), output;
  ObJsonObject *config = nullptr;
  int ret = OB_SUCCESS;
  if (resolver == nullptr) return OB_NOT_INIT;
  if (!t.config.empty() && OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator,config_text,config))) return ret;
  if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator,name,info))) return ret;
  if (OB_FAIL(resolver->resolve_by_model_name(name,allocator,endpoint))) return ret;
  if (t.definition.input_is_json) {
    ObJsonObject *prompt = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator,input,prompt))) return ret;
    if (!ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt)) return OB_INVALID_ARGUMENT;
    if (OB_FAIL(ObAIFuncPromptObjectUtils::expand_prompt_object(allocator,prompt,input))) return ret;
  }
  ObAIFuncModel model(allocator,*info,endpoint);
  if (t.definition.function == AiColumnDefinition::EMBED) {
    if (config == nullptr) config = OB_NEWx(ObJsonObject,&allocator,&allocator);
    if (config == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
    auto *dimension = OB_NEWx(ObJsonInt,&allocator,t.definition.dimension);
    if (dimension == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
    if (OB_FAIL(config->add("dimensions",dimension))) return ret;
    if (OB_FAIL(model.call_dense_embedding(input,config,output))) return ret;
    ObIJsonBase *json = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator,output,ObJsonInType::JSON_TREE,ObJsonInType::JSON_TREE,json))) return ret;
    int64_t actual = 0;
    if (OB_FAIL(ObAIFuncUtils::check_embedding_vector(allocator,json,t.definition.dimension,actual))) return ret;
    t.result = copy(output);
  } else {
    if (OB_FAIL(ObAIFuncUtils::check_complete_config(config))) return ret;
    if (OB_FAIL(model.call_completion(input,str(t.definition.system_prompt),config,output))) return ret;
    // Completion returns provider text with JSON string quoting removed.
    t.result = copy(output);
  }
  // Validate the declared target before the grouped UPDATE performs SQL conversion.
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) return ret;
  if (OB_FAIL(guard.get_table_schema(t.table,table))) return ret;
  const ObColumnSchemaV2 *column = table == nullptr ? nullptr : table->get_column_schema(t.column);
  if (column == nullptr) return OB_SCHEMA_EAGAIN;
  if (column->is_json()) {
    ObIJsonBase *json = nullptr;
    ret = ObJsonBaseFactory::get_json_base(&allocator,str(t.result),ObJsonInType::JSON_TREE,ObJsonInType::JSON_TREE,json);
  } else if (ob_is_integer_type(column->get_data_type())) {
    const auto type = column->get_data_type();
    if (ob_is_unsigned_type(type)) {
      uint64_t number = 0;
      auto parsed = std::from_chars(t.result.data(),t.result.data()+t.result.size(),number);
      if (parsed.ec != std::errc() || parsed.ptr != t.result.data()+t.result.size() ||
          number > UINT_MAX_VAL[type]) ret = OB_INVALID_DATA;
    } else {
      int64_t number = 0;
      auto parsed = std::from_chars(t.result.data(),t.result.data()+t.result.size(),number);
      if (parsed.ec != std::errc() || parsed.ptr != t.result.data()+t.result.size() ||
          number < INT_MIN_VAL[type] || number > INT_MAX_VAL[type]) ret = OB_INVALID_DATA;
    }
  } else if (ob_is_string_type(column->get_data_type())) {
    const auto collation = column->get_collation_type();
    int64_t valid_length = 0;
    if (OB_FAIL(ObCharset::well_formed_len(collation,t.result.data(),t.result.size(),valid_length))) {
    } else if (valid_length != t.result.size()) ret = OB_INVALID_DATA;
    else if (ob_is_text_tc(column->get_data_type())) {
      if (t.result.size() > column->get_data_length()) ret = OB_ERR_DATA_TOO_LONG;
    } else if (ObCharset::strlen_char(collation,t.result.data(),t.result.size()) > column->get_data_length()) {
      ret = OB_ERR_DATA_TOO_LONG;
    }
  }
  return ret;
}
int save(Task &t, const std::string &owner, int model_ret)
{
  t.state = model_ret == OB_SUCCESS ? 2 : t.retries >= t.definition.max_retries ? 4 : 3;
  if (model_ret != OB_SUCCESS) ++t.retries;
  const std::string sql = "UPDATE oceanbase.__all_ai_column_task SET state="+std::to_string(t.state)+
      ",retry_count="+std::to_string(t.retries)+",result="+(t.state==2&&!t.result_null?ai_column_sql_text(t.result):"NULL")+
      ",last_error="+ai_column_sql_text(std::to_string(model_ret))+
      ",eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND),lease_expiry=DATE_ADD(NOW(6),INTERVAL 120 SECOND) WHERE "+
      key(t)+" AND lease_owner="+ai_column_sql_text(owner);
  int64_t affected = 0;
  int ret = write(*GCTX.sql_proxy_,sql,&affected);
  return OB_SUCC(ret) && affected != 1 ? OB_EAGAIN : ret;
}
int publish(std::vector<Task> &tasks, const std::string &owner)
{
  const Task &first = tasks.front();
  for (const auto &task : tasks) {
    if (task.state != 2 && task.state != 4) {
      return write(*GCTX.sql_proxy_,"UPDATE oceanbase.__all_ai_column_task SET lease_owner=NULL,lease_expiry=NULL WHERE "+
          group(first)+" AND lease_owner="+ai_column_sql_text(owner));
    }
  }
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  int ret = GCTX.schema_service_->get_runtime_schema_guard(guard);
  if (OB_SUCC(ret)) ret = guard.get_table_schema(first.table,table);
  if (OB_SUCC(ret) && table != nullptr) ret = guard.get_database_schema(table->get_database_id(),database);
  if (OB_FAIL(ret)) return ret;
  if (table == nullptr || database == nullptr) return OB_SCHEMA_EAGAIN;
  std::string assignments, predicate;
  for (const auto &task : tasks) {
    const auto &d = task.definition;
    const auto *spec = table->get_column_schema(d.result_spec_column_id);
    const auto *state = table->get_column_schema(d.state_column_id);
    const auto *request = table->get_column_schema(d.request_column_id);
    if (spec == nullptr || state == nullptr || request == nullptr || spec->get_schema_version() != task.version) return OB_SCHEMA_EAGAIN;
    if (!assignments.empty()) assignments += ",";
    assignments += ai_column_sql_identifier(d.column_name)+"="+(task.state==2&&!task.result_null?ai_column_sql_text(task.result):"NULL")+","+
        ai_column_sql_identifier(copy(state->get_column_name_str()))+"="+ai_column_sql_text(task.state==2?"READY":"FAILED");
    predicate += " AND "+ai_column_sql_identifier(copy(request->get_column_name_str()))+"="+ai_column_sql_text(first.request);
  }
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) return ret;
  auto *session = query::ObInnerSQLConnectionAccess::get_session(trans.get_connection());
  if (session == nullptr) ret = OB_ERR_UNEXPECTED;
  InternalWriteGuard internal(session);
  // Re-lock ownership before publishing; a replaced/deleted request cannot restore a row.
  ObMySQLProxy::MySQLResult locked;
  const std::string check = "SELECT column_id FROM oceanbase.__all_ai_column_task WHERE "+group(first)+
      " AND lease_owner="+ai_column_sql_text(owner)+" FOR UPDATE";
  size_t count = 0;
  if (OB_SUCC(ret) && OB_FAIL(trans.read(locked,check.c_str()))) {
  } else if (OB_SUCC(ret) && locked.get_result() != nullptr) {
    while (OB_SUCC(ret = locked.get_result()->next())) ++count;
    if (ret == OB_ITER_END) ret = OB_SUCCESS;
    if (OB_SUCC(ret) && count != tasks.size()) ret = OB_EAGAIN;
  }
  locked.reset();
  if (OB_SUCC(ret)) {
    const std::string update = "UPDATE /*+ USE_PLAN_CACHE(NONE) */ "+ai_column_sql_identifier(copy(database->get_database_name_str()))+"."+
        ai_column_sql_identifier(copy(table->get_table_name_str()))+" SET "+assignments+" WHERE "+first.predicate+predicate;
    ret = write(trans,update);
  }
  if (OB_SUCC(ret)) ret = write(trans,"DELETE FROM oceanbase.__all_ai_column_task WHERE "+group(first)+" AND lease_owner="+ai_column_sql_text(owner));
  internal.reset();
  const int end = trans.end(OB_SUCC(ret));
  return OB_SUCC(ret) ? end : ret;
}
} // namespace ai_column_worker_internal
int AiColumnWorker::init() { return set_thread_count(2); }
int AiColumnWorker::start()
{
  recovered_.store(false);
  return ObThreadPool::start();
}
void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumnWorker");
  std::map<uint64_t,int64_t> seeded;
  int64_t next_seed=0;
  while (!has_set_stop()) {
    try {
      if (GCTX.sql_proxy_ != nullptr && GCTX.schema_service_ != nullptr) {
        // This is a single-process worker. Previous process owners cannot
        // publish, so recover their leases before either new thread claims.
        if (!recovered_.load()) {
          if (get_thread_idx()==0) {
            const int ret=ai_column_worker_internal::write(*GCTX.sql_proxy_,
                "UPDATE oceanbase.__all_ai_column_task SET lease_owner=NULL,lease_expiry=NULL WHERE lease_owner IS NOT NULL");
            if (ret==OB_SUCCESS) recovered_.store(true);
            else if (REACH_TIME_INTERVAL(1000000)) LOG_WARN("AI column recovery deferred",K(ret));
          }
          usleep(100000);
          continue;
        }
        if (get_thread_idx()==0 && ObTimeUtility::current_time()>=next_seed) {
          bool more=false;
          const int ret=ai_column_worker_internal::seed_pending(seeded,more);
          if (ret!=OB_SUCCESS && REACH_TIME_INTERVAL(1000000)) LOG_WARN("AI column backfill deferred",K(ret));
          next_seed=ObTimeUtility::current_time()+(more?0:1000000);
        }
        std::vector<ai_column_worker_internal::Task> tasks;
        std::string owner;
        int ret = ai_column_worker_internal::claim(tasks,owner);
        if (ret != OB_SUCCESS && ret != OB_ITER_END && ret != OB_EAGAIN && REACH_TIME_INTERVAL(1000000)) LOG_WARN("AI column worker could not claim a request", K(ret));
        if (OB_SUCC(ret)) {
          for (auto &task : tasks) {
            if (has_set_stop()) break;
            if (task.state == 2 || task.state == 4) continue;
            const int call_ret = ai_column_worker_internal::infer(task);
            if (OB_FAIL(ai_column_worker_internal::save(task,owner,call_ret))) break;
          }
          if (OB_SUCC(ret) && !has_set_stop()) ret = ai_column_worker_internal::publish(tasks,owner);
          if (OB_FAIL(ret)) LOG_WARN("AI column worker deferred a request group",K(ret));
        }
      }
    } catch (const std::bad_alloc &) { const int ret = OB_ALLOCATE_MEMORY_FAILED; LOG_WARN("AI column worker allocation failed",K(ret)); }
    usleep(100000);
  }
}
} }
