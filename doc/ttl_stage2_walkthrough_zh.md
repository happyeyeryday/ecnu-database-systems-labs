# 阶段二：Get 与快照时间——实现解读

## 1. 阶段目标

阶段一已经保存 `kTypeValueWithTTL` 和 `[fixed64 到期时间 | 原始 value]`。
阶段二让点查 `Get` 理解这些信息，规则为：

- 读取时间小于到期时间：返回原始 value。
- 读取时间大于或等于到期时间：返回 `NotFound`，不再寻找旧值。
- 使用 Snapshot：同时使用快照的序列号和创建时间。
- 普通值保持原样，不解码时间头部。
- TTL 头部不足 8 字节：返回 `Corruption`。

本阶段不实现用户迭代器过滤、不物理删除过期记录、不更新统计。

## 2. 全流程

```text
DBImpl::Get
  │ 持 mutex_，确定读取视图 (sequence, read_time)
  │ 引用 mem_、imm_、current Version，防止读取期间被销毁
  │ 释放 mutex_
  ↓
LookupKey(user_key, sequence)
  ↓
MemTable::Get(..., read_time, ...)
  ├─ 命中普通值：返回值，结束
  ├─ 命中 TTL：解码、判断，成功/NotFound/Corruption，均结束
  ├─ 命中删除标记：NotFound，结束
  └─ 没有该视图可见的记录：继续
  ↓
Immutable MemTable::Get(..., 同一 read_time, ...)
  │ 同样的规则；只有未命中才能继续
  ↓
Version::Get(..., 同一 read_time, ...)
  ↓ 寻找可能包含该键的 SST
TableCache::Get → Table::InternalGet → SaveValue
  │ TTL 解码与判断；命中过期记录后停止查其他文件
  ↓
重新持 mutex_，处理查询统计，释放对象引用，返回状态
```

WAL 只用于恢复，不参与正常 Get。读取缓存也不会改变上述过期规则。

## 3. DBImpl::Get：固定读取视图

位置：`db/db_impl.cc` 的 `DBImpl::Get`。

### 实现

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

取得这两个字段时仍持有 `mutex_`，不会有写入在两次取值之间发布新的序列号。
普通 Get 只取一次当前时间，随后传给每个存储层；快照 Get 不获取当前时间。

### 为什么不能让每层自行调用 NowMicros？

一次 Get 可能先查 mem、再查 imm、最后访问磁盘，过程中时间会变化。
如果每层重新取时间，Get 的 TTL 视图会取决于数据恰好在哪一层、磁盘访问有多慢。
调用方固定时间，可以使整次查询使用同一个判断基准。

### 为什么还要保留引用计数？

文件读取不能一直持有全局锁，否则慢 I/O 会阻塞写入。
但解锁后，后台任务可能替换内存表或安装新的 Version。
因此沿用原流程：锁内 `Ref`，锁外读取，重新加锁后 `Unref`。
这是对象生存期保护，不是 TTL 判断本身。

## 4. LookupKey：先选版本，后判时间

`LookupKey(key, snapshot_sequence)` 构造的是内部查找键。
同一用户键的内部版本按序列号从大到小排列，因此 Seek 会跳过比视图序列号新的记录，
定位到该视图能看到的最新版本。

之后才能判断这个版本的 TTL。不能先过滤掉所有过期版本，再选一个未过期旧值。

例如：

```text
seq=1: key → old（普通值）
seq=2: key → new（TTL 已过期）
```

当前读取选中 seq=2，结果为 NotFound，而不是 old。
序列号为 1 的快照则仍可以选中 old。

已有框架把 `kValueTypeForSeek` 设为包含 TTL 类型的最大合法类型，
这是 Seek 在同序列号边界不漏掉 TTL 记录的前提。

## 5. MemTable::Get：区分“未命中”和“命中但不可读”

位置：`db/memtable.h`、`db/memtable.cc`。

为 `Get` 增加 `uint64_t read_time_micros` 参数，并在类型分支处理 TTL：

1. 读取该记录的编码 value。
2. 调用 `DecodeValueWithTTL`，解析绝对到期时间和用户 value。
3. 解码失败：设置 Corruption，返回 true。
4. `read_time >= expiration`：设置 NotFound，返回 true。
5. 未过期：复制用户 value 到输出字符串，返回 true。

这里的布尔值与 Status 含义不同：

| 布尔返回值 | Status | 含义 | 是否继续查下一层 |
| --- | --- | --- | --- |
| false | 不作为最终结果 | 没有该视图可见的匹配记录 | 是 |
| true | OK | 找到有效值 | 否 |
| true | NotFound | 找到删除标记或过期值 | 否 |
| true | Corruption | 找到损坏的 TTL 值 | 否 |

