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
#include "ob_ai_func_utils.h"
#include "ob_ai_func_client.h"
#include "query/engine/expr/ob_ai_model_resolver.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>

namespace oceanbase 
{
namespace common 
{

int ObOpenAIUtils::get_header(common::ObIAllocator &allocator,
                              ObString &api_key,
                              common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (api_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //["Authorization: Bearer %.*s", "Content-Type: application/json"]
    int auth_header_len = 1024;
    char *auth_header_str = (char *)allocator.alloc(auth_header_len);
    ObString content_type_str("Content-Type: application/json");
    ObString content_type_c_str;
    if (OB_FAIL(ob_write_string(allocator, content_type_str, content_type_c_str, true))) {
    } else if (OB_ISNULL(auth_header_str)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("Failed to allocate memory for auth header string and content type string", K(ret));
    } else {
      int auth_header_pos = snprintf(auth_header_str, auth_header_len,
                         "Authorization: Bearer %.*s", api_key.length(), api_key.ptr());
      if (auth_header_pos < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(headers.push_back(ObString(auth_header_str)))) {
      } else if (OB_FAIL(headers.push_back(content_type_c_str))) {
      }
    }
  }
  return ret;
}
int ObOpenAIUtils::ObOpenAIComplete::get_header(common::ObIAllocator &allocator,
                                                ObString &api_key,
                                                common::ObArray<ObString> &headers) 
{
  return ObOpenAIUtils::get_header(allocator, api_key, headers);
}

int ObOpenAIUtils::ObOpenAIComplete::get_body(common::ObIAllocator &allocator,
                                              common::ObString &model,
                                              common::ObString &prompt,
                                              common::ObString &content,
                                              common::ObJsonObject *config,
                                              common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "messages": [{"role": "system", "content": "*"}, {"role": "user", "content": "*"}]}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *messages_array = nullptr;
    ObString reserved_keys[] = {ObString("model"), ObString("messages"), ObString("stream")};
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(construct_messages_array(allocator, prompt, content, messages_array))) {
    } else if (OB_FAIL(body_obj->add("messages", messages_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object_except(allocator, config,
                                                                     reserved_keys,
                                                                     ARRAYSIZEOF(reserved_keys),
                                                                     body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::construct_messages_array(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonArray *&messages)
{
  int ret = OB_SUCCESS;
  if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //messages: [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": "What is the capital of France?"}]
    ObJsonArray *messages_array = nullptr;
    ObJsonObject *sys_message_obj = nullptr;
    ObJsonObject *user_message_obj = nullptr;
    ObString system_str("system");
    ObString user_str("user");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, messages_array))) {
    } else if(!prompt.empty()) {
      if (OB_FAIL(construct_message_obj(allocator, system_str, prompt, sys_message_obj))) {
      } else if (OB_FAIL(messages_array->append(sys_message_obj))) {
      }
    } 
    if (OB_SUCC(ret)) {
      if (OB_FAIL(construct_message_obj(allocator, user_str, content, user_message_obj))) {
      } else if (OB_FAIL(messages_array->append(user_message_obj))) {
      } else {
        messages = messages_array;
      }
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::construct_message_obj(ObIAllocator &allocator, ObString &role, ObString &content, ObJsonObject *&message)
{
  int ret = OB_SUCCESS;
  if (role.empty() || content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *message_obj = nullptr;
    ObJsonString *role_json_str = nullptr;
    ObJsonString *content_json_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, message_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, role, role_json_str))) {
    } else if (OB_FAIL(message_obj->add("role", role_json_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, content, content_json_str))) {
    } else if (OB_FAIL(message_obj->add("content", content_json_str))) {
    } else {
      message = message_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"response_format":{"type":"json_object"}}
    ObString json_str("json_object");
    ObJsonString *json_str_obj = nullptr;
    ObJsonObject *json_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, json_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, json_str, json_str_obj))) {
    } else if (OB_FAIL(json_obj->add("type", json_str_obj))) {
    } else if (OB_FAIL(config->add("response_format", json_obj))) {
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIComplete::parse_output(common::ObIAllocator &allocator, 
                                                  common::ObJsonObject *http_response,
                                                  common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIJsonBase *j_tree = http_response;
    common::ObString path_text("$.choices[0].message.content");
    ObJsonPath j_path(path_text, &allocator);
    ObJsonSeekResult hit;
    if (OB_FAIL(j_path.parse_path())) {
    } else if (OB_FAIL(j_tree->seek(j_path, j_path.path_node_cnt(), false, false, hit))) {
    } else if (hit.size() == 0 || OB_ISNULL(hit[0])) {
      ret = OB_INVALID_DATA;
    } else if (hit[0]->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_DATA;
    } else if (hit[0]->get_data_length() == 0) {
      // An empty completion result is not a valid response.
      ret = OB_INVALID_DATA;
    } else {
      result = hit[0];
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::get_header(common::ObIAllocator &allocator,
                                             ObString &api_key,
                                             common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::get_body(common::ObIAllocator &allocator,
                                           common::ObString &model,
                                           common::ObArray<ObString> &contents,
                                           common::ObJsonObject *config,
                                           common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *input_array = nullptr;
    ObString reserved_keys[] = {ObString("model"), ObString("input")};
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, input_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object_except(allocator, config,
                                                                     reserved_keys,
                                                                     ARRAYSIZEOF(reserved_keys),
                                                                     body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOpenAIUtils::ObOpenAIEmbed::parse_output(common::ObIAllocator &allocator, 
                                               common::ObJsonObject *http_response,
                                               common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonNode *data_node = nullptr;
    if (OB_ISNULL(data_node = http_response->get_value("data"))) {
      ret = OB_INVALID_DATA;
    } else if (data_node->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else {
      ObJsonArray *data_array = static_cast<ObJsonArray *>(data_node);
      const int64_t count = static_cast<int64_t>(data_array->element_count());
      bool has_index = false;
      bool missing_index = false;
      // Validate every item first so that a malformed response never produces
      // a partially filled result.
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        ObJsonNode *item = data_array->get_value(i);
        if (OB_ISNULL(item) || item->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
        } else {
          ObJsonObject *item_obj = static_cast<ObJsonObject *>(item);
          ObJsonNode *index_node = item_obj->get_value("index");
          if (OB_ISNULL(index_node)) {
            missing_index = true;
          } else {
            has_index = true;
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (count <= 0) {
        ret = OB_INVALID_DATA;
      } else if (has_index && missing_index) {
        // Cannot restore the input order when only part of the entries carry
        // an index, treat it as a protocol violation.
        ret = OB_INVALID_DATA;
      } else {
        ObJsonArray *result_array = nullptr;
        ObJsonNode **slots = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
        } else if (has_index &&
                   OB_ISNULL(slots = static_cast<ObJsonNode **>(
                       allocator.alloc(sizeof(ObJsonNode *) * count)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (has_index) {
          MEMSET(slots, 0, sizeof(ObJsonNode *) * count);
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
          ObJsonObject *item_obj = static_cast<ObJsonObject *>(data_array->get_value(i));
          ObJsonNode *embedding_node = item_obj->get_value("embedding");
          int64_t actual_dimension = 0;
          if (OB_ISNULL(embedding_node)) {
            ret = OB_INVALID_DATA;
          } else if (OB_FAIL(ObAIFuncUtils::check_embedding_vector(allocator,
                                                                   embedding_node,
                                                                   0,
                                                                   actual_dimension))) {
            ret = OB_INVALID_DATA;
          } else if (!has_index) {
            if (OB_FAIL(result_array->append(embedding_node))) {
            }
          } else {
            ObJsonNode *index_node = item_obj->get_value("index");
            int64_t index = -1;
            if (OB_ISNULL(index_node)) {
              ret = OB_INVALID_DATA;
            } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
              index = index_node->get_int();
            } else if (index_node->json_type() == ObJsonNodeType::J_UINT) {
              index = static_cast<int64_t>(index_node->get_uint());
            } else {
              ret = OB_INVALID_DATA;
            }
            if (OB_FAIL(ret)) {
            } else if (index < 0 || index >= count || OB_NOT_NULL(slots[index])) {
              ret = OB_INVALID_DATA;
            } else {
              slots[index] = embedding_node;
            }
          }
        }
        if (OB_SUCC(ret) && has_index) {
          // Build the result in input order using the server provided index.
          for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
            if (OB_ISNULL(slots[i])) {
              ret = OB_INVALID_DATA;
            } else if (OB_FAIL(result_array->append(slots[i]))) {
            }
          }
        }
        if (OB_SUCC(ret)) {
          result = result_array;
        }
      }
    }
  }
  return ret;
}

int ObOllamaUtils::get_header(common::ObIAllocator &allocator,
                              common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  // Ollama does not authenticate requests with an API key, but an explicit
  // content type is still required by the native json protocol.
  ObString content_type_str("Content-Type: application/json");
  ObString content_type_c_str;
  if (OB_FAIL(ob_write_string(allocator, content_type_str, content_type_c_str, true))) {
  } else if (OB_FAIL(headers.push_back(content_type_c_str))) {
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::get_header(common::ObIAllocator &allocator,
                                                common::ObString &api_key,
                                                common::ObArray<ObString> &headers) 
{
  UNUSED(api_key);
  return ObOllamaUtils::get_header(allocator, headers);
}

int ObOllamaUtils::ObOllamaComplete::get_body(common::ObIAllocator &allocator,
                                              common::ObString &model,
                                              common::ObString &prompt,
                                              common::ObString &content,
                                              common::ObJsonObject *config,
                                              common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || content.empty()) { 
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "llama3.1", "prompt": "What is the capital of France?", "stream": false}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonString *content_str = nullptr;
    ObJsonBoolean *stream_bool = nullptr;
    ObJsonObject *options_obj = nullptr;
    // The native protocol accepts one prompt only, fold the optional system
    // prompt into it when present.
    ObString prompt_str;
    if (prompt.empty()) {
      prompt_str = content;
    } else {
      ObStringBuffer prompt_buf(&allocator);
      if (OB_FAIL(prompt_buf.append(prompt))) {
      } else if (OB_FAIL(prompt_buf.append("\n\n"))) {
      } else if (OB_FAIL(prompt_buf.append(content))) {
      } else {
        prompt_str = prompt_buf.string();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, prompt_str, content_str))) {
    } else if (OB_FAIL(body_obj->add("prompt", content_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, false, stream_bool))) {
    } else if (OB_FAIL(body_obj->add("stream", stream_bool))) {
    } else if (OB_FAIL(ObOllamaUtils::build_options(allocator, config, body_obj, options_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::set_config_json_format(common::ObIAllocator &allocator, common::ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"format": "json"}
    ObString json_str("json");
    ObJsonString *json_str_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, json_str, json_str_obj))) {
    } else if (OB_FAIL(config->add("format", json_str_obj))) {
    } 
  }
  return ret;
}

int ObOllamaUtils::ObOllamaComplete::parse_output(common::ObIAllocator &allocator,
                                                  common::ObJsonObject *http_response,
                                                  common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIJsonBase *j_tree = http_response;
    common::ObString path_text("$.response");
    ObJsonPath j_path(path_text, &allocator);
    ObJsonSeekResult hit;
    if (OB_FAIL(j_path.parse_path())) {
    } else if (OB_FAIL(j_tree->seek(j_path, j_path.path_node_cnt(), false, false, hit))) {
    } else if (hit.size() == 0 || OB_ISNULL(hit[0])) {
      ret = OB_INVALID_DATA;
    } else if (hit[0]->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_DATA;
    } else if (hit[0]->get_data_length() == 0) {
      ret = OB_INVALID_DATA;
    } else {
      result = hit[0];
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaEmbed::get_header(common::ObIAllocator &allocator,
                                             common::ObString &api_key,
                                             common::ObArray<ObString> &headers) 
{
  UNUSED(api_key);
  return ObOllamaUtils::get_header(allocator, headers);
}

int ObOllamaUtils::ObOllamaEmbed::get_body(common::ObIAllocator &allocator,
                                           common::ObString &model,
                                           common::ObArray<ObString> &contents,
                                           common::ObJsonObject *config,
                                           common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (model.empty() || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "input": ["*"], "options": {...}}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonArray *input_array = nullptr;
    ObJsonObject *options_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, input_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_array))) {
    } else if (OB_FAIL(ObOllamaUtils::build_options(allocator, config, body_obj, options_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObOllamaUtils::ObOllamaEmbed::parse_output(common::ObIAllocator &allocator,
                                               common::ObJsonObject *http_response,
                                               common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIJsonBase *j_tree = http_response;
    common::ObString path_text("$.embeddings");
    ObJsonPath j_path(path_text, &allocator);
    ObJsonSeekResult hit;
    if (OB_FAIL(j_path.parse_path())) {
    } else if (OB_FAIL(j_tree->seek(j_path, j_path.path_node_cnt(), false, false, hit))) {
    } else if (hit.size() == 0 || OB_ISNULL(hit[0])) {
      ret = OB_INVALID_DATA;
    } else if (hit[0]->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else {
      // Ollama returns embeddings positionally, so the response order must be
      // kept as is.
      ObJsonArray *embedding_array = static_cast<ObJsonArray *>(hit[0]);
      if (embedding_array->element_count() == 0) {
        ret = OB_INVALID_DATA;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < embedding_array->element_count(); i++) {
        ObJsonNode *embedding_node = embedding_array->get_value(i);
        int64_t actual_dimension = 0;
        if (OB_ISNULL(embedding_node)) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(ObAIFuncUtils::check_embedding_vector(allocator,
                                                                 embedding_node,
                                                                 0,
                                                                 actual_dimension))) {
          ret = OB_INVALID_DATA;
        }
      }
      if (OB_SUCC(ret)) {
        result = embedding_array;
      }
    }
  }
  return ret;
}

int ObOllamaUtils::build_options(common::ObIAllocator &allocator,
                                 common::ObJsonObject *config,
                                 common::ObJsonObject *body,
                                 common::ObJsonObject *&options)
{
  int ret = OB_SUCCESS;
  options = nullptr;
  if (OB_ISNULL(body)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_NOT_NULL(config) && config->element_count() > 0) {
    ObJsonObject *options_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, options_obj))) {
    } else {
      const int64_t count = static_cast<int64_t>(config->element_count());
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        ObString key;
        ObJsonNode *value = config->get_value(i);
        if (OB_ISNULL(value)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(config->get_key(i, key))) {
        } else if (key.case_compare("model") == 0
                   || key.case_compare("prompt") == 0
                   || key.case_compare("input") == 0
                   || key.case_compare("stream") == 0
                   || key.case_compare("dimensions") == 0) {
          // reserved or unsupported by the native protocol
        } else if (key.case_compare("options") == 0) {
          if (value->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ObJsonObject *user_options = static_cast<ObJsonObject *>(value);
            for (int64_t j = 0; OB_SUCC(ret) && j < user_options->element_count(); j++) {
              ObString option_key;
              ObJsonNode *option_value = user_options->get_value(j);
              if (OB_ISNULL(option_value)) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_FAIL(user_options->get_key(j, option_key))) {
              } else if (OB_FAIL(options_obj->add(option_key, option_value))) {
              }
            }
          }
        } else if (key.case_compare("format") == 0) {
          if (OB_FAIL(body->add("format", value))) {
          }
        } else if (key.case_compare("response_format") == 0) {
          // OpenAI style {"response_format": {"type": "json_object"}} maps to
          // the native {"format": "json"}.
          if (value->json_type() == ObJsonNodeType::J_OBJECT) {
            ObJsonNode *type_node = static_cast<ObJsonObject *>(value)->get_value("type");
            if (OB_NOT_NULL(type_node)
                && type_node->json_type() == ObJsonNodeType::J_STRING) {
              ObString type_str(type_node->get_data_length(), type_node->get_data());
              if (type_str.case_compare("json_object") == 0) {
                ObJsonString *format_str = nullptr;
                ObString json_value("json");
                if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, json_value, format_str))) {
                } else if (OB_FAIL(body->add("format", format_str))) {
                }
              }
            }
          }
        } else if (key.case_compare("max_tokens") == 0) {
          if (OB_FAIL(options_obj->add("num_predict", value))) {
          }
        } else {
          if (OB_FAIL(options_obj->add(key, value))) {
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (options_obj->element_count() > 0
                 && OB_FAIL(body->add("options", options_obj))) {
      } else {
        options = options_obj;
      }
    }
  }
  return ret;
}

int ObDashscopeUtils::get_header(common::ObIAllocator &allocator,
                                 common::ObString &api_key,
                                 common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::get_header(common::ObIAllocator &allocator, 
                                                      common::ObString &api_key,
                                                      common::ObArray<ObString> &headers) 
{
  return ObOpenAIUtils::get_header(allocator, api_key, headers);
}

int ObDashscopeUtils::ObDashscopeComplete::get_body(common::ObIAllocator &allocator,
                                                    common::ObString &model,
                                                    common::ObString &prompt,
                                                    common::ObString &content,
                                                    common::ObJsonObject *config,
                                                    common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(content)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "input": {"messages": [{"role": "system", "content": "*"}, {"role": "user", "content": "*"}]}, "parameters": {}}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;
    ObJsonObject *parameters_obj = nullptr;
    ObString reserved_keys[] = {ObString("model"), ObString("input"), ObString("stream")};
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, parameters_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object_except(allocator, config,
                                                                     reserved_keys,
                                                                     ARRAYSIZEOF(reserved_keys),
                                                                     parameters_obj))) {
    } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeComplete::set_config_result_format(allocator, parameters_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeComplete::construct_input_obj(allocator, prompt, content, input_obj))) {
    } else if (OB_FAIL(body_obj->add("input", input_obj))) {
    } else if (OB_FAIL(body_obj->add("parameters", parameters_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::set_config_result_format(ObIAllocator &allocator, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_NOT_NULL(config->get_value("result_format"))) {
    if (OB_FAIL(config->remove("result_format"))) {
    }
  } 
  if (OB_SUCC(ret)) {
    // {"result_format": "message"}
    ObJsonString *result_format_str = nullptr;
    ObString result_format_str_val("message");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, result_format_str_val, result_format_str))) {
    } else if (OB_FAIL(config->add("result_format", result_format_str))) {
    }
  } 
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::construct_input_obj(ObIAllocator &allocator, ObString &prompt, ObString &content, ObJsonObject *&input_obj)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(content)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *obj = nullptr;
    ObJsonArray *messages_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
    } else if (OB_FAIL(ObOpenAIUtils::ObOpenAIComplete::construct_messages_array(allocator, prompt, content, messages_array))) {
    } else if (OB_FAIL(obj->add("messages", messages_array))) {
    } else {
      input_obj = obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::set_config_json_format(ObIAllocator &allocator, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(config)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    //{"type": "json_object"}
    ObJsonString *type_str = nullptr;
    ObString type_str_val("json_object");
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, type_str_val, type_str))) {
    } else if (OB_FAIL(config->add("type", type_str))) {
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeComplete::parse_output(ObIAllocator &allocator, ObJsonObject *http_response, ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonNode *output_node = nullptr;
    ObJsonObject *output_obj = nullptr;
    ObJsonNode *choices_node = nullptr;
    ObJsonArray *choices_array = nullptr;
    ObJsonNode *choice_node = nullptr;
    ObJsonObject *choice_obj = nullptr;
    ObJsonNode *message_node = nullptr;
    ObJsonObject *message_obj = nullptr;
    ObJsonNode *content_node = nullptr;
    if (OB_ISNULL(output_node = http_response->get_value("output"))
        || output_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(choices_node = static_cast<ObJsonObject *>(output_node)->get_value("choices"))
               || choices_node->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else if ((choices_array = static_cast<ObJsonArray *>(choices_node))->element_count() == 0) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(choice_node = choices_array->get_value(0))
               || choice_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(message_node = (choice_obj = static_cast<ObJsonObject *>(choice_node))->get_value("message"))
               || message_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(content_node = (message_obj = static_cast<ObJsonObject *>(message_node))->get_value("content"))) {
      ret = OB_INVALID_DATA;
    } else if (content_node->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_DATA;
    } else if (content_node->get_data_length() == 0) {
      ret = OB_INVALID_DATA;
    } else {
      result = content_node;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeEmbed::get_header(common::ObIAllocator &allocator,
                                                   common::ObString &api_key,
                                                   common::ObArray<ObString> &headers) 
{
  return ObDashscopeUtils::get_header(allocator, api_key, headers);
}

int ObDashscopeUtils::ObDashscopeEmbed::get_body(common::ObIAllocator &allocator,
                                                 common::ObString &model,
                                                 common::ObArray<ObString> &contents,
                                                 common::ObJsonObject *config,
                                                 common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"model": "*", "input": {"texts": ["*"]}, "parameters": {}}
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;
    ObJsonArray *texts_array = nullptr;
    ObJsonObject *parameters_obj = nullptr;
    ObString reserved_keys[] = {ObString("model"), ObString("input")};
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, input_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::transform_array_to_json_array(allocator, contents, texts_array))) {
    } else if (OB_FAIL(input_obj->add("texts", texts_array))) {
    } else if (OB_FAIL(body_obj->add("input", input_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, parameters_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object_except(allocator, config,
                                                                     reserved_keys,
                                                                     ARRAYSIZEOF(reserved_keys),
                                                                     parameters_obj))) {
    } else if (OB_NOT_NULL(parameters_obj->get_value("dimensions"))
               && OB_FAIL(parameters_obj->rename_key("dimensions", "dimension"))) {
    } else if (parameters_obj->element_count() > 0
               && OB_FAIL(body_obj->add("parameters", parameters_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeEmbed::parse_output(common::ObIAllocator &allocator,
                                                    common::ObJsonObject *http_response,
                                                    common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // {"output": {"embeddings": [{"embedding": ["*"]}]}}
    ObJsonObject *output_obj = nullptr;
    ObJsonNode *embeddings_node = nullptr;
    ObJsonArray *embeddings_array = nullptr;
    ObJsonArray *result_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
    } else if (OB_ISNULL(embeddings_node = http_response->get_value("output"))
               || embeddings_node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(embeddings_node = (output_obj = static_cast<ObJsonObject *>(embeddings_node))->get_value("embeddings"))
               || embeddings_node->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else if ((embeddings_array = static_cast<ObJsonArray *>(embeddings_node))->element_count() == 0) {
      ret = OB_INVALID_DATA;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < embeddings_array->element_count(); ++i) {
        ObJsonNode *embedding_item = embeddings_array->get_value(i);
        ObJsonNode *embedding_node = nullptr;
        int64_t actual_dimension = 0;
        if (OB_ISNULL(embedding_item) || embedding_item->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
        } else if (OB_ISNULL(embedding_node = static_cast<ObJsonObject *>(embedding_item)->get_value("embedding"))) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(ObAIFuncUtils::check_embedding_vector(allocator,
                                                                 embedding_node,
                                                                 0,
                                                                 actual_dimension))) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(result_array->append(embedding_node))) {
        }
      }
      if (OB_SUCC(ret)) {
        result = result_array;
      }
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_header(common::ObIAllocator &allocator,
                                                    common::ObString &api_key,
                                                    common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_body(common::ObIAllocator &allocator,
                                                  common::ObString &model,
                                                  common::ObString &query,
                                                  common::ObJsonArray *document_array,
                                                  common::ObJsonObject *config,
                                                  common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonObject *input_obj = nullptr;
    ObJsonObject *parameters_obj = nullptr;
    ObString reserved_keys[] = {ObString("model"), ObString("input"), ObString("query"),
                                ObString("documents")};
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObDashscopeUtils::ObDashscopeRerank::get_input_obj(allocator, query, document_array, input_obj))) {
    } else if (OB_FAIL(body_obj->add("input", input_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, parameters_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object_except(allocator, config,
                                                                     reserved_keys,
                                                                     ARRAYSIZEOF(reserved_keys),
                                                                     parameters_obj))) {
    } else if (parameters_obj->element_count() > 0
               && OB_FAIL(body_obj->add("parameters", parameters_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::get_input_obj(common::ObIAllocator &allocator,
                                                      common::ObString &query,
                                                      common::ObJsonArray *document_array,
                                                      common::ObJsonObject *&input_obj) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *obj = nullptr;
    ObJsonString *query_str = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, query, query_str))) {
    } else if (OB_FAIL(obj->add("query", query_str))) {
    } else if (OB_FAIL(obj->add("documents", document_array))) {
    } else {
      input_obj = obj;
    }
  }
  return ret;
}

int ObDashscopeUtils::ObDashscopeRerank::parse_output(common::ObIAllocator &allocator,
                                                      common::ObJsonObject *http_response,
                                                      common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObJsonObject *output_obj = nullptr;
  ObJsonArray *results_array = nullptr;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(output_obj = static_cast<ObJsonObject *>(http_response->get_value("output")))) {
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(results_array = static_cast<ObJsonArray *>(output_obj->get_value("results")))) {
    ret = OB_INVALID_DATA;
  } else {
    result = results_array;
  }
  return ret;
}

int ObSiliconflowUtils::get_header(common::ObIAllocator &allocator,
                                 common::ObString &api_key,
                                 common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::get_header(common::ObIAllocator &allocator,
                                                    common::ObString &api_key,
                                                    common::ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOpenAIUtils::get_header(allocator, api_key, headers))) {
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::get_body(common::ObIAllocator &allocator,
                                                  common::ObString &model,
                                                  common::ObString &query,
                                                  common::ObJsonArray *document_array,
                                                  common::ObJsonObject *config,
                                                  common::ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model) || OB_ISNULL(query) || OB_ISNULL(document_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonObject *body_obj = nullptr;
    ObJsonString *model_str = nullptr;
    ObJsonString *query_str = nullptr;
    ObString reserved_keys[] = {ObString("model"), ObString("query"), ObString("documents")};
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, body_obj))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, model, model_str))) {
    } else if (OB_FAIL(body_obj->add("model", model_str))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, query, query_str))) {
    } else if (OB_FAIL(body_obj->add("query", query_str))) {
    } else if (OB_FAIL(body_obj->add("documents", document_array))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::compact_json_object_except(allocator, config,
                                                                     reserved_keys,
                                                                     ARRAYSIZEOF(reserved_keys),
                                                                     body_obj))) {
    } else {
      body = body_obj;
    }
  }
  return ret;
}

