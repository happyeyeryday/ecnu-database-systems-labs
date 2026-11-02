# LevelDB 统计元数据设计

本文对应 `实验说明.md` 的“功能二”。当前已接入写入、一致查询、文件摘要持久化、Compaction 扣减与全局恢复。最新实现见 [`metadata_recovery_zh.md`](metadata_recovery_zh.md)。阶段四/五的文档保留阶段性说明；下文的全量扫描方案是正确性基线，实际正常启动已使用逐文件键引用摘要，避免全量 SST 数据扫描。

## 1. 先理解 LevelDB 保存了什么

LevelDB 对外看起来是 `key -> value`，内部保存的却是多版本记录：

```text
InternalKey = user_key + sequence_number + value_type
```

- `user_key` 是用户传入的键。
- `sequence_number` 是全库单调递增的写入序号。一个 `WriteBatch` 中每条操作各占一个序号。
- `value_type` 区分值和删除标记（tombstone）。删除不是立刻擦除旧值，而是先写一条删除记录。
- 同一个用户键可以同时有多个内部记录。例如连续两次 `Put("a", ...)` 会留下两个版本，直到 Compaction 判断旧版本不再被快照需要才回收。

数据依次经过这些位置：

```text
WriteBatch -> WAL + mutable MemTable -> immutable MemTable -> SST files
                                      \-> Compaction -> new SST files
```

- WAL（write-ahead log）是故障恢复日志。它与 MemTable 表示同一批记录，不能重复计数。
- MemTable 是内存中的有序表；写满后变成 immutable MemTable（`imm_`），等待刷成 SST。
- SST 是不可修改的有序文件。
- Version 是“当前有效 SST 文件集合”的不可变视图。Compaction 先生成新文件，再用一次 Version 切换原子替换输入文件。

因此，本实验统计的是**当前仍存在的内部记录**，不是当前能被 `Get` 读到的逻辑键值。删除标记、旧版本和已过期但尚未回收的 TTL 记录都要计数。

## 2. 推荐数据结构

### 2.1 全局统计

`MetadataState` 保存：

```cpp
uint64_t total_entries;
map<user_key, uint64_t, UserComparator> key_references;
```

- `total_entries` 是所有当前内部记录数。
- `key_references[k]` 是用户键 `k` 当前拥有的内部记录数。
- `distinct_keys` 不单独维护，等于 `key_references.size()`。
- 引用数从 0 变 1 时，去重键数增加；从 1 变 0 时减少。

不能只用 `std::set<string>`：Compaction 删除 `a` 的一个旧版本时，`a` 可能还有其他版本，集合无法判断是否应该删除 `a`。

骨架使用带 LevelDB 用户 Comparator 的 `std::map`，而不是普通 `unordered_map`。原因是自定义 Comparator 可能把不同字节串判为同一个逻辑键，统计必须与数据库的键相等语义一致。代价是每条变更为 `O(log K)`；若性能测试证明默认 BytewiseComparator 下成本过高，可以仅对默认比较器增加哈希表快路径，自定义比较器仍走有序 map。

### 2.2 内存统计、文件统计和全局统计

每个 MemTable/SST 可以维护一个小的 `SourceMetadata`：

```cpp
struct SourceMetadata {
  uint64_t total_entries;
  uint64_t distinct_keys;
};
```

关系为：

```text
全局 total_entries
  = mem_ 记录数
  + imm_ 记录数
  + current Version 中所有 SST 的记录数

全局 distinct_keys
  = 上述所有来源 user_key 集合的并集大小
  != 各来源 distinct_keys 之和
```

例如 `a` 同时存在于 MemTable 和 L1 SST，两边局部去重键数都是 1，但全局只能算 1。因此文件的 `distinct_keys` 只能用于校验、诊断或恢复优化，不能直接相加。全局 `key_references` 才负责跨内存表、跨文件去重。

WAL 不在公式中，因为 WAL 是 MemTable 的持久化副本。MemTable 刷盘只是把同一批记录从 `imm_` 搬到 SST，**全局统计不变**。

建议把 `SourceMetadata` 加到 `MemTable` 和 `FileMetaData`。构建 SST 时内部键已排序，`SourceMetadataBuilder` 比较相邻 user key 即可在 `O(N)` 扫描内得到两个局部计数，无需额外集合。

## 3. 原子状态更新

所有“发布”动作都使用 `DBImpl::mutex_`。耗时工作可以在锁外完成，但 `MetadataDelta` 必须在最终成功后随数据库状态一次应用。

### 3.1 普通写入和 WriteBatch

位置：`DBImpl::Write`。

