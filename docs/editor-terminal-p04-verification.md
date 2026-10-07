# Editor 终态规范：P04 Root transaction / fallback

P04 gate：PASS。实现 `652885ec6552153dbf3ac63a6bb4e722e59e08f5`，前置 P03 验收 `7694ef869`。
本记录单独提交；继续 P05–P07，不等待阶段复审。

Root::replacePanes 完整验证、准备容量后进入短提交区间：撤销旧路由，交换唯一 owner，调用一次
on_commit，最后通知并返回旧 Pane owners。add/remove/clear 共用此内核；clear 保护覆盖用户析构。
容量按最终保留数量判断，失败不消费候选。stale/foreign removal 不追随复用注册。

Root 的应用命令 fallback 只存 ObjectId，通过原 Event 路由执行。目标已处理或禁用时不回落；
无焦点可回落；已取消的旧目标请求不能冒充无目标全局请求。ProjectUiMount 只拥有卸载责任，
不拥有 Pane，仅在 app 私有头中提供。此实现未改变 Context、Process、ObjectScheduler 或旧宿主政策。

## 实际资格

固定 tracked 实现通过 ValidateTrackedSnapshot。在独立干净检出中复用已冻结 P03 的构建目录做增量
验证，不宣称冷构建。SDK 使用全新 `install/Framework-terminal-p04` 前缀。

| 项目 | 结果 |
|---|---|
| Editor | all -j 4 -- -k 0，第二轮 no work；59/59 CTest |
| PLAYER | all、no work；40/40 CTest |
| SDK | 17/17，新增事务测试直接消费已安装公共头；公共头逐个 C++20 / 无 RTTI 编译 |
| 最小消费者 | Project、Scene、services/tasks、ObjectScheduler、TaskScope 各 1/1；Object 2/2 |
| 实际依赖负例 | Context→UI 与 UI→Editor 分别准确拒绝，去边恢复；恢复后 all / no work |
| 产品 | 安装 lux_editor 中文路径 create/reopen、WM_CLOSE 正常；无输入接管 |
| 删除/安装闭包 | 单结构算法，UI 无 Project 政策，私有 mount 未安装，无旧 SDK 回落 |

新增真实测试覆盖第 N 候选拒绝、容量/重复/回调 BUSY、提交与通知顺序、旧 owners 存活、旧 Pane 停止维护、
代际复用、无焦点 fallback、局部处理优先、禁用不回落、旧命令取消及 mount 移动/析构责任。
已有危险断言保留。构建与 GPU/桌面验证串行。

首次 SDK 构建读取了复用 CMake 缓存中的 P03 绝对包路径，缺少新接口而失败；日志保留。
使用 --fresh 重配 SDK 与最小消费者后通过，随后检查实际编译/链接路径不包含 P03 SDK。
这项是资格配置修正，未修改实现以绕过失败。

## 证据及范围

原始输出位于源码树外：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/terminal-p04/verified-evidence/`。
共 950 文件、57 条实际命令；清单 SHA256：
`cdcd7499e3767f8f3ed35f70eb0bbaeaedf0d2939e66ab13881b70d3656aac9b`。
中文/空格路径搬迁验证通过，缺失/篡改真实 SDK 日志被拒绝。旧快照不改写。

Root.hpp 已同步 Debug、RelWithDebInfo、Android 三个 include 前缀，字节哈希一致；Android 构建未运行。
lux-cxx 仍为 `0a0e7419fc7229df6e372cd35a540249f92250ef`。
用户三处排版差异保持独立；ProjectBuilder 补丁未应用；不改 main。
原生输入 NOT_RUN_USER_DEFERRED，Linux/IME/历史性能延期不变；sanitizer 仍在 P06 执行，不由本阶段替代。