过期时返回 false 会导致 DBImpl 继续读旧 SST，造成旧值复活。
损坏时返回 false 同样会掩盖数据错误。

`DecodeValueWithTTL` 返回的 Slice 借用底层内存，因此有效值要复制到调用者的
`std::string`；不能让结果依赖 MemTable 的后续生存期。

当前 MemTable 和 Immutable MemTable 使用同一个类，所以同一实现覆盖两条路径。

## 6. SST：通过 Saver 把读取时间送进回调

位置：`db/version_set.h`、`db/version_set.cc`。

### 为什么要改 Version::Get 和 Saver？

`DBImpl` 知道读取视图，`SaveValue` 才拿到 SST 中的键值。
两者之间隔着 Version、TableCache 和 Table 的回调机制。

因此给 `Version::Get` 增加时间参数，再保存进回调上下文 `Saver`。
无需改 SST 格式、TableCache 接口或 Table 的通用读取代码。

### SaveValue 的状态机

普通值直接返回；TTL 值先解码，然后决定状态：

| 状态 | 含义 | Version 后续动作 |
| --- | --- | --- |
| kNotFound | 当前文件未找到匹配用户键 | 继续查其他候选文件 |
| kFound | 找到普通值或未过期 TTL | 停止，返回 OK |
| kDeleted | 找到删除标记或已过期 TTL | 停止，最终返回 NotFound |
| kCorrupt | 内部键损坏或 TTL 头部无效 | 停止，返回 Corruption |

最关键的是把过期 TTL 映射为 `kDeleted`，而不是 `kNotFound`。
Level 0 可以有范围重叠的文件，查询按从新到旧的文件顺序进行；更深层也可能存在旧值。
因此发现过期的新版本后，必须阻断继续查找。

`kDeleted` 只是本次查询的状态，不会把 SST 中的 TTL 记录改写成删除标记。

### 为什么缓存不会返回过期值？

TableCache 和 block cache 缓存的是文件/块相关对象及数据，不是这个键的最终 TTL 可见性。
即使读取命中缓存，SaveValue 仍会按本次固定时间解码并判断。

## 7. Snapshot：序列号和时间缺一不可

位置：`db/snapshot.h`、`DBImpl::GetSnapshot`。

已有框架已经提供不可变的 `sequence_number_`、`read_time_micros_`，
并在创建快照时持锁保存两者，因此本阶段不需要再改 Snapshot 结构。
新增的工作是在 Get 中真正使用已保存的时间。

```text
t=0：写入 key=value，TTL=20 秒，seq=1
t=10：创建 S1 = (seq=1, time=10)
t=20：创建 S2 = (seq=1, time=20)
t=30：读取
```

- 普通 Get：使用 time=30，NotFound。
- 使用 S1：time=10 小于到期时间，读到 value。
- 使用 S2：time=20 恰好到期，NotFound。

S1 和 S2 的序列号相同，但读取结果不同。这说明 TTL 可见性是由
`(序列号, 时间)` 共同决定的，而不是仅由序列号决定。

快照还限制“之后的写入不可见”。因此创建 S1 后即使覆盖为普通新值，
S1 也仍读取自己序列号范围内的旧 TTL 版本，并以自己的时间判断。

调用者仍必须遵守原有规则：Get 使用快照期间不能释放该快照。

## 8. 测试如何证明正确

位置：`db/ttl_internal_test.cc`，新增 `TTLReadMemTableTest` 与 `TTLReadTest`。

- 直接测试 MemTable，区分 miss、有效值、过期命中和损坏命中。
- 可控时钟设为到期前 1 微秒、恰好到期、到期后，避免 sleep 带来的不确定性。
- 普通二进制值、TTL 二进制值和空值验证编码不会泄露或误判。
- 分别在 mem 和 SST 里保存过期的新版本，验证旧值不会复活。
- 暂停后台刷盘，轮换 mem 为 imm，验证 Immutable MemTable 的过期与快照语义。
- 验证相同序列号、不同时间的快照返回不同结果；刷盘和后续覆盖不改变原快照视图。
- 统计 Env 的时钟调用次数：普通 Get 一次，快照 Get 零次。
- 先预热 SST 缓存再推进时间，验证可见性没有错误地被缓存。
- 分别从 WAL、SST 重开，验证读取仍按原始到期时间判断。

复现命令，在仓库根目录运行：

```bash
cmake --build build --target leveldb_tests ttl_test -j2
./build/leveldb_tests --gtest_filter='TTLRead*'
./build/leveldb_tests
```

课程基础点查测试可以单独运行 `ttl_test --gtest_filter=TestTTL.ReadTTL`。
该程序使用相对路径 `testdb`，建议从独立临时目录启动，避免污染现有测试库。
`CompactionTTL` 属于后续物理回收阶段，不作为阶段二验收条件。
