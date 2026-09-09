# ER-1 续行候选：实际修复与资格

ER-1仍未通过。生产源码候选为 `aa91ba553f2ab56c5670a9061d7c41b1336f0433`，已从独立clean clone分别完成正常和专用诊断RelWithDebInfo构建、安装及实际GPU验证。本报告不宣称旧正式入口迁移完成，也不进入ER-2。报告提交与被编译的源码提交分别记录，后续报告更新不改变已有产物身份。

## 实际代码修复

- EX-01：AlignedAllocator保留标准容器契约，溢出/真实分配失败按标准异常传递，在准确factory/prepare边界转为结构化错误；aligned new/delete配对。callback/payload准备与显式release失败保留输入和owner。诊断实际覆盖Renderer工厂20、seal6、Window8、Scene12、client10个分配失败点；原seal index=2保留，不以abort/terminate替换成功判据。
- EX-02：resources[ri]访问先通过有效范围检查；不同target首次触及与跨View末态分别关联，保留共享导入和preserve语义。双View同尺寸/逆序/生命周期、FIF、resize和实际像素回归通过；没有清零所有cross_view_index或加入逐帧waitIdle。
- EX-03：现有Control/Program/Upload reply pump共用剩余额度并轮转。实际7个envelope、6个callback、3个failure、1个unmatched；连续60次budget1轮询各lane消费20个；一个含64条record的envelope计为1。预算计数没有使用全drain后截断。
- clean q1暴露的反射use-after-free已修复：新示例使用增量drainPending，不重复注册并替换已有metadata；100次实际构建保持反射指针稳定。原ASan/CDB证据见HEAP-INCIDENT.zh-CN.md及历史原始日志。
- 同量像素对比暴露的新SceneView相机原点错误已修复：按既有Render wire契约发送实际相机位置与rotation-only view。CPU shadow/cull与GPU投影一致，500帧旧/新实际图像相同；详见CAMERA-PROTOCOL-INCIDENT.zh-CN.md。
- aa91ba55补上SceneViewport/Workspace输入辅助与图像借用/释放的外线程入口检查；21个fallible owner调用、实际帧图像保留、Pane错误不被外线程覆盖及七种owner复制/移动禁用均有当前源码回归。其余owner-thread-only值/引用访问器没有被包装成完整跨线程安全接口。

## 当前资格

| 类别 | 实际结果与范围 |
|---|---|
| 正常clean构建 | all -j4 -k0、第二轮no-op、134/134 CTest；九个Scene GPU变体、foreign、Application部分启动生命周期 |
| 专用诊断clean构建 | all、no-op、135/135 CTest；19个Scene GPU变体、foreign、Application生命周期；真实分配点复验 |
| CPU/GPU寿命 | 旧packet、真实迟到resize回复、零尺寸恢复、pool/backing换代、两FIF；仅CPU引用释放不能伪造GPU完成；完成后descriptor计数配平 |
| 安装迁移 | 两个全新SDK位置各五种消费者，编译/链接规则排除source/build/旧依赖前缀，迁移位置排除原SDK；实际Toolset DLL路径/hash符合所选prefix |
| 生成器 | 复制安装SDK后改头/模板/宏实际再生成；精确非法widget负例保留完整旧输出的字节与mtime；恢复后no-op与消费者通过 |
| 路由/回归 | 真实Object/UI CommandRouter、固定Text/Record目标、InputText拦截、菜单token、direct/queued信号、既有Graph/Inspector/Context相关CTest |

原始证据：`E:/lux-er1/q6/raw/`、`diagnostic-raw/`、`regeneration/`、`qualified-isolation/`。两类DLL/EXE身份分别在qualification.json和diagnostic-qualification.json；DLL/PDB链接对象与导出审计证明正常SDK没有专用replacement allocation/fault对象。诊断代码不由普通BUILD_TESTING开启。

source-files.sha256.csv描述aa91ba55 clean clone的实际tracked字节，不描述后来报告提交。source-binding.csv的23项已逐路径检查；S02在clean checkout补齐两处CRLF，规范化文本和Git内容不变，当前hash按资格源码字节记录。

## 成本与限制

五组十个独立正常进程采用同seed、尺寸、单View、100预热、500实际计量帧、8单列验证帧、8ms节奏与相同逐帧录制等待条件，像素checksum全部相同。完整原始样本和计量边界见COST-q6.zh-CN.md。q5不一致等待策略样本没有删除；仅针对这一项窄假设统一驱动边界。没有为了改善数字更改生产Renderer调度。

关闭批量均值、等待cycles等残余差异原样保留，不声称整体性能等价。未测完整Application事件循环、owner分配/保留内存、匹配resize/retry尾部成本。快照复用并不代表outlineCurrent的逐row检查为O(1)。

## 尚未完成

118行验收表当前为56 PASS_ER1、5历史PASS_INTERMEDIATE、32 PARTIAL、1 BLOCKED_DELETE_GATE、24 DEFERRED_STAGE。历史GUI等中间证据没有升级成当前提交通过；这些逻辑行数也不等于134/135个CTest注册项。

本阶段尚未完全通过的验收ID：A01, A03, A04, A05, A06, A07, A09, C05, C06, U04, U05, U10, U11, U12, H03, H07, F15, R01, R06, R07, R09, X01, X03, X04, X08, X09, X10, I02, I06, I07, I08, P02, P03。逐项范围与已有证据在acceptance-results.csv，不把不同逻辑条目的数量当CTest注册数。

主要缺口仍是线程/设备/attach分阶段启动故障、View接纳容量/分配失败、GPU mesh成功后的shader/material子失败、set_stopped/value独立路径、资源Control/Upload饱和重试、部分关闭/值访问器线程审计和成本账目。受工具输入能力限制，真实持续RMB/MMB后失焦及中文IME候选仍未验证；此前实际过滤、分栏、选择、滚轮、最小化/恢复只保留为历史GUI证据，未重标为本提交GUI通过。

M26 Toolset已冷装配并闭合Context/Inspector和安装消费者。M01—M05、M21—M23、M28—M29旧正式入口的删除门槛仍未满足，保留审计明确的旧栈，SDK不宣称legacy-free；新路径没有调用旧栈或runtime fallback。Text/Record仅证明共享Editing与路由，Material/FlowForge迁移留ER-3。

被动RenderLease析构中的既有deferred vector分配仍没有全路径OOM保证。终局record failure已实际验证，不能把它改名为VkDeviceLost专用测试。其他范围不以替身、隐藏源码补丁或删测试冒充通过。

主检出仍为c77bb41e，六个受保护Script文件hash复核不变。没有合并main、推送、发布、冻结、升级依赖或开展Android构建。
