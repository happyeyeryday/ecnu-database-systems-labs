# LevelDB Project1：TTL 与统计元数据

本仓库用于朱昭荣（10234304416）的课程作业提交，保存两人小组共同使用的一份 LevelDB 实现及实验报告，并保留原有 Git 提交历史。

## 小组成员与分工

以下分工沿用小组实验报告：

| 姓名 | 学号 | 分工 |
| --- | --- | --- |
| 朱昭荣 | 10234304416 | 统计元数据维护：写入统计、一致性查询、文件摘要、持久化恢复和回收扣减 |
| 徐卓一 | 10235102432 | TTL 功能及测试汇报 |

实验报告：[实验报告.pdf](实验报告.pdf)。

## 实现内容

- TTL：支持 `Put(options, key, value, ttl)`，保存绝对到期时间；Get、Snapshot、Iterator 使用一致的有效期语义；WAL/SST 恢复保留原到期时间，Compaction 安全回收过期记录。
- 统计元数据：通过 `GetCurrentStats` 返回 `total_entries`、`distinct_keys` 和 `seq`；覆盖普通写入、TTL 写入、删除和 WriteBatch，并在写入成功后统一发布计数与序列号。
- 持久化与恢复：在 MANIFEST 保存文件摘要和逐键引用，正常重开合并当前 SST 摘要及恢复后的内存表；旧格式文件首次扫描升级。
- 回收一致性：Compaction 安装新 Version 成功后扣除实际回收记录的贡献；时间流逝和刷盘不改变全局计数，Compaction 不推进写入序列号。

`total_entries` 统计尚未回收的内部记录，旧版本、删除标记、已过期但未回收的 TTL 记录均计数；`distinct_keys` 对这些记录的用户键去重。统计范围为 mem、imm 和当前 Version 的有效 SST，不额外累计 WAL 副本。

## 环境与依赖

- Linux 或 Windows WSL2。
- CMake 3.22+、支持 C++17 的编译器、Python 3.9+。
- GoogleTest 等依赖源码已随仓库提供，无需初始化 Git 子模块。
- 建议构建目录、测试数据库和 benchmark 输出使用 Linux 文件系统，例如 `/tmp`；源码可保存在 Windows 挂载目录。

## 构建与基础测试

在仓库根目录运行。使用新的构建目录，避免复用其他机器的 CMake 缓存。

```bash
cmake -S . -B /tmp/leveldb-p1-build-10234304416 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLEVELDB_BUILD_TESTS=ON \
  -DLEVELDB_BUILD_BENCHMARKS=OFF

cmake --build /tmp/leveldb-p1-build-10234304416 -j4

# 课程测试会在工作目录创建数据库，因此也从 Linux 目录运行。
(cd /tmp/leveldb-p1-build-10234304416 && ./metadata_test && ./ttl_test)

mkdir -p /tmp/leveldb-p1-test-data-10234304416
TEST_TMPDIR=/tmp/leveldb-p1-test-data-10234304416 \
  ctest --test-dir /tmp/leveldb-p1-build-10234304416 --output-on-failure
```

## 公开联合评测

使用课程入口 `benchmark/runner.py`，每次指定新的输出目录。

```bash
repro_tag=$(date +%Y%m%d-%H%M%S)
python3 benchmark/runner.py --kind combined --source "$PWD" \
  --output "/tmp/leveldb-p1-quick-${repro_tag}" --profile quick --jobs 4
```

三轮性能矩阵使用：

```bash
repro_tag=$(date +%Y%m%d-%H%M%S)
python3 benchmark/runner.py --kind combined --source "$PWD" \
  --output "/tmp/leveldb-p1-standard-${repro_tag}" \
  --profile standard --matrix --jobs 4
```

原生 LevelDB 对照方法见 [benchmark/README.md](benchmark/README.md)。

## 验证记录

2026-11-03，Codex 在 WSL2 环境使用当前源码执行了一次本地验证。该环境未安装 CMake/CTest，实际使用 GCC/G++ 按项目源文件清单直接编译原有测试及公开评测程序；上述标准 CMake/CTest 入口未在该环境执行。

| 检查 | 本轮结果 |
| --- | --- |
| `metadata_test` | 3/3 通过 |
| `ttl_test` | 2/2 通过 |
| `leveldb_tests` | 273 项通过，2 项可选压缩测试跳过，0 失败 |
| C API / POSIX 测试 | 通过 |
| 课程固定版本原生回归 | C++、C API、POSIX 三个程序通过；C++ 中 2 项可选压缩测试跳过 |
| 公开 combined quick 全部场景 | 22/22 通过 |

本轮用于功能验证，未在本机重新运行 standard 三轮矩阵。实验报告中的性能结果来自原测试机器。详细环境、方法和范围见 [本地核验记录](doc/local_verification_20261103.txt)。

## 设计文档

- [实现索引与当前状态](doc/implementation_todo_zh.md)
- [TTL 设计](doc/ttl_design_zh.md)
- [统计写入流程](doc/metadata_stage4_walkthrough_zh.md)
- [文件摘要与持久化](doc/metadata_stage5_walkthrough_zh.md)
- [最终恢复与回收流程](doc/metadata_recovery_zh.md)

## AI 参与说明

原实验报告末页说明了 AI 在补充测试、性能分析、实现思路和功能设计中的参与。本次同步保留该报告和原始提交历史；Codex 补充 README、整理核验记录并执行上述本地自动化验证。上述自动化结果不表示组员在本轮手工重跑了全部测试。
