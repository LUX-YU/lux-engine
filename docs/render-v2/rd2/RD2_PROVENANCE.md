# RD2-D0-C2 来源与保护清单

基线：`b9f9a909bc34ac87f00c594996201a20f0af00c8`。Frozen Legacy：`a669409a289a6fa4092f21176397795b1cdb7f3e`。

源 ZIP 未覆盖、未重打包；删除外包装目录后导入全部 34 文件，候选正文按用户裁决改订。原包 MANIFEST 校验 33 文件；新候选 MANIFEST 覆盖本 Implementation 的 rd2 文件（不含 manifest 自身与随后 V 才新增的报告）。新旧包哈希不能混用。

原始资料保存于 `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD2-D0/preflight-b9f9a909bc34/inputs/`，最终证据复制至 `<I>/preflight/`。旧 FINAL ZIP 的下载名有 `(4)` 后缀，实际找到的 F0 归档名无后缀；SHA 完全相同，按内容身份匹配。

| 来源文件 | SHA-256 |
|---|---|
| `RenderV2_RD2_Architecture_and_Implementation.zip` | `d038fe6e8ae00302c602834b2032dd4d5f128cc9238585e80306f73fc162d0a4` |
| `RenderV2_RD2_D0_Work_Order.md` | `512929c1e0ac09355638a0593216bd6f63e53ac38c067ffd55318a3068867d42` |
| `RenderV2_RD2_F4_Closeout_Work_Order.md` | `f4cfc96ebe2e7423c450584c76cde0138d2b61aa178e7c1e3aa3c9d5a94582af` |
| `RenderV2_RD2_F5_Entry_Outline.md` | `511b78ac5cee5c295560b69732d0e9848a50b50c7d02338f9898b4ea942626c4` |
| `RD2_D0_86E9A8ADE489A3E5B3B8E5A5BAE687BB%A4.md` | `8230ed2f66cfb7b94d664fcf4d84d859feec0a0358976694e794654f561150f2` |
| `LuxEngine_RenderV2_FINAL_Architecture_2026-10-10.zip` | `39be1dd08ca4e30964ef47fa0b274054bc426e79472a1d0e733deaa0fe83d551` |

## 四张原 CSV

| 文件 | 行数 | SHA-256 |
|---|---:|---|
| LegacyCapabilityMatrix.csv | 86 | `ce3555c7eaf50a981a53ec22a9ae41f5bfbe35e626970a891623c8c4448451b6` |
| GoldenWorkloads.csv | 25 | `bc9fd980b4d6455bc981a2f458ba53e427da1fceccb7e44b63ffc5a9c0f31b42` |
| RequirementsTrace.csv | 51 | `9f04107bcf5d06a77c42637614f8a315abf98c211aad101bca3340bdcbb5b9a0` |
| TypeOwnership.csv | 131 | `9a57c133d5bb7e1e4da6c57a34483be6364bfa6226195fcfe129892ed47952d4` |

## 用户工作区

`E:/SyncForder/CodeRepos/lux-engine` 的原六处修改及 `render-v2` 工作区 `Operation.hpp` 的第七处修改保持在用户检出中。本次独立 worktree 从批准 Git commit 创建，不复制它们，不 reset/stash，不改用户分支 HEAD。staged/unstaged binary diff、status、untracked 清单和七文件原字节/哈希位于 preflight/original 与 preflight/v2。

保护既有所有生产源、CMake、FINAL 和历史报告，资格时用基线→I 全 tree diff 检查；Legacy 另对 719 份 manifest 的 source/destination mode/blob/size。独立 clone 不使用 hidden sources、hardlinks 或 alternates。

## 源码身份与目标 API

[源码锚点](RD2_SOURCE_ANCHORS.json) 保存基线精确 blob。Identity/Descriptors 证明版本与稳定 ID 分离；Layout.hpp 证明当前三槽 OwnerAssignment；Executable.hpp 是现有有限 Native Graph；Plan.hpp/Builder.hpp 是已验收中立 Graph 接口。

DataSchema、GpuLayout/Representation、DataSet/TypedStorage、BindingSlotKey 多 owner、Reader/Writer、完整 Feature SDK 均为目标，不能根据名字出现而判实现。现有 ShaderOwnerId 不等于 RD2 多 owner 合同已交付。F4 三项 native 风险只是静态关注，复现属于后续 F4-C。

## 外部参照

2026-10-11 读取原第 01 章列出的 EnTT、Epic RDG、Unity BRG、Bevy 0.19 与迁移指南官方页面，链接可读取。该可达性不代表本文重新验证外部引擎的全部实现，也不是 LuxEngine 性能/资格证据。原引用保留，不以外部数字替代本机测量。
