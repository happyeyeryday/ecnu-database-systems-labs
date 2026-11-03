# LevelDB TTL 与统计元数据：六阶段实现与测试 PPT 制作提纲

## 给 PPT 制作 Agent 的说明

- 交付目标：制作一套中文技术答辩 PPT，正文约 32 页，附录 4 页。每页保留一个明确结论，复杂代码放附录或讲稿，不要把整份 Markdown 原样缩成小字。
- 内容依据：当前源码、补充测试、已完成的公开 benchmark 结果。不得把推测写成实测结论，不得生成虚构终端截图。
- 叙事顺序：问题与语义 → 六阶段实现 → 测试证据 → 性能分析 → 总结。
- 阶段映射：项目 `doc/implementation_todo_zh.md` 实际列出七阶段。本 PPT 为满足六阶段展示，将原“阶段六 Compaction 与 TTL 回收”和“阶段七 启动重建”合并为第六阶段，不省略恢复工作。
- 统一配色：蓝色=正常数据路径；橙色=TTL/时间；绿色=统计；红色=错误/禁止行为。流程图使用实线表示调用，虚线表示数据/统计变化。
- 代码字体建议 18–22pt，每页代码约 8–18 行；超过 18 行的代码分两页或放附录。代码中的 `...` 表示摘录省略，不是可编译完整程序；标注“示意”的段落不得当作原样源码。
- 引用路径都相对于仓库 `leveldb_project1/`；以函数名定位为准，行号可能因后续改动变化。
- 性能数据统一注明“3 次重复的中位数”；范围为三次最小值–最大值，不是置信区间。写线程数不是统计查询线程数。
- 封面作者、学号、课程名称由提交者补充，不能自行编造。

---

# 第一部分：目标与总体设计

## 第 1 页｜封面

**标题：LevelDB TTL 与统计元数据的协同实现**

副标题：六阶段增量开发、64 项补充测试与公开性能评测。

信息：课程实验 / 作者 / 学号 / 日期。

配图：`Write → WAL → MemTable → SST` 主链，上方橙色 TTL 时间轴，下方绿色统计账本。

讲述重点：不是两个独立补丁，而是在同一套源码中保证时间可见性、持久化、压实回收与精确统计协同。

## 第 2 页｜实验目标与难点

**结论：读取可见性与物理记录统计必须分开建模。**

要求：
1. 支持 `Put(options, key, value, ttl)`；TTL 单位是秒，内部使用微秒绝对时间。
2. Get、Snapshot、Iterator 对过期有一致语义。
3. 提供当前内部记录数、去重用户 key 数与序列号。
4. WAL/SST 重开、压实、Repair 后保持正确。
5. 查询轻量，不为每次统计扫描数据库。

三大难点：过期新版本不能暴露旧值；跨文件不能直接相加 distinct；写入和压实失败不能提前发布统计。

视觉：左右两列“用户看到的逻辑数据”与“当前存储结构里的内部记录”。

## 第 3 页｜语义契约：什么变化、什么不变

示例（假设历史记录未被压实回收）：

| 操作 | total_entries | distinct_keys | seq | 说明 |
|---|---:|---:|---:|---|
| 空库 | 0 | 0 | 0 | 初始状态 |
| Put(a, old) | 1 | 1 | 1 | 一条内部记录 |
| Put(a, new) | 2 | 1 | 2 | 覆盖产生新版本 |
| Delete(b) | 3 | 2 | 3 | 删除标记也是记录 |

关键契约：
- 普通 Put、TTL Put、Delete 每项均新增内部记录。
- TTL 到期只改变可见性，不立即改变物理计数。
- Flush 只是搬迁，不改变全局计数。
- Compaction 实际删除记录后才扣减，且不推进 seq。
- 统计接口返回当前状态，忽略 `ReadOptions::snapshot`；与 Get 的历史视图语义不同。
- 计数范围是 `mem_ + imm_ + current Version`，不是磁盘上所有仍未 unlink 的文件。

## 第 4 页｜总体架构与六阶段路线

主图：

```text
Put/Write ──→ WriteBatch ──→ WAL ──→ MemTable ──Flush──→ SST
                  │                     │                 │
                  └─MetadataDelta───────┴──────→ MetadataState
                                                        ↑
SST Compaction ──实际 drop──→ 负 MetadataDelta ──────────┘
MANIFEST 文件引用摘要 + WAL 恢复后存活 mem/imm ──→ 启动重建
```

六阶段：
1. TTL 编码、写入与 WAL 恢复。
2. 点查与快照时间。
3. 双向迭代器固定视图。
4. 写入统计与原子发布。
5. 文件局部统计与 MANIFEST。
6. 安全回收、统计扣减与启动重建。

标注：WAL、MemTable、SST 属于原有存储框架；新增的是 TTL 类型和统计相关逻辑。

---

# 第二部分：六阶段实现

## 第 5 页｜阶段一：TTL 编码选择

**结论：持久化绝对过期时间，而不是相对 TTL。**

字节布局图：

