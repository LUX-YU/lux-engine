# EC2 R1 验收报告

本轮限定补正 **PASS**，实现 `ead7e59514a5b9a5d0dc65307e7c9e2a792b69f4`。EC2 整体仍保留原 PARTIAL，原生输入按用户决定继续延后。
不恢复 Linux/IME 或旧性能长测，不把未测项目改成通过。

## 修正

- `MaterialPreview.hpp` 公开拥有型 MaterialPreviewRecipe 与默认球体构造。setDesired 接收真实网格编码，
  不以外部递增 key 冒充配方。省略参数保留当前选择，首次选择默认球体。
- 同编译输入、更换 mesh 或同 mesh 的字节会推进配方与采用代次；同值幂等。prepared/pending/accepted
  各自保存固定配方。旧完成只结清，不能覆盖新期望。
- 资源失败/候选过期同时恢复最后成功读取源和网格描述。继续由原 RenderAssets、MaterialPreview lease、
  Runtime 和 ViewportPresentation 负责资源与退休，没有新 Runtime、缓存、发布器或业务框架。
- 原私有混合配方收窄为 MaterialPreviewScene 装配数据；旧头/函数和球体专用资源成员删除。target/包名不变。
  完整八文件变更见 [files.tsv](files.tsv)，所有权和调用契约见 [ownership-and-contract.md](ownership-and-contract.md)。

## XEC2-12 勘误与实际证据

[勘误](xec2-12-erratum.md) 明确撤回原“第二种网格已被 MaterialPreview 验证”的主张。原 EC2 tree
`4bee4f4a0b270110e47c97fd3bdcc244154b1ede` 与前置完全一致，未改写其原始日志或判定。

| 要求 | 修后证据 |
|---|---|
| 正式公开入口 | 修前真实安装 SDK 编译 C2039/C2065；同一源在新 SDK 编译成功 |
| 实际第二网格 | 复用原双视口四边形 MeshAsset 夹具，与公共球体经过相同 setDesired；GPU 回读两者不同 |
| 同输入换配方、乱序结果 | 真实编译产物给两个预览；同输入不同 recipe generation；旧完成不覆盖新 adopted |
| 已进入资源阶段的候选被取代 | queued candidate 被新配方取代，旧完成只结清，像素仍与当前成功结果一致 |
| 同 id 不同字节、失败恢复 | 损坏 MeshAsset 编码实际读取/解码失败，上一幅成功像素逐字节相同；重试成功 |
| 退休与作者不变 | 原实例未重建；作者 current/dirty 不变；关闭等待 retirement/resources empty，validation_errors=0 |

两份最终真实 GPU 输出在 `final-ctest-details.log`、`sdk/desktop-views-ctest-details.log`，
包含 `EC2-R1 XEC2-12 public sphere/quad recipe GPU readbacks differ`。测试不修改预览内部 Registry，
安装消费者不依赖源码私有头。本轮不增加网格选择 UI 或通用灯光/Feature 配方语言。

## 实际运行

- 独立 clean commit 全量 `all -j 4 -- -k 0`：PASS；第二轮 `no work to do`。
- 显式 EC2 + STRICT，定向 CTest **11/11**：编译三项、投影容量、真实新 GPU、两个原 GPU 模式、
  当前架构及 projection/compilation、desktop、EC2 实际依赖负例。
- 新 SDK 四组 **26/26**：projection-compilation 12、desktop-views 12、gpu-ui 1、editor-scene-pane 1。包含八项 operation 特殊成员编译拒绝；源和安装两个真实 GPU 验证均通过。
- 同一修前 recipe consumer 在新 SDK 编译成功；修改的公开头通过 clang-cl C++20 独立解析。
- 未修改的模型、History、保存、Run、Process、脚本/PLAYER 等引用原 EC2 准确记录，未声明为本轮新运行。
- 开发首轮因私有头更名未同步 provider 记录失败；随后同步准确路径，未放宽规则。日志保留。

归档脚本首轮误在没有本地 main 的独立检出中查询 main，保留失败日志；改为核对原工作区的实际 main 后完成保护核验。该脚本错误不是生产回归失败。

命令与退出码见 `commands.json` / `sdk/commands.json`。归档哈希与源码 SHA 由 verify.py 验证。

## 工作区与交付

- 应查看工作区：`E:\SyncForder\CodeRepos\lux-engine-ec2`，实施分支 `codex/editor-redesign-v4`。
- 新安装前缀：`E:\SyncForder\CodeRepos\install\EC2-R1-ead7e59514a5`；资格检出：`E:\SyncForder\CodeRepos\lux-engine-ec2-r1-qualified`。
- 原工作区 `E:\SyncForder\CodeRepos\lux-engine` 保持 `f7c27f9375cbf8dd8af37b30a6027a460de26213`，main 保持 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`。
- ProjectBuilder 用户补丁 **未应用、未提交**，字节 SHA256 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c` 不变；
  新路径仍是 `editor/authoring/project/src/ProjectBuilder.cpp`，映射补丁沿用原 P12 保护材料。
- 实现与本验收记录分开提交，正常推送后停在 EC2 等待复审；不自动进入后续阶段。
