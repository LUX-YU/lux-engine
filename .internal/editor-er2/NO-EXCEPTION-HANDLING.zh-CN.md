# Editor 源码异常处理清理

基线 d6088ef3e67e8326cc57805b031f6417adcd2508；用户明确要求不处理异常，而非捕获 bad_alloc 后终止。

## 最终变化

- Editor 正式代码与生成器删除 try/catch；Toolset 直接构造，渲染线程启动不再捕获标准库异常。
- 未加入全局 handler、bad_alloc 分类、异常恢复、特殊 terminate 分支或编译开关替代方案。
- 正常失败仍走已有结构化返回：属性校验、历史预算、业务 prepare 拒绝、容量/背压、资源错误与关闭协议。
- 保留标准 allocator 行为，保留既有 noexcept API。生成绘制入口直接调用 typed 函数。
- 不改写底层 modules / Scene / Process 和 legacy 历史对照代码；它们的存量异常没有在本轮全部清除。
- 扫描 112 份 Editor 正式 C++ 源/头，以及正常和诊断构建的 16 份生成 cpp，
  未发现 try/catch/throw；正式源码也无新 set_terminate/set_new_handler。

## 测试契约同步

撤销先前新增的异常终止 handler 和死亡测试，不为内存不足建立第二条处理路径。
原 C04、F02/F03/F07/F08 的 OOM 恢复要求撤销；Scene 工厂/提交/关闭快照、Renderer 工厂/seal、
生成容器与 Toolset 抛异常后恢复的测试随之移除。旧原始证据保留原身份。
factory_failure、resource_snapshot、resource_publication、resource_ready_publication 四个 GPU 旧模式
明确返回“已退役”，不会落入默认路径伪装通过。后两者的普通资源状态/通知检查以
resource_state_progress 和 resource_ready_progress 继续验证，不再注入内存异常或宣称 OOM 恢复。
View 容量拒绝、绘制期间关闭返回 BUSY、选择与 owner 保留等非异常契约继续测试。
底层 Render client / 通用 UI 仍保留自己的旧异常诊断；测试它们的现有结构化返回不代表 Editor 捕获异常。

## 本轮实际验证

- MSVC RelWithDebInfo 正常 all/no-op 与 CTest 139/139；专用诊断 all/no-op 与 CTest 159/159。
- 正常 5 个 GPU 变体；诊断 17 个变体，含编辑、启动相关创建、资源子失败/背压、共享 GPU、输入和关闭。
- 正常/诊断 10 个 DLL 的导出/导入/PDB 来源隔离通过。
- 从新开发安装前缀重新编译运行 editor-editing、editor-tooling、editor-scene-readers 三个消费者。
- 上述计数是注册测试数；撤销了 OOM 恢复逻辑用例，没有拿原数量声称旧恢复契约仍通过。
- 本轮是开发回归，未重跑独立 clean-clone SDK 迁移或成本矩阵，不重标旧候选的资格。

首次 CTest 驱动缺少 MSVC INCLUDE 环境，编译负例因找不到标准头而失败；修正环境后全套通过。
首次 GPU 退出由旧 FailingPane 测试主动 throw bad_alloc 触发，调试栈已定位；清理这项撤销的恢复测试后
正常 GPU 回归通过，保留了它原先同时检查的绘制期关闭 BUSY 契约。没有以该退出宣称正常业务通过。
安装消费者最初被驱动默认配置为 Clang，其中只对 MSVC 启用的断言没有执行，关闭调用也随 assert 消失；
因此 Session 生命周期检查终止了消费者。该轮无效，改用新的 MSVC 目录、显式编译器与 /UNDEBUG 后重跑。

原始日志、调试栈、源码扫描和产物身份随本轮 source-and-reports.zip 提供。
当前构建程序 E:/lux-shadow-ui/msvc/bin/lux_editor.exe；以前 q3 固定资格产物没有覆盖。