```text
internal key = user key | tag(8B)
tag = (sequence << 8) | type
       高56位 sequence   低8位 type

type: deletion=0，value=1，valueWithTTL=2

TTL stored value = expiration_micros(fixed64, 8B) | user value
MemTable entry = varint32(key长度) | internal key |
                 varint32(value长度) | stored value
```

源码：`db/ttl_internal.cc`。

```cpp
void EncodeValueWithTTL(uint64_t expiration_micros,
                        const Slice& user_value,
                        std::string* encoded_value) {
  encoded_value->clear();
  PutFixed64(encoded_value, expiration_micros);
  encoded_value->append(user_value.data(), user_value.size());
}
```

讲稿：sequence 是版本编号，不是时间戳；TTL 时间存 value 头部，不放进排序 key；fixed64 为小端编码。

## 第 6 页｜阶段一：复用标准写入链路

源码：`db/db_impl.cc`，`DBImpl::Put` 四参数版本。

```cpp
Status DBImpl::Put(const WriteOptions& options, const Slice& key,
                   const Slice& value, uint64_t ttl) {
  uint64_t expiration_micros;
  Status s = ComputeExpirationTime(env_, ttl, &expiration_micros);
  if (!s.ok()) return s;

  WriteBatch batch;
  WriteBatchInternal::PutWithTTL(&batch, key, value, expiration_micros);
  return Write(options, &batch);
}
```

流程：计算并检查 deadline → batch 编码 → 标准 Write → WAL → InsertInto → Iterate → MemTableInserter。

恢复链：WAL 原字节 → WriteBatch → Iterate → MemTable。**不再次计算 `now + ttl`，防止重启续命。**

补充：Handler 新增非纯虚 `PutWithTTL` 默认转调 Put，兼容旧 Handler；存储 Handler 必须 override 保留 deadline。

## 第 7 页｜阶段一：边界检查与测试

源码摘录：`db/ttl_internal.cc`，`ComputeExpirationTimeFromNow`。

```cpp
if (ttl_seconds >
    std::numeric_limits<uint64_t>::max() / kMicrosPerSecond) {
  return Status::InvalidArgument("TTL seconds overflow microseconds");
}
const uint64_t ttl_micros = ttl_seconds * kMicrosPerSecond;
if (now_micros > std::numeric_limits<uint64_t>::max() - ttl_micros) {
  return Status::InvalidArgument("TTL expiration time overflow");
}
*expiration_micros = now_micros + ttl_micros;
```

测试证据：
- `ComputesAbsoluteExpiration`、`RejectsExpirationOverflow`：单位换算与两种溢出。
- `EncodesExpirationAndBinaryValue`：含 `\0` 的 value 不被截断。
- `RoundTripAndLegacyHandler`：序列化及兼容回调。
- `RejectsMalformedRecords`：损坏记录拒绝解析。
- `MixedBatchPreservesTypesAndSequences`：普通 Put、TTL、Delete 混合。
- `ZeroTTLAndOverflowHaveCorrectWriteEffects`：零 TTL 仍写入并推进 seq；溢出不写入。
- `WALReplayPreservesDeadlineEvenAfterExpiry`、`FlushAndReopenPreserveDeadline`。

视觉：三个警示卡片“零 TTL ≠ 跳过写入”“溢出 ≠ 最大寿命”“重开 ≠ 续命”。

## 第 8 页｜阶段二：读取视图是二元组

**结论：只固定 sequence 不够，还必须固定 read time。**

```text
普通 Get：在入口锁内取 (LastSequence, NowMicros)
Snapshot Get：取 snapshot 保存的 (sequence, read_time)
                 ↓ 同一组参数
       MemTable → immutable MemTable → Version/SST
```

源码摘录：`db/db_impl.cc`，`DBImpl::Get`。

```cpp
if (options.snapshot != nullptr) {
  const auto* view = static_cast<const SnapshotImpl*>(options.snapshot);
  snapshot = view->sequence_number();
  read_time_micros = view->read_time_micros();
} else {
  snapshot = versions_->LastSequence();
  read_time_micros = env_->NowMicros();
}
```

时间线示例（示意数据）：t=100 创建快照，deadline=150，t=200 普通 Get 不可见，但该快照仍可见。

## 第 9 页｜阶段二：过期是一条“终止结论”

**结论：过期记录必须遮挡更旧版本，不能当作查找未命中。**

示例：`L0: a=new(TTL已过期)`，`L2: a=old(普通值)`，正确结果 NotFound，禁止返回 old。

SST 回调源码摘录：`db/version_set.cc`，`SaveValue`。

```cpp
Status status = DecodeValueWithTTL(v, &expiration_micros, &user_value);
if (!status.ok()) {
  s->state = kCorrupt;
} else if (IsExpired(expiration_micros, s->read_time_micros)) {
  // Stop the SST search, rather than exposing an older value.
  s->state = kDeleted;
} else {
  s->state = kFound;
  s->value->assign(user_value.data(), user_value.size());
}
```

统一边界：`read_time_micros >= expiration_micros` 时过期。