1. `BuildBatchGroup` 合并等待中的批次，并分配连续序列号。
2. 在锁外解析整个合并 batch，为每条 Put/Delete/TTL Put 调用 `delta.AddRecord(user_key)`；解析失败则不写 WAL、不插入 MemTable，丢弃增量。
3. 解析成功后写 WAL、按需 sync，再插入 `mem_`。删除标记也算一条内部记录；只有存储操作成功才能发布已收集的增量。
4. 重新持有 `mutex_` 后，先应用整个 delta，再把 `versions_->LastSequence()` 发布为批次末序号。
5. 查询也取得同一把锁，因此只能看到批次前或整个批次后，不能看到半个批次。

伪代码：

```cpp
MetadataDelta delta(user_comparator());
CollectWriteBatchKeys(write_batch, &delta);  // 每项都是 AddRecord

// WAL 和 MemTable 成功，且 mutex_ 已重新持有
Status metadata_status = metadata_.Apply(delta);
if (metadata_status.ok()) {
  versions_->SetLastSequence(last_sequence);
}
```

TTL 为 0 的 Put 仍产生内部记录和序列号，所以同样增加统计。TTL 时间溢出应在进入 `Write` 前返回 `InvalidArgument`，不产生记录、不应用 delta、不推进序列号。

### 3.2 MemTable 刷盘

位置：`WriteLevel0Table`、`CompactMemTable`。

刷盘只是移动记录，不能先加 SST 再减 `imm_`，否则容易在失败路径或查询并发时产生短暂错误。成功安装新 Version 并释放 `imm_` 时，全局 delta 为 0。局部计数从 MemTable 复制到新 `FileMetaData` 即可。

### 3.3 Compaction

位置：`DBImpl::DoCompactionWork` 和 `InstallCompactionResults`。

Compaction 输入中的每条未丢弃记录会原样进入某个输出文件，因此它对全局统计净影响为 0。只需要在现有 `drop` 分支中收集真正被回收的记录：

```cpp
if (drop) {
  compact->metadata_delta.RemoveRecord(ikey.user_key);
}
```

不要在锁外扫描时立即修改全局状态，因为 Compaction 可能失败。只有 `versions_->LogAndApply(...)` 成功、输入 SST 已被新 Version 原子替换后，才在持有 `mutex_` 时应用这个负 delta。普通旧版本、删除标记和可安全回收的 TTL 记录都走同一机制。时间自然流逝不触发 delta，只有记录实际被回收才扣减。

一个重要实现细节：Version 安装与 metadata delta 必须被查询视为同一个发布动作。最简单的做法是 `LogAndApply` 成功返回后、释放 `DBImpl::mutex_` 前应用 delta；若 metadata delta 由正确代码生成，`Apply` 不应失败，骨架中的下溢检查用于尽早暴露实现错误。

## 4. 一致查询

`GetCurrentStats` 应忽略 `ReadOptions::snapshot`，并检查空指针：

```cpp
Status DBImpl::GetCurrentStats(const ReadOptions&, Stats* stats) {
  if (stats == nullptr) {
    return Status::InvalidArgument("stats must not be null");
  }
  MutexLock lock(&mutex_);
  *stats = metadata_.Snapshot(versions_->LastSequence());
  return Status::OK();
}
```

三个字段在同一临界区复制，`seq` 与计数属于同一个发布状态。不要把三个字段分别做成 atomic：即使单字段读写原子，查询仍可能读到“旧 total + 新 distinct + 新 seq”的混合结果。

查询为 `O(1)`，只持锁复制三个整数。`ReadOptions::snapshot` 只影响普通数据读取，不影响“当前数据库物理记录”统计。

## 5. 恢复

以下为全量扫描恢复的正确性基线，不是必须采用的最终启动策略。
为避免常规重启扫描全部 SST，可进一步持久化逐文件的 `user_key -> 引用数` 摘要，
绑定文件号并随 MANIFEST 管理其生存期；再结合已有 WAL 恢复未刷盘部分。
最终实现已在两个局部计数之外增加 MANIFEST tag 11，保存逐文件键引用摘要，
并接入无常规全量 SST 扫描的全局恢复。以下扫描流程保留为旧方案背景。

全量重建比另建独立统计日志简单，但有启动代价。独立统计日志很难与 WAL、MANIFEST 和崩溃恢复做原子提交，容易出现数据库已恢复但统计日志少一条或多一条的问题。

恢复流程：

