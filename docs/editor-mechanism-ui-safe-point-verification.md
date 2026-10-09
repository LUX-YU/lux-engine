# MA07：UI 安全点队列闭包

本记录仅验收 MA07 的安全点队列统一，不表示整个 MA07 或机制整改完成。

实现提交：`88e2320512a2bc44d9e141c54a2999d5ecdf3de0`。
依赖 lux-cxx：`0a0e7419fc7229df6e372cd35a540249f92250ef`。

## 问题与修改

真实旧安装 SDK（实现 `1f1e59b8d83fc1455116133d5ba44796e526fa01`）先接纳命令、再接纳结构修改，
在维护安全点却执行为 `2 1`，违反入队顺序。回归通过公共 Root、Pane、菜单和输入入口复现，
实际断言失败输出保存在外部 `before` 证据中，未使用替代实现。

Root 的唯一 pending 队列现在保存 `DeferredMutation` 或 `CommandExecution`。
删除 `menu_state.calls`、`change_state`、两段排空逻辑及重复目标失效处理；
保留原 Root/Object/Pane 代际校验、结构意图去重、同步 QUERY 和延迟 EXECUTE。
同一固定批次按 FIFO 执行，回调追加的工作留给下一批。目标借用覆盖完整同步调用链，
取消中的当前批次条目置空，后批条目移除。目标失效不会变成无目标的全局请求。

只修改 UI 私有实现及原 transaction 测试；未改变公共头、target、安装包、ObjectScheduler 或执行器。

## 实际资格

- 独立 clean tracked 源码，`ValidateTrackedSnapshot` 通过；构建树为复用的增量树，不记作冷构建。
- 全量 `all -j 4 -- -k 0` 通过，第二轮 `ninja: no work to do`。
- 完整 CTest **170/170** 通过；相对上一完整矩阵无删除测试名称。原 transaction 断言保留。
- 新断言覆盖命令/结构修改 FIFO、结构意图去重、重入 update 拒绝、回调追加留到下一批，
  以及前序操作移除目标后同时取消其命令和修改、禁止转投全局接收者。
- 新 SDK 前缀 `D:/LuxQualification/ma07-safe-point-install`，公共头消费者重新构建及执行通过，
  实际输出为 `1 2`。编译与链接输入检查未使用源码私有头、旧构建 DLL 或 legacy。
- 本轮 CTest 包含现有 UI Scene、项目切换、关闭、桌面和 GPU 回归；未执行原生输入接管。
- 六处用户差异逐字节哈希保持，未纳入实现；ProjectBuilder 历史补丁仍独立、未应用。

PLAYER 未在本闭包重跑，其未修改路径仍引用原准确 SHA 的资格；未更改 modules 公共头，
没有新的三前缀头同步或 Android 构建成绩。

## 证据与保留范围

外部证据根：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma07/safe-point/evidence`。
包含原始失败、实际命令及日志哈希、源码/安装消费者、构建输入、固定实现 SHA 和验证脚本。
归档已在中文/空格路径验证；真实日志缺失和篡改均被拒绝。
清单包含 39 个文件、17 条命令；manifest SHA256 为
`0a3a76f7916e3a2688985392a57bc8668689c0a291abad09f8353c0c88f9e1b3`。

先前 `1f1e59b8` 的 `render.transfer_idle` 缺少完成标记失败继续保留，原因未查明；
本次通过不改写旧失败或宣称其根因已修复。

MA07 的 ActionDescriptor、引用式 MenuEntry、Editor 菜单贡献及确定性排序仍未完成。
MA06/MA08 剩余边界与图呈现资格、后续 MA 工作仍待完成。LR08/Linux 保持 PARTIAL/未测未满足，
原生输入继续按用户决定延期；IME、sanitizer、历史性能及其他历史未通过范围不扩大。
