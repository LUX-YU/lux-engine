# P10 施工记录（进行中）

前置 `3363f83dcc448db9e79cea97c5176a546c05cf5a`；分支 codex/editor-redesign-v4；main 不变。
启动包 40 项 SHA256 校验通过；reference/v4 与原施工规范共有文件逐字一致。
ProjectBuilder.cpp 保留用户差异，SHA256 ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c。

本文件为三份现有账本的施工说明，不是独立状态账本。最终验收尚未执行，原 180/93 项仅是前置结果。开发构建/局部测试记录见现有 test-coverage.json 的 p10.development_evidence，不替代最终实现 SHA 验收。

## 顺序和责任

A：原文件/入口盘点；B：ViewHost/DesktopShell；C：Scene 主/辅助视图；D：重绑定/退休和双 GPU；E：Material/Flow/任务/项目；F：正式模块集成；G：全部回归和归档。

- Host 独占 DetachedView；Root 仅登记。固定槽、完整域和代次，回调新请求进入后批；挂载通知内查询准确 BUSY。
- SessionStore/RunStore/SaveService/编译 owner 保持原责任；View 不拥有这些业务对象。
- ProjectionHub 共享作者投影；ViewportPresentation 保留本视口请求，析构将停止事实交原资源 owner，不能阻塞或保活整图。
- SceneInteractionGroup 由装配根拥有（可共享选择），View 借用；关闭一个 View 不销毁组或 Session。
- WindowInput/Output、Presentation/UiRenderSyncStage 迁 desktop，删除原体，旧产品使用同一个正式实现。
- Scene 字段/配置/空间事件/codegen 归工具 ui；空间查询/相机数学归 scene/ui（interaction 保持作者交互的依赖边界）；列表/节点 UI 算法归 widgets；资产查询归 project/ui；任务查询归 tasks/ui。
- 原工具 UI、Context 转换仅原产品及既有回归消费，期限 P12。所有新链禁止反向依赖。

## 必须保留的功能矩阵

Scene：结构、字段、配置、创建、拾取、Undo/Redo、作者/运行隔离、资源失败重试。
Material：图、常量、槽、编译、发布、旧效果/stale、预览导航。
Flow：节点连接、变量、函数签名、导出、字面值、固定产物链接重试。
辅助：真实任务查询/取消、项目/资产选择、Inspector 生成字段、原 launcher 入口。

ViewHost、新 SceneView、生成 Inspector、SceneConfiguration/Creation、Outliner/Resource、Material/Flow、Task/Project 的开发期回归已接通。P10-services-input-boundaries-dev-03 中真实 GPU、原生输入、当前 P10 边界与 25 个真实依赖夹具通过。它们仍是开发记录，最终干净实现提交验收尚未运行。

## 已完成路径的具体证据与边界

- SceneConfiguration：正式描述载入，RenderSystem 页大小 1024→256，Apply 经一次 SceneEditBatch；Undo 与 Revert 恢复 1024，显式 provider 绑定。
- Outliner：创建空间对象、重新挂父、删除、Undo，树折叠；默认组件编码唯一算法移到 SceneAlgorithms，原 SceneEditor 只保留旧记录转换。
- ResourceView：同一实际资产端点先报告 NOT_FOUND，正式资源表显示 FAILED；用户请求重试后恢复 READY，进入双视口材质 mesh 绘制。BUSY 不解释为不存在。
- Material/Flow：控件使用的 View 编辑路径覆盖常量/槽/连接/节点布局，变量/函数签名/调用/字面值/导出；Preview 不改作者，Commit/Undo/Redo 保持原模型契约。真实编译、固定产物链接重试及发布使用原服务和同一 WriteCoordinator。
- 关闭 Material/Flow/Task 视图后，应用 owner 继续结清已接受保存、产物发布或取消结果；源/History/checkpoint/编译/Run 责任不转入视图。
- 实机：正式 SceneView 双 mesh 像素回读验证左右高亮隔离和单视口退休；新 DesktopShell 接受真实 OS 鼠标拖动、窗口外捕获、Unicode 字符输入和 DIRECT 回调关闭。系统 IME 候选/提交未实测，不能据此声称通过。
- 独立依赖夹具覆盖 direct/static/imported 旧依赖、私有头、rooted 工厂、反向依赖；旧后端测试的内部头只对该测试 PRIVATE 提供。
- 新模块均为正式 STATIC 库。harness 不安装，所有功能来自正式模块；不切换旧产品入口，不实现 P11/P12。

## 失败保留

开发日志原样保留在同一施工目录，包括输入焦点、扩展未装配、provider 配置、COMDAT 对象错误、错误类型及依赖闭包失败。修复后重新运行不抹去首次失败。
ProjectBuilder.cpp 哈希未变。C01/C03/C04 维持原 FAIL 与责任。旧快照不修改。

## 安装消费者补正与最终资格开始

安装消费者发现并修正：scene_ui 安装登记早于 codegen 脚本登记；关联容器分页仍使用旧 finishPending；完整组件移动契约与普通 noexcept swap 的差异。现通过已有 schema 安装契约进行整组件移动构造交换，不只交换反射字段、不放宽失败恢复。新 SDK 以原完整 consumer::Component 覆盖嵌套、序列、映射、集合、optional/variant 和 Eigen；实际标量与集合操作验证作者编码及 Undo/Redo。4/4 通过，真实双视口与输入 validation_errors=0。

实现 5d0a7c37abca246e9bd63ad451d524ccfa0c083a 已提交。原工作区因受保护 ProjectBuilder 差异不符合 clean 资格，拒绝日志保留；正在独立 clean clone 执行最终 P10，尚未宣称最终通过。

最终资格首轮 5d0a7c37：clean all 构建、二次无工作及 185/185 CTest 通过；旧 SDK 消费者只引入 desktop_shell 时缺 view_host 包内依赖，配置拒绝。补正使用现有 component_add_internal_dependencies；新 SDK 消费者改为只请求 desktop_shell，验证传递导入。补正实现 db9f2bf8b90cec85ffba3c4031b5589a07b549dc，完整资格重新执行。首轮目录全保留，不当作最终 SHA 成绩。

资格第二轮 db9f2bf8：构建/无新增工作通过，184/185；原生输入在 NumericEdit 拖动预览断言失败。固定三次 UI capture 不足以证明系统消息已消费。测试补充平台鼠标位置、Root native sequence、按钮状态和控件 active 的有界准入，保留作者/预览/提交断言，启动清除上个失败进程遗留按钮状态。五次连续真实原生输入通过，未修改生产 UI 算法。实现 11de2c1fee9d477daaa2d26b07911306ea9d6b9f 开始完整重验；首次失败完整保留。

## 最终实现验证完成

实现 11de2c1fee9d477daaa2d26b07911306ea9d6b9f：独立干净源码显式 P10 全量构建及二次无工作通过，185/185 CTest、PLAYER 11/11、原13组及新增正式桌面 SDK 共14组97项通过。25项实际P10依赖夹具、八项operation编译负例、历次模型/持久化/交互/Workspace回归通过。新双SceneView实际mesh/高亮像素回读、原生桌面输入与SDK对应测试validation_errors=0；两个旧显式GPU模式另行通过。实际环境为NVIDIA GeForce RTX 4070 Ti，系统IME候选/组合/提交未实测，保持NOT_RUN。C01/C03/C04按原FAIL保留，ProjectBuilder哈希不变。将上述日志与失败历史冻结到dev_log/P10，并执行只读归档门禁后提交验收。
