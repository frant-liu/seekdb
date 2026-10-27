/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include <algorithm>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

namespace
{

struct RerankCandidate
{
  double score_;
  int64_t origin_idx_;
  ObJsonObject *result_;
};

bool rerank_candidate_less(const RerankCandidate &left, const RerankCandidate &right)
{
  // Score descending, original position ascending for a stable result.
  if (left.score_ != right.score_) {
    return left.score_ > right.score_;
  }
  return left.origin_idx_ < right.origin_idx_;
}

int get_rerank_index(const ObJsonObject *result_obj, int64_t &index)
{
  int ret = OB_SUCCESS;
  ObJsonNode *index_node = nullptr;
  index = -1;
  if (OB_ISNULL(result_obj) || OB_ISNULL(index_node = result_obj->get_value("index"))) {
    ret = OB_INVALID_DATA;
  } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
    index = index_node->get_int();
  } else if (index_node->json_type() == ObJsonNodeType::J_UINT) {
    index = static_cast<int64_t>(index_node->get_uint());
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

int get_rerank_score(ObIAllocator &allocator,
                     ObJsonObject *result_obj,
                     ObJsonNode *&score_node,
                     double &score)
{
  int ret = OB_SUCCESS;
  score_node = nullptr;
  score = 0.0;
  if (OB_ISNULL(result_obj)) {
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(score_node = result_obj->get_value("relevance_score"))) {
    score_node = result_obj->get_value("score");
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(score_node)) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(allocator, score_node, score))) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

} // namespace

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_type(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_type(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (OB_ISNULL(arg_model_id) || OB_ISNULL(arg_query) || OB_ISNULL(arg_documents)
             || arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObJsonArray *document_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    bool has_doc_key = false;
    ObString doc_key;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                                               j_base, is_null_result))) {
    } else if (is_null_result || OB_ISNULL(j_base)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, documents is null");
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (0 == document_array->element_count()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else if (expr.arg_cnt_ >= 4 && OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null()) {
      ObExpr *doc_key_expr = expr.args_[DOC_KEY_IDX];
      if (OB_ISNULL(doc_key_expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (ob_is_json(doc_key_expr->datum_meta_.type_)) {
        // A json scalar field name is accepted as well.
        ObIJsonBase *doc_key_base = nullptr;
        bool is_json_null = false;
        if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOC_KEY_IDX,
                                                   doc_key_base, is_json_null))) {
        } else if (is_json_null || OB_ISNULL(doc_key_base)) {
        } else if (doc_key_base->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
        } else {
          doc_key.assign_ptr(doc_key_base->get_data(), static_cast<int32_t>(doc_key_base->get_data_length()));
        }
      } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_,
                                                                   temp_allocator,
                                                                   *arg_doc_key,
                                                                   doc_key_expr->datum_meta_,
                                                                   doc_key_expr->obj_meta_.has_lob_header(),
                                                                   doc_key))) {
      }
      has_doc_key = OB_SUCC(ret) && !doc_key.empty();
    }

    // Extract the text of every document and validate the input shape.
    ObArray<ObString> document_texts;
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(document_array->element_count()); i++) {
      ObJsonNode *doc_node = document_array->get_value(i);
      if (OB_ISNULL(doc_node)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
      } else if (has_doc_key) {
        ObJsonNode *doc_value = nullptr;
        if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (OB_ISNULL(doc_value = static_cast<ObJsonObject *>(doc_node)->get_value(doc_key))) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
        } else if (doc_value->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
        } else if (OB_FAIL(document_texts.push_back(static_cast<ObJsonString *>(doc_value)->get_str()))) {
        }
      } else if (doc_node->json_type() != ObJsonNodeType::J_STRING) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
      } else if (OB_FAIL(document_texts.push_back(static_cast<ObJsonString *>(doc_node)->get_str()))) {
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
      ObArray<RerankCandidate> candidates;
      const int64_t document_count = document_texts.count();
      for (int64_t start = 0; OB_SUCC(ret) && start < document_count; start += RERANK_BATCH_SIZE) {
        const int64_t batch_count = std::min(RERANK_BATCH_SIZE, document_count - start);
        ObArray<ObString> batch_texts;
        ObJsonArray *batch_document_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        for (int64_t i = 0; OB_SUCC(ret) && i < batch_count; i++) {
          if (OB_FAIL(batch_texts.push_back(document_texts.at(start + i)))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(temp_allocator,
                                                                            batch_texts,
                                                                            batch_document_array))) {
        } else if (OB_FAIL(model.call_rerank(query, batch_document_array, batch_result_array))) {
        } else if (OB_ISNULL(batch_result_array)
                   || batch_result_array->json_type() != ObJsonNodeType::J_ARRAY) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "ai_rerank, result is not array");
        } else if (static_cast<int64_t>(batch_result_array->element_count()) != batch_count) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "ai_rerank, result count is not equal to document count");
        } else {
          char *seen = static_cast<char *>(temp_allocator.alloc(batch_count));
          if (OB_ISNULL(seen)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMSET(seen, 0, batch_count);
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < batch_count; i++) {
            ObJsonNode *result_node = batch_result_array->get_value(i);
            ObJsonObject *result_obj = nullptr;
            int64_t batch_index = -1;
            ObJsonNode *score_node = nullptr;
            double score = 0.0;
            if (OB_ISNULL(result_node) || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
              ret = OB_INVALID_DATA;
            } else if (OB_FALSE_IT(result_obj = static_cast<ObJsonObject *>(result_node))) {
            } else if (OB_FAIL(get_rerank_index(result_obj, batch_index))) {
              LOG_USER_ERROR(OB_INVALID_DATA, "ai_rerank, result index is invalid");
            } else if (batch_index < 0 || batch_index >= batch_count || 0 != seen[batch_index]) {
              ret = OB_INVALID_DATA;
              LOG_USER_ERROR(OB_INVALID_DATA, "ai_rerank, result index is invalid");
            } else if (OB_FAIL(get_rerank_score(temp_allocator, result_obj, score_node, score))) {
              LOG_USER_ERROR(OB_INVALID_DATA, "ai_rerank, result score is invalid");
            } else {
              seen[batch_index] = 1;
              const int64_t origin_idx = start + batch_index;
              ObJsonObject *output_obj = nullptr;
              if (has_doc_key) {
                ObJsonNode *origin_doc = document_array->get_value(static_cast<uint64_t>(origin_idx));
                ObJsonNode *score_clone = nullptr;
                if (OB_ISNULL(origin_doc) || origin_doc->json_type() != ObJsonNodeType::J_OBJECT) {
                  ret = OB_ERR_UNEXPECTED;
                } else if (OB_ISNULL(output_obj = static_cast<ObJsonObject *>(
                               origin_doc->clone(&temp_allocator, true)))) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else if (OB_ISNULL(score_clone = score_node->clone(&temp_allocator, true))) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else if (OB_FAIL(output_obj->add(MODEL_SCORE_KEY, score_clone))) {
                }
              } else {
                // Keep the provider returned object, only rewrite the index to
                // the global input position.
                ObJsonInt *global_index = nullptr;
                output_obj = result_obj;
                if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator,
                                                            origin_idx,
                                                            global_index))) {
                } else if (OB_FAIL(output_obj->add(INDEX_KEY, global_index))) {
                }
              }
              if (OB_SUCC(ret)) {
                RerankCandidate candidate;
                candidate.score_ = score;
                candidate.origin_idx_ = origin_idx;
                candidate.result_ = output_obj;
                if (OB_FAIL(candidates.push_back(candidate))) {
                }
              }
            }
          }
        }
      }

      if (OB_SUCC(ret) && candidates.count() > 0) {
        std::sort(&candidates[0], &candidates[0] + candidates.count(), rerank_candidate_less);
      }
      if (OB_SUCC(ret)) {
        ObJsonArray *result_array = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, result_array))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < candidates.count(); i++) {
          if (OB_ISNULL(candidates[i].result_)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(result_array->append(candidates[i].result_))) {
          }
        }
        if (OB_SUCC(ret)) {
          ObString raw_str;
          if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
          } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