MemTable::Get 的 bool：false=本源未命中可继续；true=已有结论，可能是成功、删除、过期或错误。不要把 true 等同于“返回了值”。

## 第 10 页｜阶段二：跨数据源与时间采样测试

测试文件：`db/ttl_internal_test.cc`。

| 验证点 | 测试名 |
|---|---|
| 到期边界与二进制值 | `MemTableDeadlineBoundaryAndPlainBinaryValues` |
| SST/缓存重复读取 | `SSTDeadlineBoundaryAndRepeatedCacheReads` |
| 内存过期版本遮挡旧 SST | `ExpiredMemTableVersionHidesOldSSTValue` |
| immutable 与快照语义 | `ImmutableMemTableUsesSameTTLAndSnapshotRules` |
| SST 过期版本遮挡更旧 SST | `ExpiredSSTVersionHidesOlderSSTValue` |
| 快照跨 Flush 固定时间及版本 | `SnapshotFixesBothSequenceAndTimeAcrossFlush` |
| 普通读采样一次、快照不采时钟 | `OrdinaryGetSamplesClockOnceAndSnapshotGetNeverSamplesIt` |

讲稿补充：WAL 与 SST 重开读取均测试原始 deadline；fake Env 控制时钟，避免靠 sleep 测微秒边界。

## 第 11 页｜阶段三：Iterator 的固定视图

**结论：迭代器创建时确定读取版本和时间，遍历中不再采时钟。**

架构图：

```text
DBIter：过滤不可见版本、删除和过期；返回用户 key/value
   ↓
MergingIterator：合并多个有序内部记录流
   ├─ mem Iterator
   ├─ imm Iterator
   └─ SST Iterators
```

正向源码摘录：`db/db_iter.cc`，`FindNextUserEntry`。

```cpp
if (IsExpired(expiration_micros, read_time_micros_)) {
  // The newest visible version expired: hide all older versions.
  SaveKey(ikey.user_key, skip);
  skipping = true;
  break;
}
```

不能只跳过过期的这一条，否则同 key 的旧值可能被下一次返回。

## 第 12 页｜阶段三：双向状态机与模型测试

反向遍历会先遇到旧版本，后遇到新版本；发现更新的可见版本已过期时，必须清除先前候选。

源码摘录：`db/db_iter.cc`，`FindPrevUserEntry`。

```cpp
if (IsExpired(expiration_micros, read_time_micros_)) {
  // Reverse scanning sees old versions first. Discard the candidate
  // when a newer visible version is expired, just as for deletion.
  value_type = kTypeDeletion;
}
if (value_type == kTypeDeletion) {
  saved_key_.clear();
  ClearSavedValue();
}
```

测试（共 9 项 Iterator 专项）：
- `MemTableSeekTraversalAndDirectionChanges`：Seek、正反向、方向切换。
- `SSTAndReopenHaveSameView`、`ExpiredImmutableVersionHidesOldSSTInBothDirections`。
- `ImplicitViewIsFixedBeforeFirstSeekAndAcrossCompaction`。
- `SnapshotUsesCreationTimeAndSequenceNotIteratorTime`。
- `EmptyAndAllExpiredAreNormalEndNotErrors`、`TimeIsSampledOnlyAtCreation`。
- `CorruptTTLStopsBothDirectionsAndErrorRemainsSticky`。
- `RandomizedViewsMatchIndependentLatestVersionModel`：独立模型比对，避免只验证手写样例。

## 第 13 页｜阶段四：统计抽象的职责分工

**结论：把全局状态、事务增量、局部构建工具、结果数据分离。**

| 抽象 | 范围/生命周期 | 作用 |
|---|---|---|
| `MetadataState` | DB 级、长期 | 全局 total 与每个 key 的引用计数 |
| `MetadataDelta` | 一次写入/压实 | 带符号的记录数和逐 key 变化 |
| `SourceMetadata` | 单 MemTable/SST | total_entries、distinct_keys 两个结果字段 |
| `SourceMetadataBuilder` | 有序扫描期间 | 构建局部摘要、逐 key 引用和 has_ttl |
| `Stats` | 查询输出 | total_entries、distinct_keys、seq |

示例：mem=[a,b]，SST=[a,a,c]，全局引用 `{a:3,b:1,c:1}`，total=5，distinct=3。

数据结构：`KeyReferenceCounts = map<string,uint64_t,UserKeyLess>`；按配置 Comparator 判断相等，而非简单字节字符串相等。

## 第 14 页｜阶段四：整批收集、一次发布

流程图：

```text
BuildBatchGroup → CollectWriteBatchMetadata → delta
                     ↓ 解析失败不提交
WAL append → 可选 Sync → InsertInto MemTable
                     ↓ 全部成功
锁内：metadata.Apply(delta) + SetLastSequence
```

源码摘录：`db/db_impl.cc`，`DBImpl::Write`。

```cpp
if (status.ok()) {
  status = metadata_.Apply(metadata_delta);
  if (status.ok()) {
    // One publication point for the whole group: statistics queries see
    // either the previous counts/sequence or all of this group's changes.
    versions_->SetLastSequence(last_sequence);
  } else {
    RecordBackgroundError(status);
  }
}
```

