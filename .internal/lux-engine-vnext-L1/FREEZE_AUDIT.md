# vNext L1 v2.1 Independent Re-audit Candidate — 2026-08-24

## 结论

独立审阅发现的 Change Journal、Hierarchy recovery 与 cold/resync blockers 已完成
定向加固；全部 correctness、代表性性能、平台和安装资格门已重跑。当前状态为：

- `Architecture Accepted`
- `Correctness Hardened`
- `Performance Contract Passed`
- `Public API Freeze Candidate`
- `Independent Re-audit Required`

这不是 Frozen 声明。独立重新审阅接受前，Render、Physics、Animation、Script、
Streaming 等 domain migration 以及建立在 L1 之上的 L2/L3 产品工作继续 STOP。

v2.1 定向整改进度：Change Journal 的 history-loss 已改为 pin-safe epoch，Schedule
scratch 已改为 wave-wide bounded arena；Hierarchy cold rebuild 已改为线性三阶段
校验，incremental sibling mutation 为 O(1)，orphan cleanup 由内嵌 repair queue
重试。Snapshot/LXWS cold construction 现已使用 internal suppressing edit，并由 journal
lifetime counters 证明 construction record writes 与额外 block acquisitions 都为零。
deterministic failure injection 覆盖首次 stream descriptor、block acquire/attach、
Schedule scratch exhaustion 和 orphan command failure；历史丢失统一降级为 pin-safe
resync，没有 partial history publication。

## 构建与测试矩阵

| 矩阵 | 结果 |
|---|---|
| Windows RelWithDebInfo | `target all -j 4 -k 0` 通过；CTest 67/67；第二轮 `ninja: no work to do` |
| Windows Debug | `target all -j 4 -k 0` 通过；CTest 54/54；第二轮 `ninja: no work to do` |
| Windows Hardened Contracts | 独立 RelWithDebInfo tree，`LUX_ECS_CONTRACT_CHECKS=ON`；CTest 67/67；第二轮 no-work |
| Android arm64 PLAYER | L0 + L1 `target all -j 4 -k 0` 通过；第二轮 no-work；交叉构建不运行目标侧 CTest |
| Fresh install | 空 staging lineage 安装 583 个文件；installed-architecture gate 通过 |
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
`../benchmarks/20260824-vnext-l1/l1-v2.1-benchmark.csv`。

- 100k/1m ReadQuery median 相对 raw EnTT 分别为 -13.90%/-9.51%，没有超过约
  5% steady-state overhead 上限；1m WorldEdit/Schedule WriteQuery median 为
  8.975/15.623 ms，warmup 后 allocation count 为 0。
- Schedule 1/16/64/256/1024 systems 和 reserved commands steady path 的
  allocation-event 计数为 0。
- 1m real balanced Parent graph initial sync median 为 51.096 ms，no-change visited
  nodes 为 0；100k deep/star initial sync 为 4.584/4.279 ms，100k star reparent
  sibling walk 为 0，cursor overflow resync 为 4.434 ms。
- Transform 100k star/deep propagation median 为 16.112/13.455 ms；1m peak/100k
  active sparse high-water retained dense bytes 为 32,400,024，作为独立审阅输入保留。
- Snapshot 1m capture/instantiate/restore median 为
  48.108/47.245/47.254 ms；LXWS 的真实 mixed-data 1m World build、encode、decode、
  materialize median 为 1498.140/277.687/300.200/328.938 ms。

## Quarantine 与 STOP 条件

- `legacy/` 仍不参与 configure、compile、install、link、codegen 或 package，且
  本轮没有物理删除。
- Physics、Render ECS、Animation、Audio、Input、Script、Navigation、Streaming
  等未迁移；L2 TaskSystem/AssetStore/AssetClient/AssetLease/ExtensionLoader 未创建。
- 只有独立审阅接受本 Freeze Candidate，且后续首个新 L3 headless Scene 完成
  construction、tick、snapshot/restore 与 persistence round trip 后，才可另立
  domain migration 或 legacy 删除工作。
