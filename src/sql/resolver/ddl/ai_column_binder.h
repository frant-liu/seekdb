/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/ai_service/ai_column_definition.h"
#include "sql/parser/parse_node.h"
namespace oceanbase {
namespace share { namespace schema { class ObTableSchema; class ObColumnSchemaV2; class ObSchemaGetterGuard; } }
namespace sql {
class ObRawExprFactory;
class ObSQLSessionInfo;
class AiColumnBinder {
public:
  // All result columns must already be marked, including forward references.
  // The caller owns allocation of the three hidden column IDs.
  static int bind(ObRawExprFactory &factory, const ObSQLSessionInfo &session,
      const share::schema::ObTableSchema &table,
      const share::schema::ObColumnSchemaV2 &column,
      const ParseNode &expression, const ParseNode *options,
      share::AiColumnDefinition &definition);
  static int validate_model(common::ObIAllocator &allocator,
      share::schema::ObSchemaGetterGuard &guard, const share::AiColumnDefinition &definition);
  static int add_hidden_columns(share::schema::ObTableSchema &table,
      share::AiColumnDefinition &definition, uint64_t &last_column_id);
};
} }