讲稿：收集对象必须是合并后的完整 batch；统计查询不能观察到“新计数+旧 seq”或半个 batch。WAL 出错后避免继续写入造成恢复和内存发布不一致。

## 第 15 页｜阶段四：O(1) 查询与增量校验

源码：`db/db_impl.cc`。

```cpp
Status DBImpl::GetCurrentStats(const ReadOptions& options, Stats* stats) {
  (void)options;  // 统计始终查询当前状态，不使用 ReadOptions::snapshot。
  if (stats == nullptr) {
    return Status::InvalidArgument("stats must not be null");
  }
  MutexLock lock(&mutex_);
  // Counts and sequence are copied under the same publication lock.
  *stats = metadata_.Snapshot(versions_->LastSequence());
  return Status::OK();
}
```

`MetadataState::Snapshot` 的三字段来源：

```cpp
result.total_entries = total_entries_;
result.distinct_keys = key_references_.size();
result.seq = sequence;
```

复杂度与代价：查询本体 O(1)，仍可能等待 mutex；增量更新需要有序 map 查找，状态保存需要逐 key 内存。不要宣称所有统计操作都是 O(1)。

Apply 先验证整批 total/key 的溢出和下溢，再修改；引用归零才从 map 删除。

## 第 16 页｜阶段四：并发与失败注入测试

选列测试名及断言：

| 测试 | 核心断言 |
|---|---|
| `CountsEveryOperationIncludingTTLAndDeletion` | 每种写操作均贡献一条记录 |
| `ComparatorEquivalentKeysShareOneReferenceCount` | 比较器等价 key 全局去重 |
| `RejectsBadDeltaBeforeChangingAnyState` | 非法 delta 不产生部分更新 |
| `MalformedBatchDoesNotWriteOrPublishAndNextWriteWorks` | 无效批次无写入/发布副作用 |
| `WALAppendFailureDoesNotPublishAndDisablesLaterWrites` | append 失败不发布 |
| `WALSyncFailureDoesNotPublishAndDisablesLaterWrites` | sync 失败不发布 |
| `QueryDuringBlockedWALSeesPreviousWholeBatch` | 阻塞期间只见上一完整状态 |
| `ConcurrentBatchesAndQueriesNeverExposePartialCounts` | 并发读不到半批计数 |

视觉：并发时间线，标记“锁外 I/O”与“锁内单一发布点”。

## 第 17 页｜阶段五：从 MemTable 到 SST 的局部统计

**结论：BuildTable 边写 SST 边重算摘要，不是直接复制计数。**

源码摘录：`db/builder.cc`。

```cpp
for (; iter->Valid(); iter->Next()) {
  key = iter->key();
  ParsedInternalKey parsed;
  if (!ParseInternalKey(key, &parsed)) {
    s = Status::Corruption("invalid internal key while building table");
    break;
  }
  builder->Add(key, iter->value());
  metadata_builder.Add(parsed.user_key, parsed.type == kTypeValueWithTTL);
}
```

图：冻结 MemTable → Iterator → 两条支路：TableBuilder 写记录 / SourceMetadataBuilder 算摘要。

`WriteLevel0Table` 对比重算摘要与 mem.metadata() 的 total、distinct，不一致则 Corruption。

重要：Flush 不给全局统计再加一遍；局部摘要保存到 FileMetaData，持久化载体是 MANIFEST，而非给 SST 新增统计块。

## 第 18 页｜阶段五：为什么要逐 key 引用摘要

反例：SST-A={a,b}，SST-B={a,c}；各 distinct=2，总 distinct=3，不是4。

源码：`db/metadata_stats.cc`。

```cpp
void SourceMetadataBuilder::Add(const Slice& user_key, bool has_ttl) {
  has_ttl_ = has_ttl_ || has_ttl;
  ++metadata_.total_entries;
  if (!has_previous_key_ ||
      comparator_->Compare(Slice(previous_key_), user_key) != 0) {
    ++metadata_.distinct_keys;
    previous_key_.assign(user_key.data(), user_key.size());
    has_previous_key_ = true;
    references_->emplace_back(previous_key_, 1);
  } else {
    ++references_->back().second;
  }
}
```

输入必须按用户 key 顺序分组。例 `a,a,b,c,c` → 摘要 total=5/distinct=3；references=[(a,2),(b,1),(c,2)]。

空间权衡：更快重建全局去重需要持久化逐文件 key 引用，增加 MANIFEST 大小和内存占用，不是免费的优化。

## 第 19 页｜阶段五：MANIFEST 格式演进与兼容

| tag | 含义 | 恢复策略 |
|---:|---|---|
| 7 | 原始 NewFile，无统计 | unknown，首次扫描升级 |
| 10 | 带 total/distinct | 局部摘要已知，缺逐 key 引用，首次扫描升级 |
| 11 | 带摘要、has_ttl、逐 key 引用 | 用引用摘要恢复全局统计 |