1. `VersionSet::Recover` 从 MANIFEST 恢复当前 SST 集合和最后序列号。
2. `RecoverLogFile` 把尚未刷盘的 WAL 恢复到 MemTable，或生成待安装的 L0 SST。
3. `DB::Open` 完成必要的 `LogAndApply` 后，数据库仍持有 `DBImpl::mutex_`，此时扫描 `mem_ + imm_ + current Version` 的内部迭代器。
4. 对每条内部记录调用 `rebuild_delta.AddRecord(parsed.user_key)`，最后一次性应用。
5. 扫描失败则 `DB::Open` 返回错误，不开放一个统计不可信的数据库。

扫描的是内部迭代器，不能用公开的 `NewIterator`：公开迭代器会隐藏旧版本和删除标记，得到的是错误口径。也不能再单独扫描 WAL，否则会和恢复后的 MemTable/SST 重复计数。

每个 `FileMetaData` 持久化 `SourceMetadata` 后，可以快速得到 total 的校验值；但为了重建全局 `key_references`，第一版仍需扫描所有 user key，启动代价是 `O(N)`。若以后需要优化启动，可以设计带校验和、与 MANIFEST 版本绑定的统计快照，但这不是建议的第一版。

## 6. 开销分析

设当前内部记录数为 `N`，去重用户键数为 `K`，一个批次操作数为 `B`，本次 Compaction 丢弃记录数为 `D`。

| 项目 | 开销 |
| --- | --- |
| Put/Delete/WriteBatch | `O(B log K)`；每条内部记录增加一次引用 |
| MemTable 刷盘 | 全局统计 `O(1)`；构建文件局部统计随原有 SST 扫描顺便完成 |
| Compaction | 原有扫描之外每个丢弃记录 `O(log D)` 收集，安装时 `O(D log K)` |
| GetCurrentStats | 时间 `O(1)`，额外空间 `O(1)`，短暂持有 DB mutex |
| 常驻额外内存 | `O(K)`，包含每个 user key 的一份副本、引用数和 `std::map` 节点开销 |
| 每个 SST 元数据 | 两个 `uint64_t` 加编码开销，即 `O(number_of_files)` |
| Compaction 临时内存 | 最坏 `O(min(D, K))`，只保存被丢弃键的 delta |
| 启动恢复 | 时间 `O(N log K)`，会读取当前内部记录；最终常驻 `O(K)` |

最大风险是长 key 与大量去重 key 导致全局 map 占用明显。报告中应测量峰值 RSS，并把它与不维护精确 distinct key 的基线对比。查询临时全库扫描虽然省常驻 map，但会让每次查询变成 `O(N)`，不适合 benchmark 的并发高频查询，因此不推荐。

## 7. 文件修改顺序

1. 在 `DBImpl` 增加 `MetadataState metadata_`，以 `user_comparator()` 初始化；实现空指针检查和锁内快照。
2. 写一个 `WriteBatch::Handler` 收集 user key，在 `DBImpl::Write` 的成功发布点应用正 delta；补空库、重复 Put、Delete、WriteBatch 和 seq 测试。
3. 给 `MemTable`、`FileMetaData` 和 `CompactionState::Output` 增加 `SourceMetadata`；在 `BuildTable` 和 Compaction 输出中计算它。
4. 扩展 `VersionEdit` 的新文件编码来持久化文件统计。使用新的 tag/格式并对缺失统计的旧记录标记为 unknown，不要悄悄把缺失值当 0。
5. 在 Compaction 的 `drop` 路径收集负 delta，在 Version 安装成功后应用；补精确回收测试。
6. 在 `DB::Open` 完成 Version/WAL 恢复后扫描内部记录重建；补正常重开和 `sync=true` 后强制退出的恢复测试。
7. 最后接入 TTL：过期不可读不改变统计，只有 Compaction 真正 drop 时才减少。

## 8. 本机环境与命令

已检查的工具版本：CMake 3.28.3、G++ 13.3.0、Python 3.12.3、Ninja 1.11.1，均满足 CMake 3.22+、C++17、Python 3.9+。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLEVELDB_BUILD_TESTS=ON -DLEVELDB_BUILD_BENCHMARKS=ON
cmake --build build --target metadata_test ttl_test -j2
./build/metadata_test
./build/ttl_test
```

初始 base 的预期基线是：`metadata_test` 通过 1/3（重开测试因前后都为 0 而假通过），`ttl_test` 通过 0/2。这是初始基线，不是阶段四完成后的结果。阶段四只验收写入统计，不能要求尚未实现的统计恢复测试通过。全部功能完成后运行：

```bash
python3 benchmark/runner.py --kind combined --source "$PWD" \
  --output "$PWD/build/evaluation-combined-quick" --profile quick --jobs 2
```

评测输出目录每次必须使用新路径。
