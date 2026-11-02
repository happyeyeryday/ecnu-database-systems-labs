# 阶段五：文件统计持久化，以及重启恢复的边界

说明：本文记录阶段五最初只保存两个计数的版本。后续已实现本文第八节讨论的
逐文件键引用摘要（MANIFEST tag 11）与全局恢复，最新流程见
[`metadata_recovery_zh.md`](metadata_recovery_zh.md)。文中的“尚未实现”是阶段五时的状态。

## 1. 先回答：重启必须全库重新扫描吗？

不必须。全量扫描内部记录可以正确重建，但会读大量 SST，启动成本较高。
持久化足够的统计信息后，正常重启可以改为读取统计摘要并重放尚未刷盘的 WAL。

不过要区分三种东西：

| 数据 | 当前保存位置 | 阶段五是否持久化 |
| --- | --- | --- |
| 每个 SST 的 total_entries、distinct_keys | FileMetaData → MANIFEST | 是 |
| 全局 user_key → 内部记录引用数 | DBImpl::metadata_ 中的 map | 否 |
| 最近尚未刷盘的写入 | WAL | 原本已持久化，可在恢复时派生统计 |

阶段五完成的是第一项，不代表第三项恢复后全局 map 也已自动恢复。
在全局恢复方案接入前，重开后的 GetCurrentStats 仍不能作为完整功能验收。

## 2. 为什么只保存两个文件计数不够？

例如：

```text
SST-1：a、b → total=2，distinct=2
SST-2：a、c → total=2，distinct=2
```

全局 total=4，但 distinct=3，不是 2+2=4。
另一个数据库的 SST-2 如果是 c、d，它的两个文件计数完全相同，但全局 distinct=4。
因此，仅从两个整数无法推断文件间的键重叠关系。

当前全局 MetadataState 还需要知道每个键有多少条内部记录，以便以后回收一个版本时
正确决定引用数是否归零。只保存全局 distinct 一个整数也不足以恢复这个数据结构。

## 3. 本阶段写入与刷盘流程

```text
MemTable::Add
  ├─ 用已有 skiplist 查该用户键是否已出现
  ├─ 插入内部记录
  └─ 更新 SourceMetadata（total 加一；新键时 distinct 加一）
  ↓ 冻结为 imm 或恢复时准备刷盘
BuildTable
  ├─ 按内部键顺序写 SST
  ├─ 解析每条内部键
  └─ SourceMetadataBuilder 顺带计算记录数与相邻去重键数
  ↓
核对 MemTable 与 SST 的统计相等
  ↓
VersionEdit::AddFile(..., metadata, metadata_known)
  ↓
LogAndApply → MANIFEST 持久化 → 安装新 Version
```

刷盘只移动记录，不改变全局 metadata_，也不推进序列号。
TTL=0、删除标记、旧版本都计入局部统计，不能用用户迭代器计算。

## 4. MemTable 为什么不用额外的 set？

位置：`db/memtable.h/.cc`。

SourceMetadata 只保存两个整数。Add 在插入前用最大序列号 LookupKey 查已有 skiplist，
检查是否存在 Comparator 等价的用户键。
这样支持任意写入顺序、重复写入、删除不存在键、大小写等价 Comparator，
又无需给每个 MemTable 再复制一份去重键集合。

代价是每次 Add 多一次 skiplist 查找。metadata() 只允许在该表不再被写入时读取，
例如刷盘或单线程测试；它不是活动 MemTable 上的无锁并发统计查询接口。
正常用户查询仍走 mutex_ 保护的 GetCurrentStats。

## 5. SST 如何计算局部计数？

位置：`db/builder.cc`。

SST 输入已经按内部键排序，相同用户键的版本连续出现，因此：

- 每写一条记录，total 加一。
- 当前用户键与上一个用户键按用户 Comparator 不等时，distinct 加一。

只保存前一个键即可，不需要全文件 set。比较的是用户键，不能用包含序列号的内部键。
options.comparator 在构建内部 SST 时是 InternalKeyComparator，需要取出其中的用户 Comparator。

构建、同步、关闭、校验或输入迭代失败时，不设置 known=true；损坏内部键使本次 BuildTable 失败。
只有最终成功的文件才获得可信摘要。

## 6. MANIFEST 编码与恢复

位置：`db/version_edit.h/.cc`。

FileMetaData 新增：

```cpp
SourceMetadata metadata;
bool metadata_known = false;
```

新文件记录格式：

```text
tag=10
level、file_number、file_size、smallest、largest
varint64 total_entries
varint64 distinct_keys
```

旧 tag=7 继续保留原格式。解码时，旧记录得到 known=false，而不是“已知为零”。
同一个 VersionEdit 可以混用新旧格式；解码复用的 FileMetaData 必须逐条重置，
否则旧记录可能继承上一条新记录的 known 标志。