源码摘录：`db/version_edit.cc`。

```cpp
PutVarint32(dst, f.key_references ? kNewFileWithReferences
                                : (f.metadata_known ? kNewFileWithMetadata
                                                    : kNewFile));
```

图：文件基本信息 → total/distinct → flags(has_ttl) → distinct 条 `(key,count)`。

兼容关键：unknown 不等于零；解码每条新文件记录前重置 `FileMetaData`，避免混合新旧记录串状态；校验引用次数、总数与键顺序。

文件移层、MANIFEST 重写、Repair 都保留或重建摘要。

## 第 20 页｜阶段五：落盘、重写和 Repair 测试

| 验证点 | 测试名 |
|---|---|
| SST 构建与 MemTable 统计 | `SortedBuilderCountsRecordsAndAdjacentDistinctKeys` / `MemTableCountsArbitraryOrderAndComparatorEquivalence` |
| Flush 后统计不变 | `FlushPersistsExactSummaryWithoutChangingGlobalCounts` |
| 多次重开与 MANIFEST 重写 | `ManifestRewritesKeepFileMetadataAcrossRepeatedReopen` |
| WAL 恢复产生已知摘要 SST | `WALRecoveryBuildsSSTWithKnownMetadata` |
| 多个压实输出逐文件重算 | `CompactionRecomputesEachSplitOutputSummary` |
| Repair 重建与持久化 | `RepairRebuildsAndPersistsFileMetadata` |
| 旧格式只升级一次 | `LegacyUnknownIsUpgradedOnceForGlobalRecovery` |

另新增 4 个 VersionEdit 测试：新旧混合往返；已知零与 unknown 区分；截断/不一致拒绝；逐 key 引用往返与非法次数拒绝。

测试方法亮点：独立扫描实际 SST，与元数据摘要比较，避免“实现用同一套计数验证自己”。

## 第 21 页｜阶段六：TTL 回收的安全条件

**结论：已过期不代表可以立即物理删除。**

当前策略保守：存在活跃 Snapshot 时关闭 TTL 专项 GC；常规多版本压实仍按序列号边界保护快照。

源码摘录：`db/db_impl.cc`，`DoCompactionWork`。

```cpp
compact->compaction_time_micros = env_->NowMicros();
compact->allow_ttl_gc = snapshots_.empty();
```

```cpp
if (!drop && ikey.type == kTypeValueWithTTL && compact->allow_ttl_gc &&
    ikey.sequence <= compact->smallest_snapshot) {
  uint64_t expiration;
  Slice value;
  status = DecodeValueWithTTL(input->value(), &expiration, &value);
  if (!status.ok()) break;
  if (IsExpired(expiration, compact->compaction_time_micros) &&
      compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
    drop = true;
  }
}
if (drop) compact->metadata_delta.RemoveRecord(ikey.user_key);
```

讲稿：固定压实时间；检查序列号；更深层不能留旧值；旧迭代器通过引用保留旧数据源，当前统计只计算 current Version。

## 第 22 页｜阶段六：Version 安装与负增量原子发布

流程：输入记录 → drop 判定 → 收集负 delta → 构建输出 SST → MANIFEST/Version 安装 → 成功后应用负 delta。

源码摘录：`db/db_impl.cc`，`InstallCompactionResults`。

```cpp
Status s = versions_->LogAndApply(compact->compaction->edit(), &mutex_);
if (s.ok()) {
  // LogAndApply returns holding mutex_; no query can observe the installed
  // Version without also observing its exact physical-record delta.
  s = metadata_.Apply(compact->metadata_delta);
}
return s;
```

示例（示意）：压实前 total=100，实际 drop=30，安装成功后 total=70；安装失败仍为100。seq 不变。

补充实现：需要 TTL 回收时避免只做 trivial move；支持最深层原地重写，否则数据可能一直没有再次参与回收的机会。

## 第 23 页｜阶段六：启动重建，不重复扫描 WAL

**结论：重建依据恢复后的当前数据集合，而不是把历史日志再累计一次。**

```text
恢复 MANIFEST/current Version
        ↓
完成 WAL 恢复（可能保留 mem，也可能已 Flush 成 SST）
        ↓
对 current SST 合并逐 key 引用摘要
        +
只扫描仍存活的 mem/imm
        ↓
一次性重建 MetadataState
```

源码摘录：`db/db_impl.cc`，`RebuildMetadata`。

```cpp
for (MemTable* table : {mem_, imm_}) {
  if (table == nullptr) continue;
  std::unique_ptr<Iterator> iter(table->NewIterator());
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey key;
    if (!ParseInternalKey(iter->key(), &key)) {
      return Status::Corruption("invalid recovered MemTable key");
    }
    Status s = rebuild.AddRecords(key.user_key, 1);
    if (!s.ok()) return s;
  }
  if (!iter->status().ok()) return iter->status();
}
```

新格式正常重开不扫描 SST 数据；但仍需解析逐 key 引用，不能称启动 O(1)。旧 tag7/10 扫描一次升级并重写 MANIFEST。摘要错误或升级失败则 Open 失败。

