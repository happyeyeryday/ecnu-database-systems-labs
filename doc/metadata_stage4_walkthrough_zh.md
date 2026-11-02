# 阶段四：写入统计与原子发布

说明：本文记录阶段四当时的实现范围。后续已完成全局恢复与 Compaction 扣减，
最新状态见 [`metadata_recovery_zh.md`](metadata_recovery_zh.md)。

## 1. 统计口径

这里统计的是内部记录，不是当前 Get 能读到的键值。

| 操作 | total_entries 增量 | distinct_keys 增量 | seq 增量 |
| --- | ---: | ---: | ---: |
| Put 一个从未出现的键 | 1 | 1 | 1 |
| 覆盖已有键 | 1 | 0 | 1 |
| Delete 已有键 | 1 | 0 | 1 |
| Delete 从未出现的键 | 1 | 1 | 1 |
| TTL Put，包括 TTL=0 | 1 | 按用户键是否已存在决定 | 1 |
| TTL 自然到期 | 0 | 0 | 0 |
| TTL 时间计算溢出 | 0 | 0 | 0 |

Delete 会写删除标记，不会立即清除旧记录；过期 TTL 在物理回收前也仍是内部记录。
WAL 是 MemTable 的日志副本，不另算一份。

本阶段实现写入带来的正增量，以及计数和序列号的原子发布。
阶段六实现 Compaction 的负增量，阶段七实现重开后的初始统计重建。
在这两阶段完成前，重开后原有记录的统计和物理回收后的扣减仍不正确。

## 2. 完整流程

```text
Put / TTL Put / Delete / WriteBatch
  ↓ 统一进入 DBImpl::Write
Writer 加入队列，等待成为队头或被队头合并处理
  ↓ mutex_ 内
MakeRoomForWrite → BuildBatchGroup
  │ 得到整个合并 batch，而不是只取本线程的原始 batch
  │ 为其设置连续序列号范围（此时尚未发布 LastSequence）
  ↓ 释放 mutex_
CollectWriteBatchMetadata：解析并校验整批，生成局部 MetadataDelta
  ↓ 解析成功才继续
写 WAL → 按需 Sync → 插入 MemTable
  ↓ 重新持有 mutex_
全部成功？
  ├─ 是：metadata_.Apply(delta) → SetLastSequence(batch_end)
  └─ 否：不发布计数和序列号；存储失败时记录永久后台错误
  ↓
唤醒这组 Writer，返回整组执行状态
```

## 3. MetadataState 为什么使用引用数？

已有 `MetadataState` 保存总记录数，以及 `map<用户键, 内部记录数量>`。
新增记录时，相应键的引用数加一；去重键数等于 map 的节点数。

```text
Put(a, 1) → total=1，a 的引用数=1，distinct=1
Put(a, 2) → total=2，a 的引用数=2，distinct=1
Delete(a) → total=3，a 的引用数=3，distinct=1
```

不能因为 Delete(a) 就从 map 中移除 a，因为内部版本和删除标记都还存在。
后续 Compaction 只回收一个版本时，也只能减少一次引用，而非直接删除这个键。

map 使用数据库配置的 Comparator。若 Comparator 认为 `Key` 和 `KEY` 相等，
统计也只能把它们算成一个去重键，不能仅按原始字节区分。

## 4. 新增 batch 收集器

位置：`db/metadata_stats.h/.cc`。

新增 `CollectWriteBatchMetadata`，内部 Handler 实现三个回调：

```cpp
void Put(const Slice& key, const Slice&) override {
  delta_->AddRecord(key);
}
void Delete(const Slice& key) override {
  delta_->AddRecord(key);
}
void PutWithTTL(const Slice& key, const Slice&, uint64_t) override {
  delta_->AddRecord(key);
}
```

三类操作统计逻辑相同，因为它们各自产生一条内部记录。
收集器不检查 TTL 是否到期，也不读取当前时间。

`batch.Iterate` 同时验证类型、长度前缀、TTL 时间头和操作数。
解析失败可能留下局部的部分 delta，但它从未应用到全局状态，直接丢弃即可。
必须在写 WAL 和插入 MemTable 前完成这个校验，否则坏 batch 可能先插入一半。

为什么不直接遍历用户传来的 updates？因为 `BuildBatchGroup` 可能把多个等待线程
合并成一个 batch。队头负责整组写入，也必须为整组记录收集一次完整增量。
被合并的 Writer 返回时不能再单独计数，否则会重复统计。

## 5. 为什么在锁外收集，在锁内应用？

位置：`db/db_impl.cc` 的 `DBImpl::Write`。

