# P02 实施记录

输入提交：`3bc49e44646df30ea73990864658a9121789757a`；分支 `codex/editor-redesign-v4`。
开始时工作树干净，P01-R1/check_receipt.py 通过。原 P02 与下载副本一致。

## 已确认的实施选择

- SceneSource 独占 CPU Registry 和 WorldEntityMap；不创建 SceneRuntime 或系统实例。
- 结构批次在预算内准备完整 CPU 候选，最后交换；字段和配置只准备受影响内容。
- 历史算法仍为 editor/history 唯一实现；SessionState 唯一保存准入和基线。
- 快照同步编码插件值为自有字节，不借可变插件节点；未知 payload 保留。
- WorldMaterializer 去除无用 composition 依赖；Camera 数据/算法/codec 分离为纯 CPU target。
- TFieldValue 和模型展开等纯算法迁出后旧调用使用同一实现；旧 Registry 适配最迟 P12 删除。

## 顺序

1. CPU 依赖与 SceneSource。
2. Session/批次/历史、冻结捕获及有界变化、私有重载候选。
3. 旧算法迁移与定向回归。
4. scene_model 正反依赖门禁、安装消费者。
5. P02 完整验收，分别提交实现与冻结记录，推送后停在 P02。

最终验收通过；C01、C03、C04 保持 FAIL 及原后续责任。

## 施工核对补充

- 新旧同名的 detail::SceneFieldEdit 已消除：旧运行 Registry 适配器为 RegistryFieldEdit，不留别名。
- EModelCreationError/ESceneStructureError 迁到轻量 SceneEditError.hpp，保留旧 domain_code；旧公开字段交互头不再引入完整 SceneAlgorithms/ScenePackage。
- Camera 的磁盘 schema 与版本不变，只移动 CPU 声明/算法/codec；RenderSystem 继续采用同一 schema。
- SceneDescription 增加 retainedBytes，配置捕获同时计入编码与解码后存储；Source 临时复制先检查预算。
- 全量回归发现 transitive sinclude 泄漏，已在 asset/core/world_storage/hierarchy 提供方收窄；真正的 codec 消费者显式 PRIVATE 使用。没有放宽门禁。
- 全量回归首次失败还包括未初始化 MSVC 环境，原日志保留；最终命令统一初始化工具链。
- 修复夹具局部变量重名；消费者一次出现 MSVC COMDAT 错误，拆除无关重型公开头依赖后重编通过，不删除用例。
- 工程规则引用的 ValidateTrackedSnapshot.cmake 在起点不存在，补入最小 clean tracked 检查，并为最终实现提交执行独立 clone 配置。
- 最终命令、原始输出和冻结门禁见 dev_log/P02；实现提交：fb55a5a9694831242e537a5cb1b3f7080a6f25fe。