## 第 24 页｜阶段六：恢复、回收、并发的联合测试

| 测试名 | 验证点 |
|---|---|
| `EmptyAndMixedWALRecoverExactCountsWithBothLogModes` | 不同 WAL 恢复模式下精确统计 |
| `SSTSummaryRestoresGlobalMapWithoutOpeningSST` | 新格式靠摘要重建，不打开 SST 数据 |
| `CrossSSTAndReusedMemTableDeduplicateWithoutWALCopies` | 跨源去重，不重复计入 WAL 副本 |
| `RecoveredComparatorEquivalentReferencesDeduplicate` | 恢复时仍遵守自定义 Comparator |
| `CompactionReclaimsExactVersionsAndSurvivesReopen` | 实际回收与重开计数一致 |
| `TTLLastLevelGCProtectsSnapshotAndPinnedIterator` | 最深层 GC 保护快照和旧迭代器 |
| `FailedCompactionInstallDoesNotSubtractItsDelta` | 安装失败不扣计数 |
| `QueryDuringCompactionManifestWriteSeesOldWholeState` | 发布前只见旧完整状态 |
| `ConcurrentWritersQueriesAndCompactionAgreeAtEnd` | 并发写/查/压实最终一致 |
| `InvalidPersistedReferenceOrderMakesOpenFail` | 拒绝损坏摘要 |
| `SyncedWritesSurviveExitWithoutDBDestruction` | sync 写后进程不析构退出仍恢复 |

若版面不足：正文选 6 项，完整表放附录；不要删掉异常退出测试与安装失败测试。

---

# 第三部分：测试方法与结果

## 第 25 页｜验证体系：从可控单元测试到公开评测

测试金字塔：
1. 可控时钟单元测试：边界、溢出、编码、状态机。
2. 故障注入集成测试：WAL append/sync、压实安装、MANIFEST 阻塞。
3. 独立模型与实际文件校验：随机 Iterator、扫描 SST 校验局部摘要。
4. 并发与进程恢复：整批发布、真实退出不析构。
5. 原有测试 + 公开 combined benchmark。

新增测试分布：

| 文件 | 新增数 |
|---|---:|
| `db/ttl_internal_test.cc` | 28 |
| `db/metadata_stats_test.cc` | 32 |
| `db/version_edit_test.cc` | 4 |
| **合计** | **64** |

注意：`version_edit_test.cc` 总共有5个 TEST，其中1个为原有 EncodeDecode，新增4个；不要误报新增65个。

## 第 26 页｜正确性结果汇总

| 测试层级 | 已确认结果 |
|---|---|
| Debug 配置/构建 | 成功 |
| 课程 `ttl_test` | 2/2 通过 |
| 课程 `metadata_test` | 3/3 通过 |
| 完整 CTest | 3/3 测试目标通过，0失败；92.79秒 |
| combined quick | 22/22 场景运行通过 |
| combined standard matrix | 22种场景，168次参数化/重复运行全部通过 |
| standard 内原生回归 | C++、C、POSIX 三项均 pass |

CTest 的3项是 `leveldb_tests`、`c_test`、`env_posix_test`；`leveldb_tests` 内含许多用例及新增测试，不是只有3个单元用例。

结果路径：
- quick：`build/evaluation-combined-quick-20261029-194510/`
- standard：`build/evaluation-combined-standard-20261029-195401/`

标准评测证据：`summary.json` 中 `status=pass`、`full_suite=true`，records 共168条全 pass，regression=pass。

## 第 27 页｜性能实验方法与可复现性

评测配置：
- `--kind combined --profile standard --matrix --jobs 2`。
- benchmark 独立构建类型：**Release**；不要与前面的 Debug 单元测试混淆。
- N=10,000 / 100,000；适用场景写线程=1 / 8；每组重复3次。
- value=256字节，基础 seed=7774596，三轮 seed 递增。
- TTL 联合场景过期比例0% / 50% / 100%。
- `--jobs 2` 是构建并行度，不是工作负载写线程数。
- `perf_stats` 是无写入查询；`perf_stats_under_write` 是普通写入对照；`perf_combined_stats_under_write` 是 TTL 写入对照。

配对方法：两个相同预加载数据库分别执行“仅写入”和“写入+持续统计查询”；一条查询线程，T条写线程。三轮交替前后顺序降低顺序偏差。

统计：吞吐、P99各取3轮中位数；额外给最小–最大范围。写入影响按每轮计算下降比例再取中位数，不把两个独立中位数之比误当配对中位数。

环境提醒：本机结果，不代表老师统一机器成绩。硬件/OS/编译器具体版本在提交前采集补入，不能凭空填写；测试时避免其他重负载。

## 第 28 页｜结果一：无写入时统计查询接近规模无关

数据来源：standard `aggregate.json`，`perf_stats/stats_query`。