新程序可以读取旧格式；旧版程序不认识新 tag，不能据此声称新库能被旧程序读取。
截断统计、distinct 大于 total、正 total 配零 distinct 会返回 Corruption。

重启的文件统计恢复链路是：

```text
CURRENT → 找到 MANIFEST
  ↓
读取 MANIFEST 中的 VersionEdit 日志
  ↓
DecodeFrom 解析新增文件、删除文件、序列号和文件摘要
  ↓
VersionSet::Builder 应用文件集合变更
  ↓
当前 Version 的每个 FileMetaData 已包含原统计
```

这一步只读 MANIFEST，不需要打开 SST 数据文件。测试通过统计 SST 文件打开次数验证了这一点。
MANIFEST 中的“删除文件”会使旧文件退出当前集合，因此恢复时不会把所有历史文件都累加。

原有 WAL 恢复仍走 WriteBatch → MemTable::Add；如果恢复过程产生新 SST，BuildTable 也会
生成并持久化它的文件摘要。相同写入不能同时按 WAL 副本和 SST 再计两次。

## 7. 哪些路径必须保留摘要？

- `VersionSet::WriteSnapshot`：新 MANIFEST 要完整复制当前文件摘要及 known 标志。
  这里的 snapshot 是“文件集合快照”，不是 ReadOptions 中的用户快照。
- Trivial move：同一个 SST 仅改变层级，摘要不变，必须一并复制。
- 真正 Compaction：输入中可能丢弃旧版本，因此按每个输出文件重新计算摘要。
  每新建一个输出文件都有独立的 SourceMetadataBuilder，不能把前一个文件的计数带过去。
- Repair：扫描/重写 SST 时重建摘要，并写入新 MANIFEST；若保留了无法解析的内部键，
  标记 unknown，不把不完整统计伪装成精确值。

本阶段计算了 Compaction 输出文件的局部计数，但尚未应用全局回收负 delta，那是阶段六。

## 8. 后续如何避免扫描全部 SST？

更适合当前设计的方案是持久化每个 SST 的逐键引用摘要，例如：

```text
SST-1 摘要：a → 3，b → 1
SST-2 摘要：a → 2，c → 4
合并恢复：  a → 5，b → 1，c → 4
```

正常启动流程可以变成：

1. 重放 MANIFEST，确定当前有效 SST 集合。
2. 读取这些文件的逐键摘要，合并得到全局引用 map；不读取 SST 中的 value 和每个旧版本。
3. 按已有恢复规则重放有效 WAL，补齐尚未体现在 SST 中的记录；恢复中已刷成 SST 的部分不能重复加。
4. 所有状态准备好后再开放数据库。

逐键摘要可以存储在 SST 的专用元数据块或独立摘要文件里，但必须满足：

- 摘要对应明确的 SST 文件号和格式版本，并有校验。
- SST 与摘要都持久化完成后，才能让 MANIFEST 引用它们。
- Compaction 安装新文件时同步切换摘要；旧文件退出当前 Version，旧摘要也退出统计范围。
- 崩溃留下的未引用摘要按孤儿文件处理，不能计入统计。
- 旧库缺少摘要时，需要一次兼容性扫描/升级，不能把 unknown 当零。

这是建议的后续方案，阶段五尚未实现逐键摘要。
因为全局 map 本身占 O(K) 内存，重启时把 K 个键装入内存仍有成本；
优化目标是避免扫描所有内部记录和值，不是让恢复无限数据量都变成 O(1)。

另一种方案是持久化全局统计检查点加增量重放。但检查点必须绑定 MANIFEST 状态、
有效 WAL 范围和回收进度，不能只记一个 seq：Compaction 不推进 seq，却会改变记录数。
不能简单在每次 Put 后另写一个 METADATA 文件，否则在 WAL 成功与统计文件更新之间崩溃，
两者就可能不一致。仅在正常 Close 时保存也不能覆盖异常退出。

## 9. 验证

新增 11 项测试，分布于 `db/metadata_stats_test.cc` 和 `db/version_edit_test.cc`：

- MemTable 任意写入顺序与 Comparator 等价键。
- 有序输出摘要计数。
- 新旧 MANIFEST 格式混合、known=0 与 unknown 区分、损坏编码拒绝。
- 刷盘前后全局统计不变，文件统计正确。
- 连续三次重开并确认 MANIFEST 实际换代，摘要仍保留。
- WAL 恢复生成 SST 的摘要。
- Compaction 多文件输出的每文件统计。
- Repair 重建持久化统计。
- 旧格式 unknown 经过恢复与 MANIFEST 重写后仍然是 unknown。

文件摘要不仅前后比较，还在测试中用独立 set 和物理记录扫描核对。
这个扫描仅用于测试；此前先断言 MANIFEST 恢复过程没有打开 SST。

```bash
cmake --build build --target leveldb_tests -j2
./build/leveldb_tests --gtest_filter='MetadataFileTest.*:SourceMetadataTest.*:VersionEditTest.*'
./build/leveldb_tests
```
