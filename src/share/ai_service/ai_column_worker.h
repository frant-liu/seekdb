/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/ob_thread_pool.h"
#include <atomic>
namespace oceanbase { namespace share {
class AiColumnWorker : public ObThreadPool {
public:
  int init();
  int start() override;
  void run1() override;
private:
  std::atomic<bool> recovered_{false};
};
} }