| N | 查询吞吐中位数（百万次/秒） | 最小–最大 | P99中位数（μs） |
|---:|---:|---:|---:|
| 10,000 | 3.460 | 3.291–3.522 | 0.046 |
| 100,000 | 3.232 | 3.227–3.251 | 0.046 |

分析：数据规模10倍，吞吐下降约6.6%，尾延迟基本不变，符合直接读取已维护计数而非扫描数据库的设计。

图表：两根吞吐柱，带 min/max 误差线，旁边大字“10×数据量，约6.6%吞吐变化”。

重要脚注：吞吐包含驱动循环、校验和采样等工作；P99是采样接口调用延迟；不能用吞吐倒数直接解释为调用延迟，也不要把46ns当作跨机器保证。

## 第 29 页｜结果二：写入期间的统计查询开销

数据：`perf_stats_under_write/stats_under_write`。

| N | 写线程 | 查询吞吐中位数（百万次/秒） | 最小–最大 | P99（μs） |
|---:|---:|---:|---:|---:|
| 10,000 | 1 | 4.598 | 3.750–4.653 | 6.184 |
| 10,000 | 8 | 3.511 | 2.947–5.236 | 1.429 |
| 100,000 | 1 | 3.439 | 3.388–4.132 | 6.176 |
| 100,000 | 8 | 2.990 | 2.666–3.133 | 1.220 |

图表：按N分组的查询吞吐柱状图，区分1/8写线程；P99单独小图，不用不清晰的双轴。

分析：
- 持续写入下仍有约299万–460万次/秒统计查询。
- 相比静态场景，P99增大，说明O(1)计算不等于没有锁等待。
- 吞吐测量窗口和采样工作不同，不应把并发场景吞吐高于静态场景解释为写入“加速”查询。
- 8线程的P99较低不构成线程越多越快的证明，锁占用、批处理和调度会改变分布。

## 第 30 页｜结果三：查询对普通写入吞吐的影响

公式：`每轮下降率 = (1 - with_queries / without_queries) × 100%`，再取3轮中位数。

| N | 写线程 | 无查询写吞吐中位数（ops/s） | 有查询写吞吐中位数（ops/s） | 配对下降率中位数 | 配对下降率范围 |
|---:|---:|---:|---:|---:|---:|
| 10,000 | 1 | 206,890 | 188,110 | 9.08% | 0.82%–14.44% |
| 10,000 | 8 | 109,579 | 117,644 | -6.32% | -14.48%–0.82% |
| 100,000 | 1 | 217,111 | 208,249 | 7.51% | 3.18%–10.03% |
| 100,000 | 8 | 67,475 | 69,050 | -2.33% | -14.49%–8.14% |

分析：
- 单写线程下持续统计查询带来约7.5%–9.1%吞吐损失。
- 多写线程结果跨过0且波动大，不能证明查询提升了写性能，也不能严谨断言零开销。
- 8写线程整体吞吐低于单线程，说明本工作负载没有获得扩展收益；具体瓶颈需 profiling，不能仅从结果认定某一个模块。

图表：无/有查询分组柱，另用小图显示带正负号的配对下降率范围。

## 第 31 页｜结果四：TTL 与统计协同性能

展示50%过期比例；完整评测已覆盖0/50/100%，不是仅测50%。

| N | 写线程 | 无查询TTL写吞吐中位数 | 有查询TTL写吞吐中位数 | 配对下降率中位数 | 配对下降率范围 |
|---:|---:|---:|---:|---:|---:|
| 10,000 | 1 | 188,203 | 159,941 | 18.93% | 15.02%–30.25% |
| 10,000 | 8 | 66,981 | 68,325 | -2.01% | -14.69%–19.74% |
| 100,000 | 1 | 204,896 | 164,834 | 19.53% | 16.05%–33.22% |
| 100,000 | 8 | 69,997 | 61,592 | 11.33% | 3.01%–14.37% |

TTL写入时统计查询：

| N | 写线程 | 查询吞吐中位数（百万次/秒） | P99（μs） |
|---:|---:|---:|---:|
| 10,000 | 1 | 2.910 | 8.001 |
| 10,000 | 8 | 4.039 | 1.030 |
| 100,000 | 1 | 1.716 | 8.516 |
| 100,000 | 8 | 2.285 | 2.189 |

结论：TTL单线程持续查询影响约19%；大规模8线程约11.3%；小规模8线程波动不足以支持提升结论。

版面建议：主文保留写入影响表，查询明细移到讲稿或附录，避免一页两个大表。

## 第 32 页｜总结与局限

完成：
1. TTL贯通写入、点查、快照、双向迭代器、WAL/SST恢复。
2. 增量精确统计，计数与seq原子发布。
3. 每文件摘要及逐key引用持久化，正常恢复不扫描新格式SST数据。
4. 安全压实回收、负增量提交、异常恢复与旧格式升级。
5. 64项补充测试；课程测试、完整回归、公开standard矩阵全部通过。

性能结论：查询本体O(1)，无写入约323万–346万次/秒；持续查询有可测写入影响，尤其单线程TTL场景。

