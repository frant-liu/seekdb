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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

namespace
{

struct EmbedBatchRow
{
  int64_t row_idx_;
  ObString content_;
};

struct EmbedBatchGroup
{
  EmbedBatchGroup() : model_id_(), dimension_(0), rows_() {}
  ObString model_id_;
  int64_t dimension_;
  ObArray<EmbedBatchRow> rows_;
};

int add_dimension_config(ObIAllocator &allocator, const int64_t dimension, ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  ObJsonInt *dimension_json = nullptr;
  config = nullptr;
  if (dimension <= 0) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dimension_json))) {
  } else if (OB_FAIL(config->add("dimensions", dimension_json))) {
  }
  return ret;
}

} // namespace

ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (types_stack[CONTENT_IDX].get_type() == ObNullType) {
      // A NULL literal input propagates to a NULL result.
    } else if (ob_is_string_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
      types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (ob_is_string_type(types_stack[CONTENT_IDX].get_type())) {
      // TEXT/BLOB input must be read as a lob at evaluation time.
      if (types_stack[CONTENT_IDX].get_charset_type() != CHARSET_UTF8MB4
          && types_stack[CONTENT_IDX].get_collation_type() != CS_TYPE_BINARY) {
        types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input must be a string");
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 3) {
      if (types_stack[DIM_IDX].get_type() == ObNullType) {
        // NULL dimension means the provider default dimension.
      } else if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    if (OB_SUCC(ret)) {
      type.set_varchar();
      type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      type.set_collation_level(CS_LEVEL_COERCIBLE);
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_single(const ObExpr &expr,
                                        ObEvalCtx &ctx,
                                        ObDatum *model_datum,
                                        ObDatum *content_datum,
                                        ObDatum *dim_datum,
                                        ObIAllocator &allocator,
                                        ObString &result)
{
  int ret = OB_SUCCESS;
  ObString model_id = model_datum->get_string();
  ObString content;
  int64_t dimension = 0;
  if (model_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_,
                                                               allocator,
                                                               *content_datum,
                                                               expr.args_[CONTENT_IDX]->datum_meta_,
                                                               expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(),
                                                               content))) {
  } else if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
  } else if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
    dimension = dim_datum->get_int();
    if (dimension <= 0 || dimension > MAX_EMBED_DIMENSION) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    ObAIFuncExprInfo *info = nullptr;
    ObJsonObject *config = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    if (OB_FAIL(add_dimension_config(allocator, dimension, config))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(allocator, *info, resolved_endpoint);
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (OB_ISNULL(arg_model_id) || OB_ISNULL(arg_content)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
  } else if (arg_content->is_null()) {
    // SQL NULL input propagates without a model request.
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString result;
    if (OB_FAIL(eval_ai_embed_single(expr, ctx, arg_model_id, arg_content, arg_dim,
                                     temp_allocator, result))) {
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr,
                                       ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  if (batch_size <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; i++) {
      ObExpr *arg = expr.args_[i];
      if (OB_ISNULL(arg)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(arg->eval_batch(ctx, skip, batch_size))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    ObDatum *res = expr.locate_batch_datums(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    ObArenaAllocator batch_allocator("AIEmbedBatch", OB_MALLOC_NORMAL_BLOCK_SIZE);
    ObArray<EmbedBatchGroup *> groups;
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum &model_datum = expr.args_[MODEL_IDX]->locate_expr_datum(ctx, i);
      ObDatum &content_datum = expr.args_[CONTENT_IDX]->locate_expr_datum(ctx, i);
      if (model_datum.is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
      } else if (content_datum.is_null()) {
        // Handled after all requests succeed so a failed batch publishes
        // nothing.
        continue;
      } else {
        ObString model_id = model_datum.get_string();
        ObString content = content_datum.get_string();
        int64_t dimension = 0;
        if (model_id.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
        } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                       ctx.exec_ctx_,
                       batch_allocator,
                       content_datum,
                       expr.args_[CONTENT_IDX]->datum_meta_,
                       expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(),
                       content))) {
        } else if (content.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
        } else if (expr.arg_cnt_ >= 3) {
          ObDatum &dim_datum = expr.args_[DIM_IDX]->locate_expr_datum(ctx, i);
          if (!dim_datum.is_null()) {
            dimension = dim_datum.get_int();
            if (dimension <= 0 || dimension > MAX_EMBED_DIMENSION) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
            }
          }
        }
        if (OB_SUCC(ret)) {
          EmbedBatchGroup *group = nullptr;
          for (int64_t g = 0; OB_ISNULL(group) && g < groups.count(); g++) {
            EmbedBatchGroup *candidate = groups.at(g);
            if (OB_NOT_NULL(candidate)
                && candidate->dimension_ == dimension
                && candidate->model_id_ == model_id) {
              group = candidate;
            }
          }
          if (OB_ISNULL(group)) {
            if (OB_ISNULL(group = OB_NEWx(EmbedBatchGroup, &batch_allocator))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              group->model_id_ = model_id;
              group->dimension_ = dimension;
              if (OB_FAIL(groups.push_back(group))) {
              }
            }
          }
          if (OB_SUCC(ret)) {
            EmbedBatchRow row;
            row.row_idx_ = i;
            row.content_ = content;
            if (OB_FAIL(group->rows_.push_back(row))) {
            }
          }
        }
      }
    }

    // Send one request per group/chunk and map the results back to the original
    // SQL rows.
    for (int64_t g = 0; OB_SUCC(ret) && g < groups.count(); g++) {
      EmbedBatchGroup *group = groups.at(g);
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      ObJsonObject *config = nullptr;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      if (OB_ISNULL(group)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(add_dimension_config(batch_allocator, group->dimension_, config))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(batch_allocator,
                                                         group->model_id_,
                                                         info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(group->model_id_,
                                                                  batch_allocator,
                                                                  resolved_endpoint))) {
      } else {
        ObAIFuncModel model(batch_allocator, *info, resolved_endpoint);
        int64_t start = 0;
        while (OB_SUCC(ret) && start < group->rows_.count()) {
          int64_t end = start;
          int64_t total_bytes = 0;
          while (end < group->rows_.count()
                 && end - start < EMBED_BATCH_ROW_LIMIT) {
            const int64_t row_len = group->rows_.at(end).content_.length();
            if (end > start && total_bytes + row_len > EMBED_BATCH_BYTE_LIMIT) {
              break;
            }
            total_bytes += row_len;
            end++;
          }
          ObArray<ObString> contents;
          ObArray<ObString> results;
          for (int64_t i = start; OB_SUCC(ret) && i < end; i++) {
            if (OB_FAIL(contents.push_back(group->rows_.at(i).content_))) {
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
          } else if (results.count() != contents.count()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("embedding result count mismatch", K(ret), K(results.count()), K(contents.count()));
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < results.count(); i++) {
              ObString copied_result;
              const int64_t row_idx = group->rows_.at(start + i).row_idx_;
              if (OB_FAIL(ob_write_string(ctx.exec_ctx_.get_allocator(),
                                          results.at(i),
                                          copied_result))) {
              } else {
                res[row_idx].set_string(copied_result);
              }
            }
          }
          start = end;
        }
      }
    }

    // Publish the batch only when every request and result copy succeeded.
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < batch_size; i++) {
        if (skip.at(i) || eval_flags.at(i)) {
          continue;
        }
        ObDatum &content_datum = expr.args_[CONTENT_IDX]->locate_expr_datum(ctx, i);
        if (content_datum.is_null()) {
          res[i].set_null();
        }
        eval_flags.set(i);
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   ObObj const_data;
  //   bool got_data = false;
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
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