构建 delta 需要扫描 batch、复制用户键和操作有序 map，工作量可能比较大。
它只修改局部数据，不依赖当前全局计数，因此放在锁外完成，避免阻塞统计查询。
当前队头 Writer 原本就负责保护这次 WAL 写入和 MemTable 插入，不会有另一个
队头并发处理同一存储写入流程。

但 `metadata_.Apply` 修改共享状态，必须放在锁内，并与 LastSequence 同时发布：

```cpp
if (status.ok()) {
  status = metadata_.Apply(metadata_delta);
  if (status.ok()) {
    versions_->SetLastSequence(last_sequence);
  } else {
    RecordBackgroundError(status);
  }
}
```

中间没有解锁，查询线程看不到两个步骤之间的状态。
WAL 和 MemTable 没成功时，整个局部 delta 不应用。

## 6. 一致性查询

`GetCurrentStats` 原有空指针检查与锁内快照逻辑继续保留：

```cpp
if (stats == nullptr) return Status::InvalidArgument(...);
MutexLock lock(&mutex_);
*stats = metadata_.Snapshot(versions_->LastSequence());
```

查询与写入发布使用同一个 mutex，因此三个字段属于同一已发布状态。
即使写线程正在锁外写 WAL、插入 MemTable，查询也只返回前一整批已发布的结果。

不能把三个字段分别改成 atomic 就认为满足一致性：那样仍可能混读旧计数和新序列号。
查询始终忽略 `ReadOptions::snapshot`，返回当前统计而不是快照时的统计。

## 7. 失败路径与序列号

- TTL 溢出：在进入 Write 之前返回 InvalidArgument，什么也不发布。
- batch 格式损坏：局部解析失败，不写 WAL，不插入 MemTable；后续合法写入仍可成功。
- WAL Append/Sync 失败：不发布计数和序列号，并调用 RecordBackgroundError。
- metadata Apply 失败：属于内部状态异常，停止后续写入，避免继续扩大不一致。

为什么 WAL 失败还要禁止后续写入？一次失败的 Append 或 Sync 可能已经写入了
部分甚至完整记录，重开时可能恢复出来。如果当前没有发布它的序列号，却允许
后续写入复用这段序列号，就可能在恢复后出现序列号冲突。

因此失败后停止写入，保留原来的已发布视图；失败操作在重开后的结果仍可能不确定，
不能把 I/O 错误理解为“磁盘上绝对没有这条记录”。

## 8. 本阶段边界与性能

- MemTable 刷成 SST 只是转移记录，不增加或减少全局计数。
- 空 WriteBatch 不产生记录；`Write(nullptr)` 是强制轮换/刷盘相关操作，不计为用户记录。
- Compaction 的实际回收暂未扣减，恢复后也暂未重建，不能据此验收完整统计功能。
- 全局额外内存约为 O(K)，K 为去重键数，包含键副本和 map 节点。
- 一个 B 条操作、U 个不同键的 batch，临时 delta 占 O(U)，收集约 O(B log U)，
  全局应用约 O(U log K)，忽略键比较与复制本身的长度成本。
- 查询是 O(1)，只在锁内复制三个整数；全局 delta 的应用仍会占用锁。
- 预校验增加了一次 batch 解析，但避免锁内逐条维护和错误批次部分插入。

## 9. 测试

新增 `db/metadata_stats_test.cc`，并加入 CMake 的 `leveldb_tests` 源文件列表。

13 项测试包括：

- 三种操作的增量收集、Comparator 去重、坏 delta 不部分更新。
- 空库、覆盖、删除不存在键、忽略快照、空指针、逐步核对序列号。
- 混合 batch、空 batch、nullptr batch。
- TTL=0、自然到期、两类时间溢出。
- 二进制键、空键、大小写等价 Comparator。
- 刷盘与跨 MemTable/SST 同键去重，并用内部记录扫描独立核对物理条数。
- 损坏 batch 不写 WAL、不占序列号，下一次合法写入正常。
- 注入 WAL Append/Sync 错误，验证无发布且后续写入被禁止。
- 刻意阻塞 WAL，验证并发查询仍返回前一完整已发布批次。
- 6 个写线程，每线程 100 个四操作 batch，并发查询必须始终满足：
  `total == seq`、`seq % 4 == 0`、`distinct * 4 == total`，最终核对 2400 条内部记录。

```bash
cmake --build build --target leveldb_tests metadata_test -j2
./build/leveldb_tests --gtest_filter='Metadata*'
./build/metadata_test --gtest_filter=StatsDBTest.BasicCount
./build/leveldb_tests
```

阶段四不运行统计恢复测试作为通过条件；它将由阶段七完成。
