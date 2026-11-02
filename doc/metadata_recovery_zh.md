# 全局统计恢复与 Compaction 回收：最终接入

## 1. 现在满足的统计范围

`DBImpl::metadata_` 表示当前 MemTable、Immutable MemTable、当前 Version 有效 SST
中的内部记录。旧版本、删除标记、过期但未回收的 TTL 都计数；WAL 副本不重复计数，
被旧迭代器引用但已经退出当前 Version 的 SST 也不再计入当前统计。

写入发布、查询和 Compaction 安装都使用同一把 DB mutex。`seq` 是全库最新已发布序列号，
时间流逝与记录回收不推进它。查询忽略 ReadOptions::snapshot，空 stats 返回 InvalidArgument。

## 2. 持久化的不是只有两个整数了

每个新 SST 的 FileMetaData 现在包含：

```text
metadata：total_entries、distinct_keys
key_references：[用户键 → 该文件中的内部记录数]
has_ttl：文件中是否包含 TTL 记录
```

`SourceMetadataBuilder::Add` 在原有 SST 顺序写入中收集相邻用户键的引用数。
BuildTable、Compaction 和 Repair 都产生这个摘要。摘要通过 shared_ptr 在内存中的
FileMetaData 副本间共享，发布后不再修改，避免每个 Version 深拷贝全部键。

例如两个文件保存：

```text
文件 100：a→3，b→1
文件 101：a→2，c→4
```

全局合并得到 `a→5，b→1，c→4`，于是 total=10、distinct=3。
这才包含恢复精确跨文件去重所需的信息，不能只相加两个文件的 distinct。

## 3. MANIFEST tag 11

`db/version_edit.cc` 增加新文件记录 tag 11：

```text
层级、文件号、大小、最小/最大内部键
total_entries、distinct_keys
has_ttl 标志
distinct_keys 组 [长度前缀用户键、varint64 引用数]
```

解码检查非零引用数、总数一致、长度和标志是否合法；恢复时用用户 Comparator
检查键严格有序、没有 Comparator 等价重复键，累加检查有符号 delta 容量溢出。
MANIFEST 的既有日志校验和保护这些字节。坏摘要不能当成零或悄悄忽略，Open 返回错误。

旧 tag 7（无计数）、tag 10（仅两个计数）仍可读取。
MANIFEST 快照、文件直接移层完整复制引用摘要，Compaction 输出使用实际保留记录的新摘要。
旧版程序不认识 tag 11，不能用未修改的旧程序读取这种新 MANIFEST。

## 4. DB::Open 如何恢复全局 metadata_

位置：`db/db_impl.cc`，`DBImpl::RebuildMetadata`。

```text
VersionSet::Recover 重放 MANIFEST
  ↓
RecoverLogFile 重放有效 WAL
  ↓
安装恢复期间生成的 SST 和日志边界
  ↓
RebuildMetadata（尚未向用户开放 DB，也未启动后台合并）
  ├─ 遍历 current Version 的有效 SST
  │    └─ 从 FileMetaData.key_references 合并全局逐键引用数
  ├─ 扫描恢复后仍存活的 mem_/imm_ 内部记录，补齐其贡献
  ├─ 必要时将旧文件升级后的摘要写入新 MANIFEST
  └─ 一次应用 rebuild delta
  ↓
成功才返回 DB 指针并启动正常后台工作
```

正常的新格式 SST 不需要打开数据文件。测试把 Env 的 SST 打开直接设为失败，
重开仍能恢复正确统计，证明不是在后台偷偷全量扫描数据。

WAL 重放有两种结果：记录留在 MemTable，或者已刷成新的 SST。
本方法只统计最后实际存在的来源，不另外再加一份 WAL，所以不会重复统计。
即使 TTL 已过期也照样计入；读取过滤不等于物理回收。

### 旧库如何升级？

只有缺少逐键摘要的文件走兼容路径：顺序扫描该文件、核对已有计数、生成摘要。
全部成功后 `VersionSet::RewriteManifest` 把当前文件集合及新摘要同步到新 MANIFEST，
成功切换 CURRENT 后再开放 DB。升级失败则 Open 失败。
以后重启就直接读取摘要，不再扫描这些已升级文件。

升级不是往当前 Version 重复 AddFile，而是在启动期更新文件描述后写完整新快照，
避免把同一个 SST 重复安装到某一层。

## 5. 崩溃一致性为什么成立？

不新增独立的全局 METADATA 日志：

- 新 SST 的摘要与“新增这个 SST”的文件记录在同一条 MANIFEST 变更内。
- SST 先完成写入与同步，再提交 MANIFEST；未被 MANIFEST 引用的输出是孤儿，不参与恢复。
- Compaction 的输入删除与输出新增在同一 VersionEdit 中提交。
- 最近写入仍由已有 WAL 恢复，sync=true 的成功写入不依赖正常析构保存统计。

因此崩溃后恢复的是 MANIFEST 确认的当前文件集合，再加有效 WAL 的剩余记录，
不是“某个可能滞后的全局计数文件”。即使一次 MANIFEST Sync 返回错误，重开时可能
恢复旧集合或完整的新集合，但统计始终从实际恢复出来的来源重建。

