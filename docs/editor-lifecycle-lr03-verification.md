# Lifecycle LR03：聚合资源完整构造与真实所有权

固定实现 `0d8cf203df19665127a5083da6bdec6edc8247e5`，lux-cxx
`0a0e7419fc7229df6e372cd35a540249f92250ef`。冻结清单的 **228 条责任、74 个路径**已核对，
实现迁移完成；本次 Windows/安装/限定 ASan 矩阵通过。历史偶发问题
`Q-LR03-HOST-MINIMIZE` 仍 **OPEN**，因此不宣称 LR03 无遗留问题或全仓最终资格 PASS。
继续独立的 LR04，不把该问题挂成已修复。

## 实际迁移

完整构造覆盖 Pipeline/Shader/layout、资源与设备 Context、mesh/instance/sparse/paged buffers、
Canvas、Shadow/EVSM、Spatial/Stream/Terrain、Staging、Scene/View、HZB、Offscreen/target、
Instance/Device/Surface、GeneralRenderServer 和原 transfer worker。候选失败释放自己的前缀，
保留已接受资源、身份和原 GPU 退休责任；具体 26 组不变量和测试映射见归档 `inputs/frozen-coverage.json`。

- 删除已替代的 `init`、构造状态和普通 `shutdown`，以及无消费者 `TGPUResourceBase`。
  ResourceRegistry 不再按成员名字调用隐式 init/shutdown，保留逆登记顺序的真实析构。
- 原线程、上传账本、QFOT、队列、slot generation、回复和 GPU watermark 保留唯一实现。
  `stopAndDrain` 表示停止接纳、join 和结清已接受工作，不把语义对象退回未初始化状态。
- ReadbackCopy 拥有 staging/command/fence，超时回复不释放仍在途的 native backing。
  它还共享原 Offscreen allocation 的准确源图像；目标 resize/remove 不使复制源悬空。
- TransferCompletion 不可复制，转移 StagingBuffer 和 SampledImage 的真实责任；Bindless
  接受同一 owner。原 graphics-finalize 队列持有 command owner，拒绝提交时先撤销借用列表，
  再释放 staging，避免重试录制旧 native 地址。

27 份闭包收据按各自实现 SHA、原始字节哈希核验；这是历史完整性核验，不冒充本轮重跑。
修前真实失败、夹具编译错误、首次构建失败与后续修复全部保留，未用源码检索代替行为证明。
原生故障注入核验精确错误、存量资源、重试、身份和准确释放；危险释放由夹具截留到 GPU 安全点，
不通过真的执行 native UAF 来取证。

## 固定提交验证

先运行 ValidateTrackedSnapshot，再使用独立 clean checkout、全新 Editor/PLAYER 构建目录和
全新 SDK 前缀。全部构建与实际 GPU 执行串行。

| 验证 | 实际结果 |
|---|---|
| Editor 全量 all / 第二轮 | 1,216 步 PASS / no work |
| Editor CTest | 104/104；显式 GPU/desktop 模式 |
| PLAYER 全量 all / 第二轮 / CTest | 1,119 步 PASS / no work / 49/49 |
| 安装框架消费者 | 16/16；独立 all/no-work |
| 安装 Window / Physics2D / Script | 1/1、1/1、2/2；分别 all/no-work |
| 公共头 | 9 个实际修改安装头独立 C++20/noRTTI 编译 |
| 公开 native 消费者 | Instance/Device/Server 真实运行，Surface 编译；旧 init 编译拒绝 |
| scoped MSVC ASan | 全新构建 all/no-work，21 组实际 provider/owner 程序通过 |
| 源码/链接/安装闭包 | Editor 643、PLAYER 587、SDK 59 个 TU；PLAYER 无 Editor，SDK 无私有源码头回退 |
| 实际窗口 | placement 与 lifecycle `--desktop` 通过；不是系统输入/IME 资格 |

首次完整 CTest 为 **103/104**：Object retirement 的故意非法析构子进程在交互 CTest 下超时，
观察到 WerFault。保留原输出；实际子进程 GetErrorMode 探针分别测得交互模式 0、非交互模式 3。
使用 `--interactive-debug-mode 0` 后，同一三个死亡测试仍以 `0xc0000409` 拒绝非法生命周期，
随后原完整矩阵 104/104 通过。没有修改生产逻辑、原断言、超时或系统安全配置。

ASan 只覆盖归档列明的 21 组实际实现及调用者；依赖/完整后端 DLL 没有全部插桩，不能据此
宣称全渲染器 sanitizer 资格。九个修改的 modules 公共头在 Debug、RelWithDebInfo、Android
三个 include 前缀逐字节一致；Android 仅同步头。

## 归档与保留事项

外部归档：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr03/verified-evidence/`。
包含 **3,653 文件、803 条命令、56 条最终必需命令**；manifest SHA256：
`32ca06fcc40d12b0cb1acd92f68da99d0b19e81305c30d49be5dc01fb7a43cf5`。
相对路径及中文/空格搬迁验证通过；删除或篡改真实 Script SDK 输出均被拒绝。

`Q-LR03-HOST-MINIMIZE` 曾在 phase 8 返回失败；当前有限重跑、clean 和 SDK 通过不构成根因证明。
新增诊断保留真实 Error，原断言仍在。SwapchainProvider 的 init/rebuild 属于原 LR07 清单，
不能未经复现就用改动这些状态来宣称修复。跨帧 skinned transient pool WAR 也未宣称解决。

源码/SDK 原生输入继续 `NOT_RUN_USER_DEFERRED`，IME 未测。Linux、支持的 ASan/UBSan 完整资格
仍按 LR08 处理；未恢复旧性能长测。历史失败和用户免验不改判。

工作区 `E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。
六处用户差异哈希保持不变，ProjectBuilder 补丁未应用。本文单独提交，不修改此前验收记录。
LR04 按原清单处理 VFS mount、Input activation 和 vertex-source registration；MA 尚未开始。
