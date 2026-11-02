# 阶段三：TTL 迭代器——流程与实现原理

## 1. 目标和整体结构

迭代器只向用户展示某个固定视图下的键值，不展示内部版本、删除标记、过期记录，
也不展示 TTL 的 8 字节时间头。

```text
DB::NewIterator(ReadOptions)
  ↓ DBImpl::NewIterator
NewInternalIterator
  ├─ mutex_ 内取得最新序列号与读取时间
  ├─ 收集 mem、imm、当前 Version 的内部迭代器
  ├─ 用 NewMergingIterator 合并成内部有序记录流
  └─ 引用这些存储对象，并注册销毁时释放引用的回调
  ↓
NewDBIterator(sequence, read_time)
  ↓ 创建 DBIter，保存固定视图
Seek / SeekToFirst / SeekToLast / Next / Prev
  ↓ 选出视图可见版本、处理删除与 TTL
用户键 + 原始用户 value
```

与 Get 不同，迭代器不是每次移动都重新查最新数据库。
创建后，即使时间推进或发生新写入，原迭代器的视图也不能变化。

## 2. 创建时固定时间，而不是第一次 Seek 时

修改位置：`db/db_impl.h/.cc` 的 `NewInternalIterator` 和 `NewIterator`。

给 `NewInternalIterator` 增加读取时间输出参数，在原来已持有的 `mutex_` 下执行：

```cpp
*latest_snapshot = versions_->LastSequence();
*read_time_micros =
    options.snapshot != nullptr
        ? static_cast<const SnapshotImpl*>(options.snapshot)->read_time_micros()
        : env_->NowMicros();
```

普通迭代器使用创建时的当前时间；显式快照迭代器使用 Snapshot 保存的时间。
在选取存储对象的同一锁区间取时间，避免序列号、时间和存储对象分别在不同阶段捕获。

`NewIterator` 随后选择显式快照序列号或刚捕获的最新序列号，把序列号和时间传给
`NewDBIterator`。Snapshot 的这些字段不可变，无需重新计算。

例如 t=10 创建迭代器，t=30 才第一次调用 Seek，而记录在 t=20 到期：
这个旧迭代器仍按 t=10 判断，能读到该记录；t=30 新建的迭代器则跳过它。

`TEST_NewInternalIterator` 同步适配了新输出参数，但仍返回不做用户级过滤的内部记录流。
它不会因为 TTL 到期而隐藏物理记录，便于后续核验回收与统计。

## 3. DBIter 保存什么？

修改位置：`db/db_iter.h/.cc`，工厂函数、构造函数和成员字段。

```cpp
SequenceNumber const sequence_;
const uint64_t read_time_micros_;
Slice current_value_;
```

- `sequence_`：过滤创建视图之后发布的新版本。
- `read_time_micros_`：始终用同一时间判断 TTL，不能在移动或 Seek 时重读时钟。
- `current_value_`：正向扫描时的用户值切片；TTL 值已去掉时间头。
- 原有 `saved_value_`：反向扫描时复制保存的用户值，不再保存 TTL 原始编码。

普通二进制值完全不解码。是否存在 TTL 由内部键类型决定，而不是猜测 value 内容。

## 4. 正向扫描：先遇到新版本

修改位置：`DBIter::FindNextUserEntry`。

内部排序大致是：用户键升序，同一用户键的序列号降序。

```text
a@seq=8（新）
a@seq=5（旧）
b@seq=9
b@seq=3
```

对每条内部记录：

1. 解析内部键；序列号大于 `sequence_` 的记录不可见，跳过。
2. 删除标记：记录要跳过的用户键，屏蔽后面的同键旧版本。
3. 普通值或 TTL 值：如果正处于跳过同键旧版本的状态，继续跳过。
4. 否则解码 TTL；损坏时设置 Corruption，停止本次定位。
5. 若已过期，像删除标记一样进入 skipping 状态。
6. 若有效，设置 `current_value_`、`valid_=true`，停在该内部记录上。

关键分支：

```cpp
if (IsExpired(expiration_micros, read_time_micros_)) {
  SaveKey(ikey.user_key, skip);
  skipping = true;
  break;
}
```

例如 a 的新 TTL 版本过期，不能只 `Next()` 一次，否则接下来遇到 a 的普通旧值，
就会错误返回它。必须跳过整个 a 的旧版本组，再寻找后面的可见用户键。

## 5. 反向扫描：先遇到旧版本

修改位置：`DBIter::FindPrevUserEntry`。

反向读取同一组内部版本时，顺序与正向相反：

```text
a@seq=1：old
a@seq=2：new，TTL 已过期
```

会先遇到 old，因此不能立即返回它。算法先把它暂存为候选，然后继续扫描同键更新版本：

- 更新版本是普通值：覆盖候选。
- 更新版本是有效 TTL：用解码后的用户值覆盖候选。
- 更新版本是删除标记或已过期 TTL：清除候选。
- 序列号超出视图：忽略，不能影响视图内的候选。

