# vNext L1 Freeze Audit — 2026-08-24

## 结论

L1-0 至 L1-8 本轮范围（其中 L1-8 仅指安装、性能与 freeze gate，不含 domain migration）已闭环。`legacy/` 保留为 quarantine；由于首个新 L3 headless Scene 尚不属于本轮，未执行物理删除。

## 构建与测试

| 矩阵 | 结果 |
|---|---|
| Windows RelWithDebInfo | `target all -j 4 -k 0` 通过；CTest 55/55；第二轮 `ninja: no work to do` |
| Windows Debug | `target all -j 4 -k 0` 通过；CTest 42/42；第二轮 `ninja: no work to do` |
| Android arm64 PLAYER | L0 + L1 `target all -j 4 -k 0` 通过；第二轮 `ninja: no work to do`；交叉构建不运行目标侧 CTest |
| Fresh install | 空 staging prefix 安装 522 个文件，installed-architecture gate 通过 |
| Installed consumer | 仅用 `find_package(lux-engine-resource/ecs COMPONENTS ...)` 和公开 alias 编译、链接、运行通过 |

三套 `compile_commands.json` 的 legacy path 与退休 API 命中数均为 0。安装树与 manifest 不含 legacy、`sinclude/pinclude`、旧 Registry/System/asset runtime 头。

## 关键契约

- 单 World/单 live Schedule、跨 Schedule handle、stale generation、required type/set、phase contradiction、cycle、stable tie-break、reverse close frontier均有测试。
- `System` 与 `LuxObject` 正交；Object System 固定 owner-thread singleton lane；Event handler 只写 inbox，下一 tick 才通过 commands 修改 World。
- Snapshot 验证 allocator/entity bits/generation/free-list/next allocation，Copy/Rebuild 分流，以及 live Schedule/observer 下 restore 拒绝。
- LXWS v1 验证 deterministic bytes、unknown field/schema、version migration port、local/stable relocation、截断/损坏/aggregate limits 和 round trip。
- hierarchy + transform pilot 验证 reparent/cycle、dirty subtree、destroy、任意安装顺序、snapshot rebuild 与 persistence round trip。
- 旧 `.luxasset` v1 fixture 和 LUXPAK v2 size/SHA-256 golden 均通过新的 manager-less L0 reader。
- 负向编译 probes 验证 `World::registry()`、Registry header、`SceneServices`、`ISystem` 与 `ScheduleBuilder` 均不可用。

## 性能

原始 CSV、方法和 median/p95 见 `../benchmarks/20260824-vnext-l1/`。

- 100k entity、20 次遍历：raw EnTT median 989.2 µs，`World::view` 988.0 µs，差值 -0.12%。
- Schedule steady run 与 command arena warm common path 的 allocation-event 计数为 0。
- Snapshot 与 LXWS 的 10k/100k/1m 数据呈线性扩展，实体 hot loop 中没有 reflection/string lookup。

## Quarantine 与 Git

- Git 索引将迁移表达为 1007 个历史 rename：旧 ECS 373、旧 Engine 553、退休 asset runtime 81；没有残留的删除+未跟踪对。
- 既有三个安装前缀中的旧 ECS/asset public surface 已可恢复地移动到 `E:/SyncForder/CodeRepos/install-quarantine/20260824-*`，随后只同步当前公开头。
- `legacy/` 不参与 configure、compile、install、link、codegen 或 package。

## STOP 条件

在另立 L1-8 domain migration 工作前保持 STOP。Physics、Render ECS、Animation、Audio、Input、Script、Navigation、Streaming 等不在本轮迁移；L2 TaskSystem/AssetStore/AssetClient/AssetLease/ExtensionLoader 也未创建。
