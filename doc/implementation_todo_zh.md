# TTL 与统计元数据实现索引

当前状态：阶段一至七均已接通。全局统计采用 MANIFEST 逐文件键引用摘要恢复，
正常重开不扫描新格式 SST 数据；旧文件首次升级时才扫描。
最新实现、恢复与回收流程见 [`metadata_recovery_zh.md`](metadata_recovery_zh.md)。
下方各阶段的原始说明保留用于学习实现过程，以本页状态及最新文档为准。

源码使用两个统一标记：

```bash
rg 'TODO\(课程实验-TTL\)' db include
rg 'TODO\(课程实验-统计\)' db include
```

建议严格按下列顺序实现。每完成一项，同时删除对应 TODO 并补测试。

## 阶段一：TTL 写入与恢复

状态：已实现。新增测试位于 `db/ttl_internal_test.cc` 的
`TTLWriteBatchTest` 和 `TTLWriteRecoveryTest`，使用可控时钟检查 batch 编解码、
混合 batch 的类型与序列号、零 TTL、溢出无写入、WAL 重放和刷盘重开。
此阶段尚不实现 Get/迭代器过期过滤，也尚未增加进程异常退出专项测试。

| 顺序 | 文件/位置 | 要实现的内容 |
| --- | --- | --- |
| 1 | `include/leveldb/write_batch.h`，`WriteBatch::Handler` | 增加非纯虚 TTL 回调，避免破坏已有 Handler 子类 |
| 2 | `db/write_batch_internal.h` | 声明内部 `PutWithTTL` |
| 3 | `db/write_batch.cc`，batch 编解码 | 定义 TTL tag，编码绝对时间和值，Iterate 解码 |
| 4 | `db/write_batch.cc`，`MemTableInserter` | 使用 `kTypeValueWithTTL` 插入 MemTable |
| 5 | `db/db_impl.h/.cc`，四参数 `Put` | 先检查时间溢出，再构造 batch 并调用标准 Write |
| 6 | `db/db_impl.cc`，`RecoverLogFile` | 确认恢复只重放绝对时间，不重新计算 TTL |

完成标准：TTL 写入不再返回 NotSupported；TTL=0 仍推进序列号；溢出不写 WAL、不推进序列号；关闭数据库前 WAL 中的数据重开后保持原到期时间。

## 阶段二：Get 与快照时间

状态：已实现。逐层流程、返回值约定与测试说明见
[`ttl_stage2_walkthrough_zh.md`](ttl_stage2_walkthrough_zh.md)。
新增 `TTLReadMemTableTest` 和 `TTLReadTest`，覆盖内存表、Immutable MemTable、
SST、到期边界、旧版本遮挡、快照的序列号与时间、缓存读取和重开。

| 顺序 | 文件/位置 | 要实现的内容 |
| --- | --- | --- |
| 1 | `db/memtable.h/.cc`，`MemTable::Get` | 增加读取时间，处理 TTL value |
| 2 | `db/version_set.cc`，`Saver`/`SaveValue` | SST 点查处理 TTL value |
| 3 | `db/db_impl.cc`，`DBImpl::Get` | 固定一次读取时间并向 MemTable、Version 传递 |
| 4 | `db/snapshot.h`，`SnapshotImpl` | 已预留 `read_time_micros`，保持序列号和时间成对使用 |

完成标准：到期前返回原始用户 value；到期时返回 NotFound；过期新版本不会暴露旧版本；Snapshot 在创建时间可见的数据以后仍可读取。

## 阶段三：迭代器

状态：已实现。逐处实现和正反向状态机解读见
[`ttl_stage3_walkthrough_zh.md`](ttl_stage3_walkthrough_zh.md)。
新增 `TTLIteratorTest` 的 9 项测试，覆盖 Seek、双向扫描、方向切换、固定时间与
序列号、Snapshot、MemTable/Immutable MemTable/SST、重开、合并期间的旧视图、
损坏记录和随机操作模型校验。

| 顺序 | 文件/位置 | 要实现的内容 |
| --- | --- | --- |
| 1 | `db/db_iter.h/.cc`，`NewDBIterator`/构造函数 | 增加固定读取时间参数和成员 |
| 2 | `db/db_impl.cc`，`NewIterator` | 选择 Snapshot 时间或迭代器创建时间 |
| 3 | `db/db_iter.cc`，`FindNextUserEntry` | 有效 TTL 解码，过期 TTL 按 deletion 跳过旧版本 |
| 4 | `db/db_iter.cc`，`FindPrevUserEntry` | 反向遍历实现同样语义 |

完成标准：Seek、SeekToFirst、SeekToLast、Next、Prev 结果一致；迭代器创建后时间推进不会改变该迭代器的视图。

## 阶段四：写入统计

状态：已实现。详见 [`metadata_stage4_walkthrough_zh.md`](metadata_stage4_walkthrough_zh.md)。
已从合并后的 WriteBatch 收集正增量，并在 WAL/MemTable 成功后将计数与序列号
一起发布。新增 `db/metadata_stats_test.cc` 的 13 项单元/集成测试。
后续已在阶段六、七补齐 Compaction 扣减和启动恢复；阶段四文档保留其当时的范围说明。

