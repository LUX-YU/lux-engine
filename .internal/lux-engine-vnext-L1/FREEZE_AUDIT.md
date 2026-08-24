# vNext L1 v2.1 Independent Audit Rejection — 2026-08-24

## 结论

独立审阅发现 Change Journal、Hierarchy recovery 与 cold/resync 路径仍有
correctness blocker。当前状态撤回为：

- `Architecture Accepted`
- `Freeze Candidate Rejected by Independent Audit`
- `Targeted Hardening Required`
- `Domain Migration Blocked`

此前的 correctness/performance 数据只证明部分 hot path，不构成冻结结论。
重新审阅前，Render、Physics、Animation、Script、
Streaming 等 domain migration 以及建立在 L1 之上的 L2/L3 产品工作继续 STOP。

v2.1 定向整改进度：Change Journal 的 history-loss 已改为 pin-safe epoch，Schedule
scratch 已改为 wave-wide bounded arena；Hierarchy cold rebuild 已改为线性三阶段
校验，incremental sibling mutation 为 O(1)，orphan cleanup 由内嵌 repair queue
重试。cold construction 与最终代表性证据尚未完成，因此本文件状态仍保持 rejected。

## 构建与测试矩阵

| 矩阵 | 结果 |
|---|---|
| Windows RelWithDebInfo | `target all -j 4 -k 0` 通过；CTest 66/66；第二轮 `ninja: no work to do` |
| Windows Debug | `target all -j 4 -k 0` 通过；CTest 53/53；第二轮 `ninja: no work to do` |
| Windows Hardened Contracts | 独立 RelWithDebInfo tree，`LUX_ECS_CONTRACT_CHECKS=ON`；CTest 66/66；第二轮 no-work |
| Android arm64 PLAYER | L0 + L1 `target all -j 4 -k 0` 通过；第二轮 no-work；交叉构建不运行目标侧 CTest |
| Fresh install | 空 staging lineage 安装 534 个文件；installed-architecture gate 通过 |
| Installed consumers | `core+schedule`、`core+schedule+object`、`schema_reflection+persistence` 三个独立工程均配置、链接、运行通过 |

Android 的独立 engine/lux-cxx 前缀曾揭示 component annotation 的隐式 meta
include。最终 `ComponentAnnotations.hpp` 由 `ecs::schema` 安装，Parent/Transform
不再取得 optional reflection adapter closure；source gate 阻止该依赖回归。

## v2 关键契约

- `World` 公开面只读；`WorldEdit` 与 declared `SystemFrame` Write capability 是
  canonical mutation 入口。bounded Change Journal 使用全 World 4 KiB block
  arena、epoch/sequence cursors 和确定性的 overflow/resync。
- Snapshot instantiate、LXWS materialize 建立 fresh baseline；restore 保留目标
  `WorldConfig`、复用 journal blocks，并递增 epoch。
- Schedule 的 execution DAG、access partition 与 lifetime DAG 分离；同 concrete
  type 多实例、phase barriers、hard `require`、stale/cross handle、stop/remove
  frontier 与 rollback 均有测试。
- Schedule 不 include/link Object。Object affinity validator 在 consumer TU
  type-erase；pure schedule installed consumer 不获得 Object/reflection closure。
- Schema 的 snapshot policy、codec 与 reflection projection 正交；
  `schema_reflection` 是独立 optional component。
- Parent 是 hierarchy 唯一 truth；HierarchyIndex 与 WorldTransform 都从 change
  streams 增量重建。no-change 与 leaf-dirty 的访问计数进入性能 gate。
- LXWS v1 magic/version 保持不变；所有结构字段使用 explicit little-endian
  primitives，真实多 archetype/component round trip、corruption 和 limits 通过。
- 负向编译 probes 覆盖 mutable World get/query、`SystemFrame::world()`、旧
  Registry/observer/attach/System extrinsic API、`SceneServices`、`ISystem` 和
  `ScheduleBuilder`。

## 性能

原始 30-sample CSV、方法和 median/p95 见
`../benchmarks/20260824-vnext-l1/l1-v2-benchmark.csv`。

- 100k/1m ReadQuery median 相对 raw EnTT 分别为 -9.33%/-7.54%，没有超过约
  5% steady-state overhead 上限。
- Schedule 1/16/64/256/1024 systems 和 reserved commands steady path 的
  allocation-event 计数为 0。
- 1m hierarchy no-change 的 visited nodes 为 0；100k-entity World 的 Transform leaf
  dirty visited nodes 为 1，而不是 World size。
- Snapshot 1m capture/instantiate/restore median 为
  88.789/97.090/94.916 ms；LXWS 的真实 mixed-data 1m encode/decode median 为
  305.950/418.042 ms。

## Quarantine 与 STOP 条件

- `legacy/` 仍不参与 configure、compile、install、link、codegen 或 package，且
  本轮没有物理删除。
- Physics、Render ECS、Animation、Audio、Input、Script、Navigation、Streaming
  等未迁移；L2 TaskSystem/AssetStore/AssetClient/AssetLease/ExtensionLoader 未创建。
- 只有独立审阅接受本 Freeze Candidate，且后续首个新 L3 headless Scene 完成
  construction、tick、snapshot/restore 与 persistence round trip 后，才可另立
  domain migration 或 legacy 删除工作。