这次实现先把过期 TTL 的局部 `value_type` 视作 `kTypeDeletion`，再复用原有清空逻辑：

```cpp
if (IsExpired(expiration_micros, read_time_micros_)) {
  value_type = kTypeDeletion;
}
if (value_type == kTypeDeletion) {
  saved_key_.clear();
  ClearSavedValue();
}
```

只有扫完该用户键的可见版本、确认仍有候选值后，才向用户返回。
若候选被删除，就继续寻找更小的用户键。

因此两种方向做的是同一件事，但操作不同：

| 方向 | 先看到哪个版本 | 过期新版本的处理 |
| --- | --- | --- |
| 正向 | 新版本 | 阻止同键旧版本成为结果 |
| 反向 | 旧版本 | 清除已暂存的同键旧值 |

## 6. value() 为什么分正反向实现？

修改位置：`DBIter::value()` 及正反向扫描结果保存。

正向扫描找到结果后，底层迭代器仍停在该记录上，因此可以直接引用其用户值：

```cpp
current_value_ = user_value;
```

TTL 解码得到的 Slice 只借用底层内存，不分配新字符串；普通值也沿用这种方式。

反向扫描必须继续越过整组版本，才能知道最后哪个版本有效。
此时底层迭代器已经离开候选记录，甚至已经切换 SST 数据块，因此不能保留临时 Slice：

```cpp
saved_value_.assign(user_value.data(), user_value.size());
```

最终：

```cpp
return direction_ == kForward ? current_value_ : Slice(saved_value_);
```

用户取得的 key/value Slice 仍遵守 LevelDB 原有生命周期约定：移动迭代器后可能失效，
需要长期使用时应自行复制。

## 7. Seek 和方向切换怎么工作？

没有另写一套 Seek 的 TTL 逻辑，而是复用两种扫描函数：

| 操作 | 定位/过滤流程 |
| --- | --- |
| Seek(target) | 用固定序列号构造内部键 → 内部 Seek → FindNextUserEntry |
| SeekToFirst() | 内部首记录 → FindNextUserEntry |
| SeekToLast() | 内部末记录 → FindPrevUserEntry |
| Next() | 调整底层位置 → FindNextUserEntry，跳过当前用户键 |
| Prev() | 调整底层位置 → FindPrevUserEntry |

Seek 的含义仍然是“第一个大于或等于 target 的可见用户键”。
如果 target 正好对应一个过期键，应定位到后面的有效键，而不是立即结束。

方向切换沿用原有 DBIter 规则：

- 正向模式底层停在当前结果记录，切换 Prev 时先越过当前键的版本组。
- 反向模式底层已位于当前结果键之前，切换 Next 时先回到当前组，再跳过整组。

TTL 参与的是这之后的可见性判断，因此不用重写方向切换机制。

## 8. 错误、结束与旧视图的保护

正常跳过过期记录、扫描到末尾：`Valid()==false` 且 `status().ok()`。
遇到损坏 TTL：停止当前定位，`status().IsCorruption()`。
解码成功不能覆盖之前的错误，所以使用局部 Status，只有失败时才赋给 `status_`。

已创建迭代器的内部清理状态持有 mem、imm、Version 引用，销毁迭代器时才释放。
后续写入由固定序列号过滤；刷盘/合并安装新的 Version 后，旧引用保护旧视图所需的存储对象。
测试同时保留基于旧 MemTable 和旧 SST 的迭代器，进行覆盖、刷盘、手动合并后再扫描。

本阶段没有新增 TTL 物理回收规则。后续 Compaction 阶段仍必须审查新的回收逻辑，
确保不破坏活跃 Snapshot 与迭代器；不能仅凭“当前时间已过期”就删除其所需数据。

## 9. 验证方法

`db/ttl_internal_test.cc` 新增 `TTLIteratorTest` 的 9 项测试：

1. MemTable 中 Seek、正反向遍历、方向切换；混合普通值、TTL、删除、覆盖、空值、二进制值。
2. SST 与重开后的结果一致。
3. 暂停后台刷盘，让过期新版本留在 imm，验证不暴露 SST 旧值，以及旧 Snapshot 仍可见。
4. 创建后不立即 Seek，推进时间并覆盖、刷盘、合并，验证 MemTable/SST 两类旧迭代器视图。
5. 显式 Snapshot 的时间和序列号，而不是 NewIterator 时的当前时间，决定结果。
6. 空库和全部过期的结果是正常结束。
7. 普通迭代器创建时取一次时间，后续移动不取时间；显式 Snapshot 迭代器不取当前时间。
8. 损坏 TTL 的正反向错误处理，后续成功解码不清除已记录错误。
9. 固定随机种子，800 次写入/删除操作；独立模型按最新用户操作和到期时间计算预期结果，
   在时间推进、后续写入及部分刷盘合并后，校验新旧迭代器的双向扫描与方向切换。

在仓库根目录运行：

```bash
cmake --build build --target leveldb_tests -j2
./build/leveldb_tests --gtest_filter='TTLIteratorTest.*'
./build/leveldb_tests
```