int ObSiliconflowUtils::ObSiliconflowRerank::parse_output(common::ObIAllocator &allocator,
                                                      common::ObJsonObject *http_response,
                                                      common::ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObJsonArray *results_array = nullptr;
  if (OB_ISNULL(http_response)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(results_array = static_cast<ObJsonArray *>(http_response->get_value("results")))) {
    ret = OB_INVALID_DATA;
  } else {
    result = results_array;
  }
  return ret;
}


int ObAIFuncUtils::get_header(ObIAllocator &allocator, 
                              const ObAIFuncExprInfo &info,
                              const share::ObAiModelEndpointInfo &endpoint_info,
                              ObArray<ObString> &headers) 
{
  int ret = OB_SUCCESS;
  ObString unencrypted_access_key;
  if (OB_FAIL(endpoint_info.get_unencrypted_access_key(allocator, unencrypted_access_key))) {
  } else if (ObAIFuncUtils::is_completion_type(&info)) {
    ObAIFuncIComplete *complete_provider = nullptr;
    if (OB_FAIL(get_complete_provider(allocator, endpoint_info.get_provider(), complete_provider))) {
    } else if (OB_FAIL(complete_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  } else if (ObAIFuncUtils::is_dense_embedding_type(&info)) {
    ObAIFuncIEmbed *embed_provider = nullptr;
    if (OB_FAIL(get_embed_provider(allocator, endpoint_info.get_provider(), embed_provider))) {
    } else if (OB_FAIL(embed_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  } else if (ObAIFuncUtils::is_rerank_type(&info)) {
    ObAIFuncIRerank *rerank_provider = nullptr;
    if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
    } else if (OB_FAIL(rerank_provider->get_header(allocator, unencrypted_access_key, headers))) {
    }
  }
  return ret;
}

int ObAIFuncUtils::get_complete_body(ObIAllocator &allocator, 
                                    const ObAIFuncExprInfo &info, 
                                    const share::ObAiModelEndpointInfo &endpoint_info,
                                    ObString &prompt, 
                                    ObString &content, 
                                    ObJsonObject *config,
                                    ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, endpoint_info.get_provider(), complete_provider))) {
  } else if (OB_FAIL(complete_provider->get_body(allocator, request_model_name, prompt, content, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::set_json_format_config(ObIAllocator &allocator, const ObString &provider, ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, provider, complete_provider))) {
  } else if (OB_FAIL(complete_provider->set_config_json_format(allocator, config))) {
  } 
  return ret;
}

int ObAIFuncUtils::get_embed_body(ObIAllocator &allocator, 
                                  const ObAIFuncExprInfo &info, 
                                  const share::ObAiModelEndpointInfo &endpoint_info,
                                  ObArray<ObString> &contents, 
                                  ObJsonObject *config,
                                  ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIEmbed *embed_provider = nullptr;
  if (OB_FAIL(get_embed_provider(allocator, endpoint_info.get_provider(), embed_provider))) {
  } else if (OB_FAIL(embed_provider->get_body(allocator, request_model_name, contents, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::get_rerank_body(ObIAllocator &allocator, 
                                   const ObAIFuncExprInfo &info,
                                   const share::ObAiModelEndpointInfo &endpoint_info,
                                   ObString &query,
                                   ObJsonArray *document_array,
                                   ObJsonObject *config,
                                   ObJsonObject *&body) 
{
  int ret = OB_SUCCESS;
  ObString request_model_name = info.model_;
  if (!endpoint_info.get_request_model_name().empty()) {
    request_model_name = endpoint_info.get_request_model_name();
  }

  ObAIFuncIRerank *rerank_provider = nullptr;
  if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(rerank_provider->get_body(allocator, request_model_name, query, document_array, config, body))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_complete_output(ObIAllocator &allocator, 
                                        const share::ObAiModelEndpointInfo &endpoint_info,
                                        ObJsonObject *http_response,
                                        ObIJsonBase *&result)                                                 
{
  int ret = OB_SUCCESS;
  ObAIFuncIComplete *complete_provider = nullptr;
  if (OB_FAIL(get_complete_provider(allocator, endpoint_info.get_provider(), complete_provider))) {
  } else if (OB_FAIL(complete_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_embed_output(ObIAllocator &allocator, 
                                      const share::ObAiModelEndpointInfo &endpoint_info,
                                      ObJsonObject *http_response,
                                      ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObAIFuncIEmbed *embed_provider = nullptr;
  if (OB_FAIL(get_embed_provider(allocator, endpoint_info.get_provider(), embed_provider))) {
  } else if (OB_FAIL(embed_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncUtils::parse_rerank_output(ObIAllocator &allocator, 
                                       const share::ObAiModelEndpointInfo &endpoint_info,
                                       ObJsonObject *http_response,
                                       ObIJsonBase *&result) 
{
  int ret = OB_SUCCESS;
  ObAIFuncIRerank *rerank_provider = nullptr;
  if (OB_FAIL(get_rerank_provider(allocator, endpoint_info.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(rerank_provider->parse_output(allocator, http_response, result))) {
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_object(ObIAllocator &allocator, ObJsonObject *&obj_node)
{
  int ret = OB_SUCCESS;
  ObJsonObject *j_obj = OB_NEWx(ObJsonObject, &allocator, &allocator);
  if (OB_ISNULL(j_obj)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    obj_node = j_obj;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_array(ObIAllocator &allocator, ObJsonArray *&array_node)
{
  int ret = OB_SUCCESS;
  ObJsonArray *j_array = OB_NEWx(ObJsonArray, &allocator, &allocator);
  if (OB_ISNULL(j_array)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    array_node = j_array; 
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_string(ObIAllocator &allocator, ObString &str, ObJsonString *&str_node)
{
  int ret = OB_SUCCESS;
  ObJsonString *j_str = OB_NEWx(ObJsonString, &allocator,str);
  if (OB_ISNULL(j_str)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    str_node = j_str;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_int(ObIAllocator &allocator, int64_t num, ObJsonInt *&int_node)
{
  int ret = OB_SUCCESS;
  ObJsonInt *j_int = OB_NEWx(ObJsonInt, &allocator, num);
  if (OB_ISNULL(j_int)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int_node = j_int;
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_boolean(ObIAllocator &allocator, bool value, ObJsonBoolean *&boolean_node)
{
  int ret = OB_SUCCESS;
  ObJsonBoolean *j_bool = OB_NEWx(ObJsonBoolean, &allocator, value);
  if (OB_ISNULL(j_bool)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    boolean_node = j_bool;
  }
  return ret;
}

int ObAIFuncJsonUtils::print_json_to_str(ObIAllocator &allocator, ObIJsonBase *base_node, ObString &str)
{
  int ret = OB_SUCCESS;
  ObJsonBuffer j_buf(&allocator);
  if (OB_FAIL(base_node->print(j_buf, 0))) {
  } else {
    str = j_buf.string();
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_object_form_str(ObIAllocator &allocator, ObString &str, ObJsonObject *&obj_node)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = NULL;
  obj_node = NULL;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, str, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base))) {
  } else if (OB_ISNULL(j_base)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (j_base->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    obj_node = static_cast<ObJsonObject *>(j_base);
  }
  return ret;
}

bool ObAIFuncJsonUtils::is_reserved_json_key(const ObString &key,
                                             const ObString *keys,
                                             const int64_t count)
{
  bool found = false;
  for (int64_t i = 0; !found && OB_NOT_NULL(keys) && i < count; i++) {
    if (key.case_compare(keys[i]) == 0) {
      found = true;
    }
  }
  return found;
}

int ObAIFuncJsonUtils::compact_json_object(ObIAllocator &allocator, ObJsonObject *obj_node, ObJsonObject *compact_obj)
{
  return compact_json_object_except(allocator, obj_node, nullptr, 0, compact_obj);
}

int ObAIFuncJsonUtils::compact_json_object_except(ObIAllocator &allocator,
                                                  ObJsonObject *obj_node,
                                                  const ObString *excluded_keys,
                                                  const int64_t excluded_count,
                                                  ObJsonObject *compact_obj)
{
  UNUSED(allocator);
  int ret = OB_SUCCESS;
  // add all members of obj_node to compact_obj except reserved keys
  if (OB_ISNULL(compact_obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(obj_node)) {
    // do nothing
  } else {
    ObString key;
    int64_t count = obj_node->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *j_node = obj_node->get_value(i);
      if (OB_ISNULL(j_node)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(obj_node->get_key(i, key))) {
      } else if (is_reserved_json_key(key, excluded_keys, excluded_count)) {
        // do not let the user config overwrite the request fields we build
      } else if (OB_FAIL(compact_obj->add(key, j_node))) {
      }
    }
  }
  return ret;
}

int ObAIFuncJsonUtils::get_json_number_value(ObIAllocator &allocator,
                                             const ObIJsonBase *node,
                                             double &value)
{
  int ret = OB_SUCCESS;
  value = 0.0;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    switch (node->json_type()) {
      case ObJsonNodeType::J_INT:
        value = static_cast<double>(node->get_int());
        break;
      case ObJsonNodeType::J_UINT:
        value = static_cast<double>(node->get_uint());
        break;
      case ObJsonNodeType::J_DOUBLE:
        value = node->get_double();
        break;
      case ObJsonNodeType::J_DECIMAL: {
        share::ObJsonReaderHelper json_reader(allocator);
        float float_value = 0.0;
        if (OB_FAIL(json_reader.get_float_value(node, float_value))) {
        } else {
          value = static_cast<double>(float_value);
        }
        break;
      }
      default:
        ret = OB_INVALID_ARGUMENT;
        break;
    }
    if (OB_SUCC(ret) && !std::isfinite(value)) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int ObAIFuncPromptUtils::replace_meta_prompt(ObIAllocator &allocator, ObString &meta_prompt, ObString &key, ObString &content, ObString &result)
{
  int ret = OB_SUCCESS;
  if (meta_prompt.empty() || key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *key_pos = nullptr;
    if (OB_ISNULL(key_pos = STRSTR(meta_prompt.ptr(), key.ptr()))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int64_t before_key_len = key_pos - meta_prompt.ptr();
      int64_t after_key_len = meta_prompt.length() - before_key_len - key.length();
      int64_t new_len = before_key_len + content.length() + after_key_len;
      char *new_str = static_cast<char *>(allocator.alloc(new_len));
      if (OB_ISNULL(new_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(new_str, meta_prompt.ptr(), before_key_len);
        MEMCPY(new_str + before_key_len, content.ptr(), content.length());
        MEMCPY(new_str + before_key_len + content.length(), key_pos + key.length(), after_key_len);
        result.assign_ptr(new_str, static_cast<ObString::obstr_size_t>(new_len));
      }
    }
  }
  return ret;
}

int ObAIFuncJsonUtils::transform_array_to_json_array(ObIAllocator &allocator, ObArray<ObString> &contents, ObJsonArray *&array)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(array = OB_NEWx(ObJsonArray, &allocator, &allocator))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      ObString content = contents[i];
      ObJsonString *j_str = OB_NEWx(ObJsonString, &allocator, content);
      if (OB_ISNULL(j_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        array->append(j_str);
      }
    }
  }
  return ret;
}

int ObAIFuncUtils::get_complete_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIComplete *&complete_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::ALIYUN) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::DEEPSEEK)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)
      || ob_provider_check(provider, ObAIFuncProviderUtils::HUNYUAN)) {
    complete_provider = OB_NEWx(ObOpenAIUtils::ObOpenAIComplete, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    complete_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeComplete, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OLLAMA)) {
    complete_provider = OB_NEWx(ObOllamaUtils::ObOllamaComplete, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(complete_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::get_embed_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIEmbed *&embed_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI) 
      || ob_provider_check(provider, ObAIFuncProviderUtils::ALIYUN)
      || ob_provider_check(provider, ObAIFuncProviderUtils::HUNYUAN)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)) {
    embed_provider = OB_NEWx(ObOpenAIUtils::ObOpenAIEmbed, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    embed_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeEmbed, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::OLLAMA)) {
    embed_provider = OB_NEWx(ObOllamaUtils::ObOllamaEmbed, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(embed_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::get_rerank_provider(ObIAllocator &allocator, const ObString &provider, ObAIFuncIRerank *&rerank_provider)
{
  int ret = OB_SUCCESS;
  if (provider.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, provider is empty");
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW)
          || ob_provider_check(provider, ObAIFuncProviderUtils::COHERE)) {
    rerank_provider = OB_NEWx(ObSiliconflowUtils::ObSiliconflowRerank, &allocator);
  } else if (ob_provider_check(provider, ObAIFuncProviderUtils::DASHSCOPE)) {
    rerank_provider = OB_NEWx(ObDashscopeUtils::ObDashscopeRerank, &allocator);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "rerank support siliconflow, cohere and ailiyun-dashscope, this provider current is");
  } 
  if (OB_SUCC(ret) && OB_ISNULL(rerank_provider)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_completion(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info is null");
  } else if (!is_completion_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_dense_embedding(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info is null");
  } else if (!is_dense_embedding_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  }
  return ret;
}

int ObAIFuncUtils::check_info_type_rerank(const ObAIFuncExprInfo *info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info is null");
  } else if (!is_rerank_type(info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info type is not rerank");
  }
  return ret;
}

int ObAIFuncUtils::set_string_result(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res, ObString &res_str)
{
  int ret = OB_SUCCESS;
  ObTextStringDatumResult text_result(expr.datum_meta_.type_, &expr, &ctx, &res);
  int64_t res_len = res_str.length();
  if (OB_FAIL(text_result.init(res_len))) {
  } else if (OB_FAIL(text_result.append(res_str))) {
  } else {
    text_result.set_result();
  }
  return ret;
}

int ObAIFuncUtils::get_ai_func_info(ObIAllocator &allocator, const ObString &model_id,
                                    share::schema::ObSchemaGetterGuard &guard,
                                    ObAIFuncExprInfo *&info)
{
  int ret = OB_SUCCESS;
  if (model_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_function, model_id is empty");
  } else {
    ObAIFuncExprInfo *info_obj = OB_NEWx(ObAIFuncExprInfo, (&allocator), allocator, T_FUN_SYS_AI_COMPLETE);
    if (OB_ISNULL(info_obj)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(info_obj->init(allocator, model_id, guard))) {
    } else {
      info = info_obj;
    }
  }
  return ret;
}

bool ObAIFuncUtils::is_provider_support_base64(const ObString &provider)
{
  return ob_provider_check(provider, ObAIFuncProviderUtils::OPENAI)
      || ob_provider_check(provider, ObAIFuncProviderUtils::SILICONFLOW);
}

int ObAIFuncUtils::check_embedding_vector(ObIAllocator &allocator,
                                          const ObIJsonBase *embedding,
                                          const int64_t expected_dimension,
                                          int64_t &actual_dimension)
{
  int ret = OB_SUCCESS;
  actual_dimension = 0;
  if (OB_ISNULL(embedding)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (embedding->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_ARGUMENT;
  } else if (static_cast<int64_t>(embedding->element_count()) <= 0) {
    // An empty vector is never a valid embedding.
    ret = OB_INVALID_DATA;
  } else {
    const int64_t dimension = static_cast<int64_t>(embedding->element_count());
    for (int64_t i = 0; OB_SUCC(ret) && i < dimension; i++) {
      ObIJsonBase *element = nullptr;
      double value = 0.0;
      if (OB_FAIL(embedding->get_array_element(static_cast<uint64_t>(i), element))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(allocator, element, value))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (expected_dimension > 0 && dimension != expected_dimension) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, result dimension is not equal to the requested dimension");
      } else {
        actual_dimension = dimension;
      }
    }
  }
  return ret;
}

int ObAIFuncUtils::check_complete_config(ObJsonObject *config)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(config)) {
    ObJsonNode *temperature_node = config->get_value("temperature");
    ObJsonNode *max_tokens_node = config->get_value("max_tokens");
    if (OB_NOT_NULL(temperature_node)) {
      ObArenaAllocator tmp_allocator;
      double temperature = 0.0;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(tmp_allocator, temperature_node, temperature))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, temperature must be a number between 0 and 2");
      } else if (temperature < 0.0 || temperature > 2.0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, temperature must be a number between 0 and 2");
      }
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(max_tokens_node)) {
      int64_t max_tokens = 0;
      if (max_tokens_node->json_type() == ObJsonNodeType::J_INT) {
        max_tokens = max_tokens_node->get_int();
      } else if (max_tokens_node->json_type() == ObJsonNodeType::J_UINT) {
        max_tokens = static_cast<int64_t>(max_tokens_node->get_uint());
      } else {
        ret = OB_INVALID_ARGUMENT;
      }
      if (OB_FAIL(ret) || max_tokens <= 0 || max_tokens > INT32_MAX) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, max_tokens must be a positive integer");
      }
    }
  }
  return ret;
}

int ObAIFuncUtils::decode_base64_embedding_array(const ObIJsonBase &embedding_jbase,
                                                 ObIAllocator &allocator,
                                                 const int64_t dimension,
                                                 float *&vector)
{
  int ret = OB_SUCCESS;
  if (embedding_jbase.json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *encoded_embedding = embedding_jbase.get_data();
    uint64_t encoded_embedding_len = embedding_jbase.get_data_length();
    uint64_t decoded_buf_len = ObBase64Encoder::needed_decoded_length(encoded_embedding_len);
    uint8_t *decoded_buf = nullptr;
    int64_t pos = 0;
    if (decoded_buf_len <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(decoded_buf = static_cast<uint8_t *>(allocator.alloc(decoded_buf_len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(ObBase64Encoder::decode(encoded_embedding, encoded_embedding_len,
                                               decoded_buf, decoded_buf_len, pos))) {
    } else if (pos != dimension * sizeof(float)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      vector = reinterpret_cast<float *>(decoded_buf);
    }
  }
  return ret;
}

int ObAIFuncUtils::decode_float_embedding_array(const ObIJsonBase &embedding_jbase,
                                                ObIAllocator &allocator,
                                                share::ObJsonReaderHelper &json_reader,
                                                const int64_t dimension,
                                                float *&vector)
{
  int ret = OB_SUCCESS;
  float *tmp_vector = nullptr;
  if (!share::ObJsonHelper::is_array_type(&embedding_jbase)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    uint64_t embedding_size = json_reader.get_array_size(&embedding_jbase);
    if (embedding_size != dimension) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      tmp_vector = static_cast<float*>(allocator.alloc(dimension * sizeof(float)));
      if (OB_ISNULL(tmp_vector)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (uint64_t i = 0; i < dimension && OB_SUCC(ret); i++) {
          ObIJsonBase *value = nullptr;
          if (OB_FAIL(json_reader.get_array_element(&embedding_jbase, i, value))) {
          } else if (!share::ObJsonHelper::is_number_type(value)) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            float f_value = 0.0;
            if (OB_FAIL(json_reader.get_float_value(value, f_value))) {
            } else {
              tmp_vector[i] = f_value;
            }
          }
        }
      }
    }
  }

  if (OB_SUCC(ret)) {
    vector = tmp_vector;
  }
  return ret;
}

int ObAIFuncUtils::get_ai_func_info(ObIAllocator &allocator, const ObString &model_id, ObAIFuncExprInfo *&info)
{
  int ret = OB_SUCCESS;
  share::schema::ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  share::schema::ObSchemaGetterGuard guard;
  
  if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(get_ai_func_info(allocator, model_id, guard, info))) {
  }
  return ret;
}

int ObAIFuncModel::call_completion(ObString &prompt, ObJsonObject *config, ObString &result)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIComplete *complete_provider = nullptr;
  ObString prompt_str;
  ObString result_str;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_completion_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(*allocator_, endpoint_info_.get_provider(), complete_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(complete_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(complete_provider->get_body(*allocator_, request_model_name, prompt_str, prompt, config, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(complete_provider->parse_output(*allocator_, response, result_base))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(*allocator_, result_base, result_str))) {
  } else {
    result = result_str;
  }
  if (ret == OB_INVALID_DATA) {
    ObString response_str;
    if (OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %.*s",
               std::min(static_cast<int>(response_str.length()), 512),
               OB_NOT_NULL(response_str.ptr()) ? response_str.ptr() : "");
      ObString ob_http_message_str(http_message_str);
      FORWARD_USER_ERROR(ret, ob_http_message_str.ptr());
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  } 
  return ret;
}

int ObAIFuncModel::call_completion_vector(ObArray<ObString> &prompts, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObArray<ObJsonObject *> body_array;
  ObArray<ObJsonObject *> response_array;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIComplete *complete_provider = nullptr;
  ObString prompt_str;
  ObString result_str;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_completion_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_complete, info type is not completion");
  } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(*allocator_, endpoint_info_.get_provider(), complete_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(complete_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < prompts.count(); i++) {
      ObString prompt = prompts[i];
      if (OB_FAIL(complete_provider->get_body(*allocator_, request_model_name, prompt_str, prompt, config, body))) {
      } else if (OB_FAIL(body_array.push_back(body))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(client.send_post_batch(*allocator_, endpoint_info_.get_url(), headers, body_array, response_array))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < response_array.count(); i++) {
      ObJsonObject *response = response_array[i];
      ObStringBuffer result_buf(allocator_);
      if (OB_FAIL(complete_provider->parse_output(*allocator_, response, result_base))) {
      } else if (OB_FAIL(result_base->print(result_buf, 0))) {
      } else {
        results.push_back(result_buf.string());
      }
    }
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding(ObString &content, ObJsonObject *config, ObString &result)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> contents;
  ObArray<ObString> results;
  if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
  } else if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(contents.push_back(content))) {
  } else if (OB_FAIL(call_dense_embedding_vector_v2(contents, config, results))) {
  } else if (results.count() != 1) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result = results[0];
  }
  return ret;
}

int ObAIFuncModel::call_dense_embedding_vector(ObArray<ObString> &contents, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObArray<ObJsonObject *> body_array;
  ObArray<ObJsonObject *> response_array;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIEmbed *embed_provider = nullptr;
  ObAIFuncClient client;
  ObString request_model_name = get_request_model_name();
  ObString unencrypted_access_key;
  if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(*allocator_, endpoint_info_.get_provider(), embed_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(embed_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
      ObString content = contents[i];
      ObArray<ObString> content_array;
      if (OB_FAIL(content_array.push_back(content))) {
      } else if (OB_FAIL(embed_provider->get_body(*allocator_, request_model_name, content_array, config, body))) {
      } else if (OB_FAIL(body_array.push_back(body))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(client.send_post_batch(*allocator_, endpoint_info_.get_url(), headers, body_array, response_array))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < response_array.count(); i++) {
      ObJsonObject *response = response_array[i];
      ObStringBuffer result_buf(allocator_);
      if (OB_FAIL(embed_provider->parse_output(*allocator_, response, result_base))) {
      } else if (OB_FAIL(result_base->print(result_buf, 0))) {
      } else {
        results.push_back(result_buf.string());
      }
    }
  }
  return ret;
}


int ObAIFuncModel::call_dense_embedding_vector_v2(ObArray<ObString> &content, ObJsonObject *config, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIEmbed *embed_provider = nullptr;
  ObString result_str;
  ObAIFuncClient client;
  int64_t dimension = 0;
  if (OB_NOT_NULL(config)) {
    ObJsonNode *dimension_node = config->get_value("dimensions");
    if (OB_ISNULL(dimension_node)) {
      // do nothing
    } else if (dimension_node->json_type() != ObJsonNodeType::J_INT) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer");
    } else {
      dimension = static_cast<ObJsonInt *>(dimension_node)->get_int();
      if (dimension <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
      }
    }
  }
  
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (OB_FAIL(ret)) {
  } else if (!is_dense_embedding_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
  } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(*allocator_, endpoint_info_.get_provider(), embed_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(embed_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(embed_provider->get_body(*allocator_, request_model_name, content, config, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(embed_provider->parse_output(*allocator_, response, result_base))) {
  } else if (OB_ISNULL(result_base) || result_base->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, embedding result is not an array");
  } else {
    ObJsonArray *result_array = static_cast<ObJsonArray *>(result_base);
    int64_t count = static_cast<int64_t>(result_array->element_count());
    if (content.count() != count || count <= 0) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, embedding result count does not match input count");
    } else {
      int64_t consistent_dimension = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        ObJsonNode *vector_node = result_array->get_value(i);
        int64_t actual_dimension = 0;
        if (OB_ISNULL(vector_node)) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(ObAIFuncUtils::check_embedding_vector(*allocator_,
                                                                 vector_node,
                                                                 dimension,
                                                                 actual_dimension))) {
        } else if (dimension > 0) {
          // already validated against the requested dimension
        } else if (0 == consistent_dimension) {
          consistent_dimension = actual_dimension;
        } else if (consistent_dimension != actual_dimension) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, embedding dimensions in one result are not consistent");
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(*allocator_, vector_node, result_str))) {
          } else if (OB_FAIL(results.push_back(result_str))) {
          }
        }
      }
    }
  }
  if (ret == OB_INVALID_DATA) {
    ObString response_str;
    if (OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %.*s",
               std::min(static_cast<int>(response_str.length()), 512),
               OB_NOT_NULL(response_str.ptr()) ? response_str.ptr() : "");
      ObString ob_http_message_str(http_message_str);
      FORWARD_USER_ERROR(ret, ob_http_message_str.ptr());
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  }
  return ret;
}

int ObAIFuncModel::call_rerank(ObString &query, ObJsonArray *contents, ObJsonArray *&results)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObAIFuncIRerank *rerank_provider = nullptr;
  ObAIFuncClient client;
  ObString unencrypted_access_key;
  ObString request_model_name = get_request_model_name();
  if (!is_rerank_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, info type is not rerank");
  } else if (OB_FAIL(ObAIFuncUtils::get_rerank_provider(*allocator_, endpoint_info_.get_provider(), rerank_provider))) {
  } else if (OB_FAIL(endpoint_info_.get_unencrypted_access_key(*allocator_, unencrypted_access_key))) {
  } else if (OB_FAIL(rerank_provider->get_header(*allocator_, unencrypted_access_key, headers))) {
  } else if (OB_FAIL(rerank_provider->get_body(*allocator_, request_model_name, query, contents, nullptr, body))) {
  } else if (OB_FAIL(client.send_post(*allocator_, endpoint_info_.get_url(), headers, body, response))) {
  } else if (OB_FAIL(rerank_provider->parse_output(*allocator_, response, result_base))) {
  } else {
    results = static_cast<ObJsonArray *>(result_base);
  }

  if (ret == OB_INVALID_DATA) {
    ObString response_str;
    if (OB_SUCCESS == ObAIFuncJsonUtils::print_json_to_str(*allocator_, response, response_str)) {
      char http_message_str[1024];
      snprintf(http_message_str, sizeof(http_message_str), "unexpected http message: %.*s",
               std::min(static_cast<int>(response_str.length()), 512),
               OB_NOT_NULL(response_str.ptr()) ? response_str.ptr() : "");
      ObString ob_http_message_str(http_message_str);
      FORWARD_USER_ERROR(ret, ob_http_message_str.ptr());
    } else {
      FORWARD_USER_ERROR(ret, "unexpected http message");
    }
  } 
  return ret;
}

bool ObAIFuncJsonUtils::ob_is_json_array_all_str(ObJsonArray* json_array)
{
  bool is_all_str = true;
  if (OB_ISNULL(json_array)) {
    is_all_str = false;
  }
  for (int64_t i = 0; is_all_str && i < json_array->element_count(); i++) {
    ObJsonNode *node = json_array->get_value(i);
    if (OB_ISNULL(node)) {
      is_all_str = false;
    } else if (node->json_type() != ObJsonNodeType::J_STRING) {
      is_all_str = false;
    }
  }
  return is_all_str;
}

int ObAIFuncPromptObjectUtils::construct_prompt_object(ObIAllocator &allocator, ObString &template_str, ObJsonArray *args_array, ObJsonObject *&prompt_object)
{
  INIT_SUCC(ret);
  ObJsonObject *prompt_obj = NULL;
  ObJsonString *template_json_str = NULL;
  if (template_str.empty() || args_array == NULL) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, template_str, template_json_str))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, prompt_obj))) {
  } else if (OB_FAIL(prompt_obj->add(ObAIFuncPromptObjectUtils::prompt_template_key, template_json_str))) {
  } else if (OB_FAIL(prompt_obj->add(ObAIFuncPromptObjectUtils::prompt_args_key, args_array))) {
  }
  if (OB_SUCC(ret)) {
    prompt_object = prompt_obj;
  }
  return ret;
}

bool ObAIFuncPromptObjectUtils::is_valid_prompt_object(ObJsonObject *prompt_object)
{
  bool is_valid = true;
  ObJsonNode *template_node = NULL;
  ObJsonNode *args_node = NULL;
  if (OB_ISNULL(prompt_object)) {
    is_valid = false;
  } else if (prompt_object->element_count() != 2) {
    is_valid = false;
  } else if (OB_ISNULL(template_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_template_key)) ||
            (OB_ISNULL(args_node = prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key)))) {
    is_valid = false;
  } else if (template_node->json_type() != ObJsonNodeType::J_STRING || args_node->json_type() != ObJsonNodeType::J_ARRAY) {
    is_valid = false;
  }
  if (is_valid) {
    ObJsonArray *args_array = static_cast<ObJsonArray *>(args_node);
    for (int64_t i = 0; is_valid && i < args_array->element_count(); i++) {
      ObJsonNode *node = args_array->get_value(i);
      if (OB_ISNULL(node)) {
        is_valid = false;
      } else {
        // String, scalar, array and object arguments are all supported, their
        // textual form is produced when the prompt is expanded.
        is_valid = true;
      }
    }
  }
  return is_valid;
}

int ObAIFuncPromptObjectUtils::append_arg_value(ObIAllocator &allocator,
                                                ObJsonNode *arg_node,
                                                const int64_t depth,
                                                ObStringBuffer &buffer)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(arg_node)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (arg_node->json_type() == ObJsonNodeType::J_OBJECT
             && is_valid_prompt_object(static_cast<ObJsonObject *>(arg_node))) {
    ObString nested_str;
    if (OB_FAIL(expand_prompt_object_recursive(allocator,
                                               static_cast<ObJsonObject *>(arg_node),
                                               depth + 1,
                                               nested_str))) {
    } else if (static_cast<int64_t>(buffer.length()) + nested_str.length()
               > MAX_PROMPT_RESULT_LENGTH) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: expanded prompt is too long");
    } else if (OB_FAIL(buffer.append(nested_str))) {
    }
  } else if (arg_node->json_type() == ObJsonNodeType::J_STRING) {
    ObString arg_str = static_cast<ObJsonString *>(arg_node)->get_str();
    if (static_cast<int64_t>(buffer.length()) + arg_str.length() > MAX_PROMPT_RESULT_LENGTH) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: expanded prompt is too long");
    } else if (OB_FAIL(buffer.append(arg_str))) {
    }
  } else {
    ObString arg_str;
    if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(allocator, arg_node, arg_str))) {
    } else if (static_cast<int64_t>(buffer.length()) + arg_str.length()
               > MAX_PROMPT_RESULT_LENGTH) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: expanded prompt is too long");
    } else if (OB_FAIL(buffer.append(arg_str))) {
    }
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::expand_prompt_object_recursive(ObIAllocator &allocator,
                                                              ObJsonObject *prompt_object,
                                                              const int64_t depth,
                                                              ObString &prompt_str)
{
  INIT_SUCC(ret);
  ObJsonNode *template_node = NULL;
  ObJsonNode *args_node = NULL;
  ObString template_str;
  if (depth > MAX_PROMPT_NEST_DEPTH) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: nested prompt is too deep");
  } else if (OB_ISNULL(prompt_object)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(template_node = prompt_object->get_value(prompt_template_key))
             || template_node->json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(args_node = prompt_object->get_value(prompt_args_key))
             || args_node->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FALSE_IT(template_str = static_cast<ObJsonString *>(template_node)->get_str())) {
  } else if (template_str.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *args_array = static_cast<ObJsonArray *>(args_node);
    const int64_t args_count = static_cast<int64_t>(args_array->element_count());
    const char *template_ptr = template_str.ptr();
    const int64_t template_len = template_str.length();
    ObStringBuffer buffer(&allocator);
    for (int64_t i = 0; OB_SUCC(ret) && i < template_len;) {
      const char cur = template_ptr[i];
      if ('{' == cur && i + 1 < template_len && '{' == template_ptr[i + 1]) {
        // "{{" is an escaped literal brace
        if (buffer.length() + 1 > static_cast<uint64_t>(MAX_PROMPT_RESULT_LENGTH)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(buffer.append("{"))) {
        } else {
          i += 2;
        }
      } else if ('}' == cur && i + 1 < template_len && '}' == template_ptr[i + 1]) {
        // "}}" is an escaped literal brace
        if (buffer.length() + 1 > static_cast<uint64_t>(MAX_PROMPT_RESULT_LENGTH)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(buffer.append("}"))) {
        } else {
          i += 2;
        }
      } else if ('{' == cur) {
        int64_t pos = i + 1;
        int64_t index = 0;
        bool has_digit = false;
        bool overflow = false;
        while (pos < template_len && template_ptr[pos] >= '0' && template_ptr[pos] <= '9') {
          has_digit = true;
          if (index > (INT64_MAX - (template_ptr[pos] - '0')) / 10) {
            overflow = true;
          } else {
            index = index * 10 + (template_ptr[pos] - '0');
          }
          pos++;
        }
        if (overflow) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid placeholder index");
        } else if (has_digit && pos < template_len && '}' == template_ptr[pos]) {
          if (index >= args_count) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid placeholder index");
          } else if (OB_FAIL(append_arg_value(allocator,
                                              args_array->get_value(static_cast<uint64_t>(index)),
                                              depth,
                                              buffer))) {
          } else {
            i = pos + 1;
          }
        } else {
          // Not a placeholder, keep the brace as a literal character.
          if (buffer.length() + 1 > static_cast<uint64_t>(MAX_PROMPT_RESULT_LENGTH)) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(buffer.append("{"))) {
          } else {
            i++;
          }
        }
      } else {
        if (buffer.length() + 1 > static_cast<uint64_t>(MAX_PROMPT_RESULT_LENGTH)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(buffer.append(template_ptr + i, 1))) {
        } else {
          i++;
        }
      }
    }
    if (OB_SUCC(ret)) {
      prompt_str = buffer.string();
    }
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::expand_prompt_object(ObIAllocator &allocator,
                                                    ObJsonObject *prompt_object,
                                                    ObString &prompt_str)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(prompt_object) || !is_valid_prompt_object(prompt_object)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: invalid prompt object");
  } else if (OB_FAIL(expand_prompt_object_recursive(allocator, prompt_object, 0, prompt_str))) {
  }
  return ret;
}

int ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(ObIAllocator &allocator, ObJsonObject* prompt_object, ObString& replaced_prompt_str)
{
  return expand_prompt_object(allocator, prompt_object, replaced_prompt_str);
}
const ObString ObAIFuncModel::get_request_model_name()
{
  ObString request_model_name = info_.model_;
  if (!endpoint_info_.get_request_model_name().empty()) {
    request_model_name = endpoint_info_.get_request_model_name();
  }
  return request_model_name;
}

} // namespace common

namespace query
{

int ObAIModelResolver::resolve_model_name(
    common::ObIAllocator &allocator,
    const common::ObString &model_id,
    common::ObString &model_name)
{
  int ret = OB_SUCCESS;
  common::ObAIFuncExprInfo *info = nullptr;
  if (OB_FAIL(common::ObAIFuncUtils::get_ai_func_info(
          allocator, model_id, info))) {
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_write_string(allocator, info->model_, model_name))) {
  }
  return ret;
}

} // namespace query
} // namespace oceanbase
