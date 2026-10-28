# IK 字符分类优化与本地验证

`ObFTCharUtil::do_classify` 原来依次调用六类判断函数。每次判断都从同一个输入重新解码 Unicode；中文和其他 CJK 字符到达较后的判断时会重复解码。修改将解码移到分类入口，每个字符只执行一次，随后依次判断字母、数字、中文、其他 CJK 和 UTF-16 高低代理项。分类优先级及错误传播保持原样。

这一定位依据来自热路径代码检查，尚未进行采样 CPU profiling。修改没有覆盖 BENG 分词器，也没有改动排序、倒排查询或存储格式。

## 正确性

`fts_index/ik_char_classification` 的完整预期输出由保留的原始二进制录制，覆盖 ASCII、全角字符、CJK、假名、韩文、补充平面、emoji、标点、连接符、混合数字单位及嵌入 NUL。修改后通过该用例，以及现有 `tokenize_function`、`simple_query`；后者包含 MATCH 查询。基准的四种查询命中数前后分别为 8001、11000、7332、20。

## 可复现基准

同一台虚拟机（4 vCPU，7.6 GiB RAM），Ubuntu 22.04 容器，单个 seekdb 实例，`memory_budget=2G`；Release 使用 `-O2 -g0 -DNDEBUG`。前后均在无并发编译的情况下运行原始 `fts_large_bench.sh`，参数为 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`，每次使用新的报告文件。原始报告附在本文末尾，公开脚本、SQL、数据生成器、评分逻辑和固定基线均未修改。

| 指标 | 修改前 | 修改后 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 12.5740 | 8.7470 | 30.44% |
| build_ik_content_sec | 7.0950 | 6.9900 | 1.48% |
| build_beng_en_sec | 3.7680 | 3.6680 | 2.65% |
| tokenize_ik_avg_ms | 0.3101 | 0.2699 | 12.96% |
| tokenize_beng_avg_ms | 0.2276 | 0.2770 | -21.70% |
| query_cn_avg_ms | 2.0861 | 2.0442 | 2.01% |
| query_beng_avg_ms | 3.0288 | 3.1132 | -2.79% |
| query_mixed_avg_ms | 2.3365 | 2.3495 | -0.56% |
| query_limit_avg_ms | 2.6957 | 2.6408 | 2.04% |

按赛题三类等权规则，相对本机修改前的构建、分词、查询平均提升率分别为 **11.52%、-4.37%、0.18%**，综合 **2.44%**，对应同机基线性能得分约 **4.89/100**。单次完整测试中 BENG 分词退化 21.70%，尽管修改没有涉及该路径；这说明 SQL 循环和机器波动也影响结果，不能把全部差异都归因于此修改。

原始固定 CI 基线评分脚本对此本机修改后报告输出 100/100、综合下降 70.38%；这与同机前后收益不同，不能据此声称正式评测满分。正式成绩取决于统一环境。

## 修改前原始报告

```text
========================================
FTS Large Benchmark Report
========================================
timestamp:              2026-10-28 16:02:28
label:                  before
git_head:               unknown
git_dirty:              1
rows:                   20000
batch:                  500
rounds:                 3000
query_rounds:           200
samples:                3
warmup:                 30
skip_load:              0
----------------------------------------
select1_avg_ms:         0.1388
select1_stdev_ms:       0.0583
raw_load_sec:           0.807
raw_load_rows_per_sec:  24783.1
build_ik_all_sec:       12.574
build_ik_content_sec:   7.095
build_beng_en_sec:      3.768
build_total_sec:        23.455
----------------------------------------
tokenize_ik_avg_ms:     0.3101
tokenize_ik_median_ms:  0.3326
tokenize_ik_stdev_ms:   0.0333
tokenize_beng_avg_ms:   0.2276
tokenize_beng_median_ms:0.2713
tokenize_beng_stdev_ms: 0.0633
----------------------------------------
query_cn_hits:          8001
query_cn_avg_ms:        2.0861
query_cn_stdev_ms:      0.0225
query_beng_hits:        11000
query_beng_avg_ms:      3.0288
query_beng_stdev_ms:    0.0448
query_mixed_hits:       7332
query_mixed_avg_ms:     2.3365
query_mixed_stdev_ms:   0.0614
query_limit_hits:       20
query_limit_avg_ms:     2.6957
query_limit_stdev_ms:   0.0536
========================================
```

## 修改后原始报告

```text
========================================
FTS Large Benchmark Report
========================================
timestamp:              2026-10-28 17:26:56
label:                  after
git_head:               unknown
git_dirty:              1
rows:                   20000
batch:                  500
rounds:                 3000
query_rounds:           200
samples:                3
warmup:                 30
skip_load:              0
----------------------------------------
select1_avg_ms:         0.1490
select1_stdev_ms:       0.0820
raw_load_sec:           0.344
raw_load_rows_per_sec:  58139.5
build_ik_all_sec:       8.747
build_ik_content_sec:   6.990
build_beng_en_sec:      3.668
build_total_sec:        19.422
----------------------------------------
tokenize_ik_avg_ms:     0.2699
tokenize_ik_median_ms:  0.2549
tokenize_ik_stdev_ms:   0.0692
tokenize_beng_avg_ms:   0.2770
tokenize_beng_median_ms:0.2883
tokenize_beng_stdev_ms: 0.0180
----------------------------------------
query_cn_hits:          8001
query_cn_avg_ms:        2.0442
query_cn_stdev_ms:      0.0823
query_beng_hits:        11000
query_beng_avg_ms:      3.1132
query_beng_stdev_ms:    0.0533
query_mixed_hits:       7332
query_mixed_avg_ms:     2.3495
query_mixed_stdev_ms:   0.0363
query_limit_hits:       20
query_limit_avg_ms:     2.6408
query_limit_stdev_ms:   0.0606
========================================
```
