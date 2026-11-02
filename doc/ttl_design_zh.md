# LevelDB TTL 框架设计

本文说明 `db/ttl_internal.{h,cc}`、`kTypeValueWithTTL` 和 Snapshot 时间字段的用途。目前已完成 TTL 写入、恢复、Get、迭代器，以及保护读取视图的 Compaction 回收和最深层 SST 重写。逐处解读见 [`ttl_stage2_walkthrough_zh.md`](ttl_stage2_walkthrough_zh.md)、[`ttl_stage3_walkthrough_zh.md`](ttl_stage3_walkthrough_zh.md) 和 [`metadata_recovery_zh.md`](metadata_recovery_zh.md)。

## 1. TTL 要解决两个不同问题

TTL（time to live）表示一条记录可以被读取到什么时候：

```text
expiration_micros = write_time_micros + ttl_seconds * 1,000,000
```

当 `read_time_micros >= expiration_micros` 时记录已经过期。

必须区分：

- 逻辑过期：Get 返回 NotFound，迭代器跳过记录。
- 物理回收：Compaction 真正不再把记录写入输出 SST。

时间到达只产生逻辑过期，不会立即修改 SST，也不会改变统计元数据。统计只在物理回收成功后减少。

## 2. 为什么保存绝对到期时间

如果只保存 `ttl=20 秒`，数据库重开时无法知道应该从第一次写入还是本次启动重新计时。框架在写入接口被调用时使用 `options_.env->NowMicros()`，立即计算绝对时间并持久化：

```text
fixed64 absolute_expiration_micros | original_user_value
```

WAL 恢复、MemTable 刷盘、SST 合并都只复制这个值，不重新计算，所以重开不会延长 TTL。

`ComputeExpirationTime` 同时检查：

- `ttl_seconds * 1,000,000` 是否溢出；
- 加到当前时间后是否溢出。

溢出必须在进入 `DBImpl::Write`、分配序列号之前返回 `InvalidArgument`。

## 3. 为什么需要独立内部类型

普通 value 可以是任意二进制。如果只约定“value 前 8 字节像时间就是 TTL”，普通值可能被误判。因此新增：

```cpp
kTypeDeletion     = 0
kTypeValue        = 1
kTypeValueWithTTL = 2
```

内部键类型明确告诉读取路径是否需要解释前 8 字节。`kValueTypeForSeek` 必须同步改为最大的合法类型 2，否则相同序列号下的 Seek 边界可能跳过 TTL 类型。

新增格式仍能读取旧数据库；但写入 TTL 后，未修改的旧版 LevelDB 不认识类型 2，因此不能用旧程序打开并正确读取新数据。

## 4. 写入框架

四参数接口最终应由 `DBImpl` 覆盖：

```cpp
Status DBImpl::Put(const WriteOptions& options, const Slice& key,
                   const Slice& value, uint64_t ttl_seconds) {
  uint64_t expiration;
  Status s = ComputeExpirationTime(env_, ttl_seconds, &expiration);
  if (!s.ok()) return s;

  WriteBatch batch;
  WriteBatchInternal::PutWithTTL(&batch, key, value, expiration);
  return Write(options, &batch);
}
```

不能直接写 MemTable，因为标准 `Write` 同时负责 Writer 排队、序列号、WAL、sync 和批次原子性。

阶段一已给 WriteBatch 内部格式增加 TTL tag，并扩展：

- `WriteBatch::Iterate`：解码 TTL tag；
- `MemTableInserter`：用 `kTypeValueWithTTL` 调用 `MemTable::Add`；
- `WriteBatchInternal::PutWithTTL`：编码绝对时间和用户 value。

WAL 本身保存序列化 WriteBatch，因此完成这一层后异常恢复会自动保留原到期时间。

TTL 为 0 不是“不写入”。它的 expiration 等于写入时间，仍写 WAL、MemTable，分配序列号并增加统计，只是在任何 `read_time >= expiration` 的读取中立即不可见。

## 5. Get 读取框架

一次 Get 必须只取得一次时间：

- 指定 Snapshot：使用 Snapshot 创建时保存的 `read_time_micros`；
- 未指定 Snapshot：在 Get 开始时调用一次 `env_->NowMicros()`。

找到第一个序列号可见的 TTL 内部记录后：

