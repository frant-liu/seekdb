/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/container/ob_iarray.h"
namespace oceanbase { namespace sql {
class ObExecContext;
class AiColumnRuntime {
public:
  static int check_ready(ObExecContext &ctx, const common::ObIArray<uint64_t> &references);
};
} }