异常退出测试在子进程内确认 sync=true 的 Put/Delete/WriteBatch 成功后 `_Exit`，
不执行 DB 析构；重开按独立操作记录核对 total、distinct、seq 以及 TTL 原到期时间。

## 6. Compaction 如何扣除全局统计？

位置：`DBImpl::DoCompactionWork` 和 `InstallCompactionResults`。

CompactionState 新增局部负 `MetadataDelta`。每条实际 `drop` 的内部记录调用：

```cpp
compact->metadata_delta.RemoveRecord(ikey.user_key);
```

锁外生成输出时不修改全局状态。安装时：

```cpp
Status s = versions_->LogAndApply(edit, &mutex_);
if (s.ok()) s = metadata_.Apply(compact->metadata_delta);
```

LogAndApply 返回时已重新持有 mutex，因此文件集合切换和扣减对查询是一个发布动作。
安装失败时不应用负 delta。回收不调用 SetLastSequence。

测试刻意阻塞 Compaction 的 MANIFEST 写入，查询看到完整旧状态；放行后看到完整新状态。
另一个测试注入 MANIFEST Sync 失败，验证计数和当前内部记录都没有被提前扣减。

## 7. TTL 回收与最深层

Compaction 开始时持锁固定时间；存在活跃 Snapshot 时保守禁止 TTL 特有回收。
TTL 过期、序列号安全且没有更深层旧值时才 drop，随后同组更老版本也按原规则丢弃，
防止旧值复活。文件含 TTL 时不走纯移层 shortcut，进入真正重写路径。

迭代器继续引用创建时的 MemTable/Version，因此当前 Version 安装新文件、扣减统计后，
旧迭代器仍能读取自己的固定视图。旧引用保留的文件不计入当前全局统计。

最深层不能再移到下一层，新增原地重写路径：读取选定范围、生成同层新文件、原子替换。
这类手动任务一次处理完整选定范围并结束，不能反复选中新输出导致无限重写。
CompactRange 会补充 TTL 文件/最深层的重写，测试可用 TEST_CompactRange(6, ...) 明确触发。

可重复回收验证：停止写入 → 释放 Snapshot/迭代器 → TEST_CompactMemTable →
逐层 TEST_CompactRange(0..6) → 等待调用返回 → 用内部迭代器核对记录和 GetCurrentStats。
TEST_CompactRange 等待后台工作完成；不要用用户迭代器验证物理记录数量。

## 8. 开销与尚需测量的取舍

令 N 为内部记录数，K 为全局去重键数，U 为当前所有 SST 的局部去重键数之和。

- 合并当前文件摘要约 O(U log K + 内存表剩余记录处理)，另有 MANIFEST/WAL 重放成本；
  历史 MANIFEST 变更也需解码，不能把总启动开销只按当前 U 估算。
  新格式不读取 SST 中的 value 或逐个旧版本。
- 旧格式升级才需要一次 O(N) 数据扫描。
- 全局 map 仍需 O(K) 内存，文件摘要另占 O(U) 键/引用数空间；不同文件的同键仍各存一份。
- MANIFEST 大小和重写成本增加到 O(U)，不只是每文件两个整数。
- SST 写入原有扫描中顺便收集摘要，批量记录同键引用；查询仍为锁内 O(1)。

这不是声称恢复 O(1)，也不是声称持久化摘要没有代价。大型库可以进一步把摘要放入
专门的 SST 元数据块或独立文件来减小 MANIFEST，但必须保留原子文件集合提交与校验。

## 9. 验证入口

```bash
cmake --build build --target leveldb_tests metadata_test ttl_test -j2
./build/metadata_test
./build/leveldb_tests --gtest_filter='MetadataRecoveryTest.*:MetadataFileTest.*:VersionEditTest.*'
./build/leveldb_tests
```

新增恢复/回收测试包括精确正常重开、两种日志复用模式、跨 SST 与内存去重、
Comparator 等价键、无 SST 打开恢复、旧库一次升级、损坏摘要拒绝、回收后重开、
最深层 TTL、活跃视图保护、失败安装不扣减、并发写入/查询/合并及异常退出。

统一自测使用新输出目录，quick 重复三次用于开发验证；正式性能报告仍需 standard/matrix
与相同设备上的基线比较，不能把开发 quick 的一次运行当作性能结论。

### 本次验证结果（2026-10-29）

- `metadata_test`：3/3 通过，包含 PersistAndRecover。
- `ttl_test`：2/2 通过，在独立临时数据库目录运行。
- `leveldb_tests`：275 项运行，273 项通过，2 项压缩依赖相关测试跳过。
- combined quick，N=10000、线程参数=4、value=256 字节、每场景重复3次：
  22 个场景共66次运行全部通过，full_suite=true；随附 C++/C/Env 原生回归均通过。
- 原始评测目录：`build/evaluation-metadata-recovery-quick-r3-20261029/`，
  汇总在 `summary.json`，各阶段测量在 `metrics.csv` 和 `aggregate.json`。

这次 quick 为功能开发自测，运行期间也进行了其他回归，没有隔离机器负载，
并未进行原生性能基线或 standard/matrix 对照，不能据此宣称性能提升或完成正式性能报告。
