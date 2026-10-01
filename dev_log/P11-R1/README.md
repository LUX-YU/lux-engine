# P11 R1 补正验收

本轮仅补正 P11 输入清理与复合贡献保护。P12 未开始。

## 改动

- `SaveService::prepareSource`：输入先组成 code/source 拥有单元；进入现有 dispatch 后立即转移到内部局部单元。所有准入后的提前返回都在保护内销毁源，最后释放代码。不增加另一个 gate，不修改完成接收。
- `CommandRegistry::Batch`：在原命令 owner 内授予栈范围保护；只读批次不消耗版本，发布准备检查容量并携带一次性 commit 权限。普通 publish 与递归 drain 被拒，固定句柄查询/执行、只读快照和后批 enqueue 继续合法。
- `ContributionRegistry`：同时持有自身及参与命令 owner 的 scope，覆盖反射验证、无分配交换、候选/旧值清理与 changed 通知。删除一次 canPublish 检查后跨回调再普通 publish 的路径。没有反向 application 依赖或新管理器。

## 证据

原 SDK 真实负例保存在 `before/`：重复、无效和完成接收清理看到新业务可用；工厂、反射失败、旧值析构、通知均能直接改变命令目录。原 BUSY 和成功转移对照通过。不是附件的小型 witness。

新增断言检查完整作者编码、current/observed/binding/dirty、History 全部状态、保存基线 Undo/Redo、源先于最后 code pin 析构，以及真实 SaveExecution 完成仅接收一次。贡献测试检查实际注册内容和未提交反射条目，不只比较返回码。原 213 行为及原断言保留；新增9项场景归入原程序。

详细命令、退出码、固定实现 SHA 和归档哈希以 receipt.json/artifacts.json 为准。所有最终验收来自同一 clean tracked commit，显式 P11 + STRICT；SDK 使用全新安装前缀。

## 修改与删除清单

实现提交：`1afb8f3f6e584075c6a9204e4faa7c406e55e146`。全部 14 个修改文件及 Git 内容哈希见 `files.json`；无新增或删除生产文件。

| 原有文件/模块 | 本次变化 |
| --- | --- |
| SaveService.hpp/.cpp | 补充清理契约，将拒绝输入从外层拥有单元转到 dispatch 内；删除原先退出保护后才析构输入的顺序 |
| CommandRegistry.hpp/.cpp | 增加原 owner 的窄 Batch；统一准入检查，普通 publish/drain 在批次内返回 BUSY |
| Contributions.cpp | 删除跨外部回调的一次 canPublish 检查及随后普通 publish；改为参与 owner 授予的一次提交权限 |
| commands/README.md、extensions/README.md | 描述同线程借用寿命、一次提交、清理与通知边界 |
| commands.cpp、contributions.cpp、models.cpp 测试 | 保留原断言，增加真实拒绝清理、反射失败、通知、最后代码 pin、可靠编码完成组合 |
| extensions/CMakeLists.txt、persistence/CMakeLists.txt 及两处 installed-consumers CMake | 登记新场景，使同一原程序也从真实安装接口构建和运行 |

未删除任何原有业务校验。新普通发布准入被拒后，不消耗命令 revision；失败候选不改变贡献、命令和反射事实。固定 handle 查询/执行与 enqueue 的原职责保留，enqueue 只留待外层安全点处理。

唯一 owner 没有变化：SaveService 仍负责注册和已准入完成；CommandRegistry 拥有命令目录；ContributionRegistry 拥有贡献目录并组合两个作用域。Batch 只借用原 CommandRegistry，不复制服务、不保留历史目录或建立代码包装链。

## 边界

无目录、target、库、包或磁盘格式变化；V7 导出表不变，Editor ABI 指纹重新计算并安装，运行 ABI 保持独立。三作者模型、History、SessionStore、Runtime、Renderer、执行器和 WriteCoordinator 算法未改变。

C03 当前 PASS 保持；历史 FAIL 不改写。C01/C04 仍为 P12 责任。Linux、系统 IME、sanitizer 未测；旧慢算法性能 PARTIAL 不补样本。已登记旧产品消费者仍按原 P12 期限交接。

用户应查看 `E:/SyncForder/CodeRepos/lux-engine-p11`。原 `lux-engine` 工作区仍停留在受保护的旧提交；ProjectBuilder 用户补丁未应用到新检出，也未提交。原文件哈希与所有历史快照保持不变。

完成接收场景的日志中 `prepare_BUSY=1` 和 `request_BUSY=1` 均在 collect 返回后测得，直接证明外层 dispatch 仍生效。`outer_preserved` 字段只在独立的准入前 BUSY 用例中采样，其他用例中的默认值不表示保护解除。

## 最终执行结果

- 固定实现 SHA 的独立 clean clone：全量构建通过，第二轮无新增工作；P11 + STRICT。
- Windows 全量 CTest 222/222，CPU native 196/196，PLAYER 12/12；保留原 213 个命名行为及断言。
- 全新 SDK 前缀 24 组消费者全部通过；真实 V7 DLL 两种最后 owner 模式、R11 的真实保存/贡献消费者及两个旧显式 GPU 模式均通过。
- 新双视口 GPU 隔离/退休和原生鼠标/键盘/焦点/Inspector 输入通过，validation_errors=0。系统 IME 未测。
- clang-cl 公共头与质量消费者通过；修改的两个安装公共头独立按 C++20 解析。
- 指定 Inspector 生成输出删除后重新生成字节一致，第二轮无新增工作。
- 实际安装 Editor ABI 指纹已改变，运行 SDK 身份不变；隔离 Editor 版本变更也证明运行 ABI 不受影响。

缺失、篡改、错误源码 SHA 和中文/空格路径迁移资格见 `archive-probes/results.json`。首次失败、测试夹具修正和修复前七个真实 SDK 失败保持在归档中。