| 顺序 | 文件/位置 | 要实现的内容 |
| --- | --- | --- |
| 1 | `db/db_impl.h`，`metadata_` | 已预留全局 `MetadataState` |
| 2 | `db/db_impl.cc`，`DBImpl::Write` | 从合并后的整批 WriteBatch 收集正 delta |
| 3 | `db/db_impl.cc`，写入发布点 | 成功后在 mutex 下同时发布 delta 和 LastSequence |
| 4 | `db/db_impl.cc`，`GetCurrentStats` | 已实现空指针检查和锁内 O(1) 快照 |

完成标准：普通 Put、TTL Put、Delete 每项都增加 total；同键重复操作不重复增加 distinct；WriteBatch 查询只能看到批次前或批次后；三个字段属于同一状态。

## 阶段五：文件局部统计

状态：已实现。详见 [`metadata_stage5_walkthrough_zh.md`](metadata_stage5_walkthrough_zh.md)。
MemTable 维护局部计数；刷盘、Compaction 输出和 Repair 产生文件统计；
MANIFEST 新 tag 持久化统计，文件移层与 MANIFEST 重写保留 known/unknown 状态。
新增 11 项测试，独立扫描 SST 核对文件计数，并确认 MANIFEST 恢复统计本身不打开 SST。
阶段五最初仅持久化每文件两个计数；后续恢复实现又增加了逐文件键引用摘要，
现在可由有效文件摘要与恢复后的内存表完整重建 `metadata_`。

| 顺序 | 文件/位置 | 要实现的内容 |
| --- | --- | --- |
| 1 | `db/memtable.h` | 增加 MemTable 的 `SourceMetadata` |
| 2 | `db/builder.cc` | 构建 SST 时计算记录数与相邻去重键数 |
| 3 | `db/version_edit.h`，`FileMetaData` | 保存文件局部统计和 known 标志 |
| 4 | `db/version_edit.cc` | 用新 tag 编解码带统计的新文件记录 |
| 5 | `db/version_set.cc`，`WriteSnapshot` | MANIFEST 快照复制文件统计 |
| 6 | `db/repair.cc` | Repair 时重建文件统计 |

完成标准：MemTable 刷 SST 前后全局统计不变；重开后文件元数据不丢失；旧格式 MANIFEST 的缺失统计被识别为 unknown，而不是错误地当 0。

## 阶段六：Compaction 与 TTL 回收

状态：已实现。实际 drop 收集负 delta，Version 安装成功后锁内应用；
TTL 回收保护活跃 Snapshot，引用计数保护旧迭代器，并支持最深层原地重写。

| 顺序 | 文件/位置 | 要实现的内容 |
| --- | --- | --- |
| 1 | `DBImpl::CompactionState` | 增加固定 compaction 时间和负 MetadataDelta |
| 2 | `DBImpl::DoCompactionWork` | 在 drop 判定中加入保守 TTL 安全条件 |
| 3 | `DBImpl::DoCompactionWork` | 每个实际 drop 的内部记录收集 RemoveRecord |
| 4 | `DBImpl::InstallCompactionResults` | Version 安装成功后原子应用负 delta |
| 5 | `DBImpl::BackgroundCompaction` | 避免需 TTL 回收的文件走 trivial move |

完成标准：时间流逝不改变统计；Compaction 真正回收后才扣统计；回收不推进 seq；活跃 Snapshot/迭代器仍能读取其固定视图；旧值不会因删除 TTL 版本而复活。

## 阶段七：启动重建

状态：已实现。正常启动加载 MANIFEST 中当前文件的逐键引用摘要，再计入 WAL
恢复后仍存在的 MemTable/Immutable MemTable；不重复扫描 WAL 或已退出当前 Version 的文件。
旧 tag 7/10 文件首次扫描升级并重写 MANIFEST。损坏摘要或升级失败导致 Open 失败。

| 顺序 | 文件/位置 | 要实现的内容 |
| --- | --- | --- |
| 1 | `DB::Open` | VersionEdit 和 WAL 全部恢复后开始重建 |
| 2 | 文件摘要与内存表迭代器 | 合并当前 SST 的逐键引用数，仅扫描存活的 mem/imm；旧 SST 首次升级时扫描 |
| 3 | `MetadataState` | 一次性应用重建 delta，失败则 Open 失败 |

完成标准：正常重开和 `sync=true` 后异常退出恢复都得到精确统计；不重复统计 WAL；过期但未回收的 TTL 记录仍被统计。

## 最终验证

```bash
cmake --build build --target metadata_test ttl_test leveldb_tests -j2
./build/metadata_test
./build/ttl_test
./build/leveldb_tests

python3 benchmark/runner.py --kind combined --source "$PWD" \
  --output "$PWD/build/evaluation-combined-quick-NEW" \
  --profile quick --jobs 2
```

benchmark 每次必须使用新的输出目录。
