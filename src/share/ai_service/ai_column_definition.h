/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/string/ob_string.h"
#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase { namespace common { class ObObj; } }
namespace oceanbase { namespace share {
namespace schema { class ObTableSchema; class ObColumnSchemaV2; }

// These occupy the two previously reserved, persisted column flag bits.
constexpr int64_t AI_RESULT_COLUMN_FLAG = INT64_C(1) << 4;
constexpr int64_t AI_RESULT_SPEC_COLUMN_FLAG = INT64_C(1) << 5;

struct AiColumnDependency {
  uint64_t column_id = 0;
  std::string column_name;
};

struct AiColumnDefinition {
  enum Function { COMPLETE = 0, EMBED = 1 };
  uint64_t column_id = 0;
  uint64_t state_column_id = 0;
  uint64_t request_column_id = 0;
  uint64_t result_spec_column_id = 0;
  std::string column_name;
  std::string expression;
  std::string normalized_expr;
  std::string input_expr;
  std::string config_expr = "NULL";
  std::string model_name;
  std::string system_prompt;
  bool input_is_json = false;
  std::vector<AiColumnDependency> dependencies;
  Function function = COMPLETE;
  int64_t dimension = 0;
  int64_t max_retries = 3;

  int encode(std::string &output) const;
  int decode(const common::ObString &input);
  int decode(const common::ObObj &input);
};

// Use hex literals so internal SQL has identical semantics with and without
// NO_BACKSLASH_ESCAPES, and never interpolates model input as SQL source.
std::string ai_column_sql_text(const std::string &value);
std::string ai_column_sql_identifier(const std::string &value);
int ai_column_rebind_spec(const schema::ObTableSchema &table, schema::ObColumnSchemaV2 &spec);

} } // namespace oceanbase::share
