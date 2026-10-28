# 异步 AI 列

AI 列限定 MySQL 模式，通过现有 AI Model / Endpoint 注册和原生 AI 函数执行模型调用。

```sql
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  body LONGTEXT,
  summary LONGTEXT AI COLUMN (AI_COMPLETE('text_model', body))
    WITH (sync_mode='async', max_retries=3),
  embedding VECTOR(4) AI COLUMN (AI_EMBED('embed_model', body, 4))
    WITH (sync_mode='async', max_retries=3)
);
SHOW AI COLUMNS FROM docs;
```

支持 CREATE 和 ALTER ADD；后者由后台为已有行补建任务。表达式必须调用 AI_COMPLETE 或 AI_EMBED，模型名称必须是非空常量。输入和配置可以依赖普通列及确定性表达式，允许 AI_PROMPT；拒绝嵌套模型调用和 AI 列之间的依赖。向量列和显式维度必须一致。Completion 结果类型支持文本、JSON 和整数，并向模型补充 JSON / 整数类型提示词。

业务事务中的插入、依赖列实际变化或主键变化，会同时失效旧结果并保存输入快照和请求标识。无关列更新不会重新推理。回滚同时撤销失效和任务；模型调用只读取已提交任务。业务 SQL 不能直接赋值 AI 结果列。删除行会清理对应任务。

后台以行请求为组领取持久任务，模型调用在领取事务提交后执行。领取者、请求标识和定义版本共同保护写回；旧请求和删除行的迟到结果不能恢复或覆盖数据。同组所有列达到终态后，以一个事务发布成功结果、失败状态和索引变更，随后删除任务。部分列失败不会阻止其他成功列发布。启动时先恢复上次进程留下的租约，再开始领取任务；已保存的终态结果无需再次调用模型。

NULL 输入会得到值为 NULL、状态为 READY 的结果，不调用模型。网络错误、非法 JSON、错误向量维度、非法元素及整数或文本结果越界进入有限重试，耗尽后为 FAILED。PENDING 和 FAILED 默认读为 NULL，因此不能仅通过结果是否 NULL 判断就绪。

```sql
ALTER TABLE docs RETRY AI COLUMN summary
  WHERE status='FAILED' AND id IN (1, 3);
SET SESSION ai_column_strict_read=ON;
SELECT summary FROM docs;
```

RETRY 支持普通 SQL 条件，只重建指定 AI 列和命中的行，也允许重算 READY 行。未加引号的 status 是该目标 AI 列的状态；反引号引用的 `status` 是同名业务列。RETRY 通过内核 UPDATE 路径执行，结果失效和新任务属于同一事务，支持回滚。零行命中不创建任务。

严格读取默认关闭。开启后，执行时对语句原始引用的 AI 列做全列就绪检查；该列任意行不是 READY 都报 11120，即使 WHERE 只匹配就绪行或被优化成零行。普通列和其他全部 READY 的 AI 列仍可读取。检查范围保存在物理计划中，缓存或预编译计划每次执行时都会重新检查。UPDATE / DELETE 的 AI 列读取也遵循此规则。

现阶段不支持修改已有 AI 列定义，也不支持直接修改、重命名或删除已绑定的 AI 列及其依赖列；解析阶段明确拒绝这些操作，避免留下失效元数据。无关列和索引的 DDL 不受该限制。系统元数据位于 `oceanbase.__all_ai_column`，持久执行任务位于 `oceanbase.__all_ai_column_task`。

回归入口为 ai_column 套件中的 ai_column_ddl、ai_column_t1_dml、ai_column_worker、ai_column_retry_strict、ai_column_vector_index 和 ai_column_runtime_edges。独立 C++ 解析、绑定及元数据测试位于 unittest/sql/resolver/test_ai_column_resolver.cpp。