局限/后续工作：
- 存在快照时TTL GC保守关闭，可能延后回收。
- 全局逐key map与MANIFEST引用摘要增加内存和持久化成本。
- mutex争用、线程扩展性需profiling进一步定位。
- 三轮结果有波动，不宣称显著性或统一机器成绩。
- 未提供原生baseline标准性能对照，不能声称统计维护对原生LevelDB的总开销已测清。

结束语：**时间决定可见性，实际记录决定统计，发布点决定一致性。**

---

# 附录（可选4页）

## 附录 A｜构建与测试命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DLEVELDB_BUILD_TESTS=ON -DLEVELDB_BUILD_BENCHMARKS=OFF
cmake --build build -j2
(cd build && ./ttl_test)
(cd build && ./metadata_test)
mkdir -p build/test-data
TEST_TMPDIR="$PWD/build/test-data" \
  ctest --test-dir build --output-on-failure
```

只运行补充测试的过滤命令（需先构建）：

```bash
./build/leveldb_tests --gtest_filter='TTL*:*Metadata*:*SourceMetadata*'
./build/leveldb_tests --gtest_filter='VersionEditTest.*'
```

说明：第二条包含一个原有测试，不能把其5个结果全称为新增测试。

## 附录 B｜公开评测与结果文件

```bash
python3 benchmark/runner.py --kind combined --source "$PWD" \
  --output "$PWD/build/evaluation-combined-quick-$(date +%Y%m%d-%H%M%S)" \
  --profile quick --jobs 2

python3 benchmark/runner.py --kind combined --source "$PWD" \
  --output "$PWD/build/evaluation-combined-standard-$(date +%Y%m%d-%H%M%S)" \
  --profile standard --matrix --jobs 2
```

每次指定新输出目录。standard默认每组repeat=3，不需要为了满足三次重复完整执行三遍。

文件用途：summary.json=状态和逐轮记录；metrics.csv=逐轮指标；aggregate.json=中位数和范围；各场景run.log=失败诊断；configure/build.log=构建证据。

## 附录 C｜TTL GC额外数据

数据来源：`perf_combined_gc/ttl_gc_with_statistics`；吞吐表示处理输入记录数/压实阶段时间，不是“每秒删除条数”。

| N | 过期比例 | 处理吞吐中位数（ops/s） | 压实调用耗时中位数约（ms） |
|---:|---:|---:|---:|
| 10,000 | 0% | 736,824 | 13.57 |
| 10,000 | 50% | 862,336 | 11.59 |
| 10,000 | 100% | 1,095,100 | 9.13 |
| 100,000 | 0% | 853,230 | 117.20 |
| 100,000 | 50% | 937,813 | 106.63 |
| 100,000 | 100% | 1,125,250 | 88.87 |

趋势：过期比例更高时，保留输出减少，测试中压实阶段更快；这只是一组负载下的观察，不推广为所有场景必然规律。

注意：该场景每轮只采样一次完整GC调用，指标里的P99不能解释为大量独立GC请求的尾延迟，上表仅作调用耗时使用。

## 附录 D｜答辩常见问题

**Q：为什么过期时不立即减统计？**
A：统计是当前数据源中的内部记录数，过期只改变读取结果；物理记录被当前Version移除后才扣。

**Q：旧迭代器仍引用旧SST，为什么新统计可以减少？**
A：全局统计定义针对current Version，不包含退休Version；引用计数保证旧视图物理文件仍安全存活。

**Q：为什么不直接相加SST distinct？**
A：跨文件有同key不同版本，必须全局去重；逐key引用能正确处理新增和最后一次删除。

**Q：为什么不只持久化一个全局计数？**
A：还需要恢复全局去重引用关系，并与文件集合变化、WAL恢复匹配；逐文件摘要有明确归属。

**Q：为什么metadata不写SST里面？**
A：当前方案在FileMetaData中管理，借MANIFEST持久化；不要混同SST数据内容与SST对应的元信息。

**Q：standard运行一次为什么有三次结果？**
A：profile自带repeat=3，matrix展开参数组合，每组合对应r1/r2/r3。

**Q：多线程下“负开销”是否意味着查询提高写性能？**
A：不能。三次结果范围跨0，可能受调度、缓存、后台活动影响，需要更多重复和profiling才能判断。

---

# 制作前核对清单

- [ ] 封面身份信息补全，机器配置采用实际采集信息。
- [ ] 六阶段与原七阶段的合并关系已标明。
- [ ] 将TTL到期、物理回收、统计扣减三个时刻区分。
- [ ] 未将快照历史读取语义错误套用给统计接口。
- [ ] 所有代码标注文件与函数，省略片段不伪装为完整实现。
- [ ] 64项是新增测试数；168是standard参数化重复运行次数；22是场景种类，三者不混用。
- [ ] Debug用于本地测试，Release用于本次benchmark，区分清楚。
- [ ] 每组3次、中位数、范围、配对下降率计算方法已说明。
- [ ] 未声称已经完成原生baseline性能对照或统计显著性检验。
- [ ] 未把单次GC调用的P99当作请求延迟分布。
