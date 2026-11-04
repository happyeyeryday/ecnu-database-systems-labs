# ECNU Database Systems Labs

华东师大数据库课程实验整理，2026年秋。

每个项目使用独立分支，项目源码、报告与相关材料放在对应分支根目录；`main`提供项目导航。

## 项目导航

| 项目 | 主题 | 分支 | 当前状态 |
| --- | --- | --- | --- |
| Project1 | LevelDB TTL与统计元数据 | [project1-leveldb-ttl-metadata](https://github.com/happyeyeryday/ecnu-database-systems-labs/tree/project1-leveldb-ttl-metadata) | 功能已实现并完成本地核验；汇报PPT为待修订原稿 |

后续项目按实际题目新增`project2-主题`等分支。

## Project1资料

- [源码与构建说明](https://github.com/happyeyeryday/ecnu-database-systems-labs/tree/project1-leveldb-ttl-metadata)
- [实验报告](https://github.com/happyeyeryday/ecnu-database-systems-labs/blob/project1-leveldb-ttl-metadata/实验报告.pdf)
- [可编辑PPT原稿](https://github.com/happyeyeryday/ecnu-database-systems-labs/blob/project1-leveldb-ttl-metadata/presentation/original.pptx)
- [PPT原稿PDF](https://github.com/happyeyeryday/ecnu-database-systems-labs/blob/project1-leveldb-ttl-metadata/presentation/original.pdf)
- [作业要求](https://github.com/happyeyeryday/ecnu-database-systems-labs/blob/project1-leveldb-ttl-metadata/requirements/project1.pdf)
- [PPT分级修改建议](https://github.com/happyeyeryday/ecnu-database-systems-labs/blob/project1-leveldb-ttl-metadata/doc/presentation_review.txt)
- [原机器standard评测证据](https://github.com/happyeyeryday/ecnu-database-systems-labs/tree/project1-leveldb-ttl-metadata/evaluation/original-standard-20261029)
- [本地quick核验记录](https://github.com/happyeyeryday/ecnu-database-systems-labs/tree/project1-leveldb-ttl-metadata/evaluation/local-quick-20261103)

## 实现与验证

Project1在Google开源LevelDB基础上扩展TTL与统计元数据，覆盖写入、读取、快照、迭代器、持久化恢复和Compaction回收。

按实验报告，小组分工为朱昭荣（10234304416）负责metadata维护，徐卓一（10235102432）负责TTL与测试汇报。

2026-11-03本地核验结果：metadata课程测试3/3、TTL课程测试2/2、内部测试273项通过和2项可选压缩测试跳过、课程固定原生回归通过、公开combined quick场景22/22通过。

本地核验由Codex在WSL2环境使用GCC/G++直接编译执行；原机器standard历史结果和本地quick核验分别存放，具体方法、AI参与和范围见项目分支说明。

## 来源与正式提交

代码基础来自[Google LevelDB](https://github.com/google/leveldb)，项目分支保留原许可证、作者信息和提交历史。课程材料没有标注改编自哪门国外课程，本仓库不作未经确认的课程来源归属。

本仓库用于项目展示和资料分享。正式课程提交保留在[码园个人仓库](https://gitea.shuishan.net.cn/CDMS.XuanZhou.2026Fall.Dase/10234304416)。
