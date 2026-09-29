# P05 R2 完成交付补正验收

实施分支 `codex/editor-redesign-v4`；前置 `5a1ccbb47d2d08ffa8cfda907f74319c4a694068`。
实现 `3a75bfe6f743d8b9bb74f93f89219054aee2119d`。显式阶段为 **P05**，停在 P05 等待复审；未进入 P06。
本目录是阶段末冻结证据；唯一可变施工材料仍在 `.internal/editor-redesign/`。

## 修正与所有权

保留 R1 的角色撤销检查、递归采用/确认保护、WriteCoordinator 有界版本衔接。
`completeEncoding` 在 owner 上接收已准入 ENCODING 的一次性完成，包括首次 collection
发生在 callback 中的情形。它只结算该操作的额度和原 ticket 的字节/错误，不运行角色、不采用基线、
不删除记录或销毁候选。内层 DispatchScope 恢复旧值，外层保护仍有效。
`SaveExecution` 明确检查交付契约，不再丢弃返回值；正常完成无需队列或重试。
错误 owner、已删除 ID 或重复完成继续是结构化调用错误；适配层的恰好一次 owner 交付若违反这些
前提则立即暴露契约破坏，不把终止当正常 BUSY 的替代。

完成前：任务拥有编码及结果；完成接收后：SaveService/原 WriteCoordinator 记录承接事实。
SessionStore、SessionState、History 的唯一所有权不变。三模型、执行器、协调器算法未修改。
无新增管理器、线程池、包、target、公开输入框架或兼容桥。

## 修复前真实失败

`before/` 是未修改生产实现时，链接已安装 R1 SDK 的真实 MaterialSaveSource 包装器、
SaveExecution、ExecutionRuntime/stdexec 运行结果，不是复审包的隔离 shim。
W2 worker 的 `TaskInfo.finished` 已成立，W1 accept 才首次调用 collect。
成功、实际 MaterialCodec 身份不匹配错误、stop 取消三个场景均退出 **20**：
收集了一个完成、encoder 执行一次，W2 却保持 ENCODING/RESERVED；取消不结束；W3 READY 被阻塞；
实际文件仍为 material1。主动退出保留交付缺陷，避免随后析构终止覆盖主要证据。
保留日志、真实文件、测试源码/补丁、链接命令及 SDK 哈希。

## 验收映射

| 要求 | 证据与断言 |
|---|---|
| R05-R2-01 | `persistence-r2-accept-success`：真实 CPU 编码，accept 首次 collect，W2 冻结字节、Undo 保存基线、W3 FIFO；一次编码/析构 |
| R05-R2-02 | `r2-accept-error/cancel`：实际 MaterialCodec 类型化错误、Runtime stop + 保存取消、NotPublished 原错误、同目标 W3；`r2-admission` 同时覆盖真实 task_capacity 拒绝 |
| R05-R2-03 | `r2-admission`：describe/capture × 正常/普通异常/撤销，每组合 12 次；已完成 W1、外层拒绝/额度回滚正确 |
| R05-R2-04 | 完成后在外层 callback 内递归 adopt/ack/request/cancel 仍被拒绝；R05-01～08 全部原场景继续运行 |
| R05-R2-05 | 72 次有界循环、两份容量恢复/第三份拒绝、无残留票据和 job；stop/join；原九组 SDK 消费者重建后执行新增关键成功/失败 |
| 继承验收 | 原 111 测试名及全部断言保留，X05-01～09、相关 Q、P01/P02/P03/P04/R1、真实 IO 与依赖负例重新运行 |

正式完整 CTest **116/116**；九组安装消费者共 **33/33**。
计数只作索引；逐项行为、运行命令/返回值/哈希、SDK 链接输入及实际依赖负例见
`receipt.json`、`logs/`、`evidence/`。P05 门禁验证命令：

```text
python dev_log/P05-R2/check_receipt.py
```

门禁只用可取得的归档相对路径与实现 SHA，不读取生产机器上的旧工作路径。
`validation/` 验证搬迁后的完整归档通过，缺失必要日志、篡改日志、缺失前置收据拒绝。
历史门禁按各阶段自己的 implementation_sha 检查，原快照不改写。

## 范围、未通过项与工程事实

- 全量 `all -j 4 -- -k 0`、二次无工作、CTest、安装、消费者按顺序在独立干净实现检出验证。
  复用既有独立构建目录；不声称本轮是从零首次冷构建成功。
- **C01/P09+P12、C03/P11、C04/P12 仍为 FAIL**，原断言、实际失败日志及责任保持。
- 原 Physics2DDescription 在类型静态头生成前编译的冷构建顺序问题仍未修复；证据保留在
  `dev_log/P05-R1/logs/development/f2b5a00c60e93dc81d7c72b127ad08b219ccfa23/build.log`，
  责任是 `engine/domain/simulation/builtin_systems/physics2d/CMakeLists.txt` 的生成依赖。
  不归入 C01/C03/C04，也不借后续构建成功改判。
- 主工作区 `editor/project/src/ProjectBuilder.cpp` 既有用户修改哈希未变，未重置、未提交，
  不计入资格实现。`evidence/excluded-user-changes.patch` 记录其范围。
- 研发测试同步错误及新增测试编译错误均保留在 `logs/development/`；修正后重新验证。
- 原限定消费者的旧适配继续最迟 P12 删除；本轮不扩展、不新增桥。无 Android 验证。

## 修改清单

`files.json` 绑定实现 SHA。共 7 个修改文件，无新增生产文件、无删除文件：
SaveService.cpp、SaveService.hpp、SaveExecution.cpp、persistence/README.md、
真实模型测试 models.cpp、测试 CMake、安装消费者 CMake。
移除的是 completeEncoding 的 blanket dispatch BUSY 拒绝，以及适配器无条件忽略交付返回的路径。
实现与本验收记录分别提交，正常推送实施分支。

归档门禁首次把 Ninja 的 @response 文件命令误当已展开的库名列表，明确拒绝验收。
原修复前命令、SDK 哈希和日志不修改；补存最终安装消费者 build.ninja 的实际链接规则，
其链接声明本轮未改。该扩展规则在 SDK 重装后取得，不能冒称修复前的 response 文件副本。
门禁同时核对原命令、原 SDK 哈希清单、安装包链接声明和实际展开规则；失败日志保留。
