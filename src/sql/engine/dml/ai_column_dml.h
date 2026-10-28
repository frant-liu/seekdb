/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
namespace oceanbase { namespace sql {
class ObTableModifyOp;
class AiColumnDml {
public:
  static int process_rows(ObTableModifyOp &op);
};
} }