1. 用 `DecodeValueWithTTL` 取得 expiration 和用户 value；
2. 未过期则只返回用户 value，不返回前 8 字节；
3. 已过期则返回 NotFound，并停止查找。

第三点非常重要。假设先写普通旧值，再写 TTL 新值，新值过期后不能重新暴露旧值。过期 TTL 的读取语义类似删除标记，会遮住更老版本。

需要把固定 read time 从 `DBImpl::Get` 传给：

- `MemTable::Get`；
- `Version::Get`；
- SST 查询回调 `SaveValue`。

格式短于 8 字节说明持久化数据损坏，应返回 Corruption，而不是当作过期。

## 6. Snapshot 和迭代器

原始 Snapshot 只保存序列号。框架已经让 `SnapshotImpl` 同时保存：

```cpp
sequence_number
read_time_micros
```

`GetSnapshot` 在 `DBImpl::mutex_` 下同时取得最后序列号和当前时间。因此快照的版本视图与时间视图都固定。

DBIter 也需要增加 `read_time_micros` 构造参数：

- 没有 Snapshot 时，在 `NewIterator` 创建时固定当前时间；
- 有 Snapshot 时使用快照时间；
- 迭代过程中不能反复调用 NowMicros，否则同一迭代器扫描到一半会改变结果。

正向遍历遇到过期 TTL 时，应像 deletion 一样跳过该键及其老版本。反向遍历遇到过期 TTL 时，应像 deletion 一样清空已保存的候选 value。有效 TTL 只向用户暴露去掉 8 字节头部后的 value。

## 7. Compaction 回收

Compaction 开始时固定一个 `compaction_time_micros`。TTL 回收必须同时满足：

- 记录在该时间已经过期；
- 活跃快照不再需要它在过期前的视图；
- 删除它不会让更深层的旧值重新出现。

第一版建议采用保守规则：存在任何活跃 Snapshot 时不执行 TTL 特有回收，并要求 `IsBaseLevelForKey(user_key)`。保守规则可能晚一点释放空间，但不会错误删除快照仍需读取的数据。

已创建的迭代器会引用旧 MemTable/Version，因此 Version 切换后仍能读取自己固定的旧视图。

在 `DoCompactionWork` 的 `drop` 路径中：

```cpp
if (drop) {
  compact->metadata_delta.RemoveRecord(ikey.user_key);
}
```

只有 `InstallCompactionResults` 成功后才能应用统计负 delta。Compaction 失败、时间自然流逝和普通 MemTable 刷盘都不能减少统计。

## 8. 恢复路径

TTL 信息会经过：

```text
WriteBatch TTL tag
  -> WAL
  -> MemTable kTypeValueWithTTL + encoded value
  -> SST 原样保存
  -> Compaction 原样复制未回收记录
```

`RecoverLogFile` 已经通过 `WriteBatchInternal::InsertInto` 重放 WAL，因此不应调用当前时间，也不应重新计算 expiration。MANIFEST 只管理 SST 文件集合，不需要重复保存每条记录的 TTL。

## 9. 与统计元数据的协同规则

| 事件 | TTL 读取结果 | total_entries | distinct_keys | seq |
| --- | --- | ---: | ---: | ---: |
| TTL Put 成功 | 到期前可读 | +1 | 可能 +1 | +1 |
| TTL=0 Put 成功 | 立即不可读 | +1 | 可能 +1 | +1 |
| 时间到期 | 不可读 | 不变 | 不变 | 不变 |
| Compaction 实际回收 | 不可读 | -1 | 引用归零才 -1 | 不变 |
| TTL 计算溢出 | 无写入 | 不变 | 不变 | 不变 |
| MemTable 刷 SST | 不变 | 不变 | 不变 | 不变 |

## 10. 推荐实现顺序

1. 扩展 WriteBatch TTL tag 和 `DBImpl::Put(..., ttl)`，验证零 TTL、溢出、WAL 重放。
2. 修改 MemTable/SST Get，验证过期遮住旧值。
3. 给 DBIter 传固定时间并实现正反向过滤。
4. 实现 Snapshot 时间读取语义。
5. 实现保守的 Compaction 回收，并连接统计负 delta。
6. 补刷盘、重开、异常退出、快照、Seek、正反向遍历和 combined 测试。
