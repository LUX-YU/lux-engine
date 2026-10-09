# Mechanism / Authority MA05：Render 扩展槽与 Kernel 登记

实现 `dd4746504b9428abdc76744307447baf5e5b1ce6`，分支 `codex/editor-framework-v2`。
lux-cxx 保持 `0a0e7419fc7229df6e372cd35a540249f92250ef`。MA05 Windows 范围通过；全仓施工尚未完成。

FrameExtensionRegistry 是唯一槽位分配者，零无效、32 个有效槽、容量满时重复登记仍幂等。
KernelRegistry 删除第二份 extension map、计数器和 extSlotOf；注册时解析槽位，运行查询使用数值 ID。
十二个内置 Kernel 改为所属模块的显式常量声明；删除文件静态注册宏，通过实际 Feature 工厂登记。
Registry 拥有名称、不可变 descriptor 和代码 pin，冷登记串行、固定数组的已发布前缀通过 release/acquire 可见。
按 ID 查询不加锁，已有记录不替换；没有 unregister 或热卸载承诺。批量失败保留已接纳前缀及其代码责任。

Render 插件导出升为 v2；Component、Scene、游戏及脚本独立协议未升级。
实际外部 Feature、生成器、loader 和消费者同步迁移，没有旧宏或旧头转发。
旧例子的 Runtime 关闭调用改用现有 requestStop/退休协议：先取消并结清 Feature 登记，再关闭后端。
没有恢复已经删除的 Runtime 关闭状态机。

## 真实失败与行为证据

- 旧安装 SDK 接受空名字并分配第 33 个槽，失败输出 exit 42 保留；新实现拒绝且不消耗有效容量。
- 并发测试使用四个登记线程与并发 reader，验证同名幂等、完整不可变记录、准确条目数和 Kernel ID 耗尽。
- 实际 DLL 的调用方释放代码引用后，Registry 仍保活回调并可执行；没有声称观察到了进程退出后的 DLL 卸载。
- 六个实际 pass 经原 RenderGraphCompiler 编译：32 条命令、252 字节、六份 DRAW_DIRECT 载荷前后完全一致。
  此项使用固定 Git 对象中的私有编译器声明，属于源码级回归，不冒充安装公共接口资格。
- 外部真实插件 GPU 红→绿回读、ECS 绑定、追加/失败回滚、取消与代码寿命通过，验证层错误为零。
- 完整旧 SDK 插件先被 SDK ABI_MISMATCH 拒绝，原输出保留。另用当前 SDK 构建、仅改为旧 Render 导出符号的
  实际插件，验证 MISSING_EXPORT/lux_render_exports_v2；没有篡改 SDK 标识来绕过检查，也不将其称作旧二进制兼容。

首次夹具的关闭 API 编译失败、样例错误升级独立导出版本、ABI 检查预期位置错误，以及图输出诊断类型编译错误均保留。
独立导出版本由 `d2900510` 修正；随后并发登记补正形成上述最终实现，最终矩阵重新运行。

## 固定实现验证

独立 clean tracked 源码 `D:/LuxQualification/ma-source`，ValidateTrackedSnapshot 通过。
Editor/PLAYER 复用构建树执行全量 all -j 4 -- -k 0，第二轮均无工作；这是增量完整构建，不是冷构建。

| 配置 | 实际结果 |
| --- | --- |
| Editor | 128/128，包含既有 GPU/desktop 回归 |
| PLAYER | 65/65 |
| 全新 SDK 原消费者 | 15/15 |
| Kernel 安装消费者 | 2/2，另有公共头 C++20 独立编译 |
| 外部实际插件 SDK/GPU | 1/1 |

逐名称比较仅增加三个 Kernel 测试，原测试及 SDK 名称没有删除。
实际 compile/link/File API 闭包检查通过，五组分别有 668/612/57/4/3 个编译单元；底层 Render 不反向依赖具体 Feature 或 Editor。
删除独立构建树中的生成 C++ 后重新生成、全量构建及二次无工作通过，生成字节与固定 SHA 原输出相同。
公开头与新 SDK 和三个规定 include 前缀一致；Android 仅同步头，不记录为构建通过。

## 归档与保留范围

新 SDK：`D:/LuxQualification/ma05-concurrent-install`。
外部证据：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma05/evidence/`。
归档含 1723 个文件、100 次实际命令，包括此前候选和失败；最终矩阵仅按最终实现 SHA 计入。
manifest SHA256：`39eea6123e4e16e7b8f445125e2c5022b543b8eec09ac2e5a9bd2c3a8c292b7b`。

六处用户差异逐字节保留，ProjectBuilder 补丁未应用，main 及历史快照未修改。
LR08 PARTIAL、Linux 未通过、原生输入延期、IME 未测、Q-LR03-HOST-MINIMIZE OPEN 和历史 skinned WAR 保留。
本阶段没有重跑 sanitizer，不扩大旧资格。后续继续 MA06–MA11。
