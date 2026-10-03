# Project activities

Existing domain activities, relocated with their separate real targets and ownership.
Author state remains in authoring; workbench and application are consumers, never dependencies.


## 项目存储

ProjectStorage 是已打开项目的清单、源/产物目录、写锁与发布状态 owner。
它借用 EngineContext 的 AssetVfs，拥有自己的挂载登记；不拥有 Renderer、线程或 Pane。
后台 prepareProjectOpen 产生 PreparedProjectOpen，在 Main 安全点安装到 ProjectStorage。

准备成功的 ProjectPublicationPlan 私有保存 manifest 与其一次编码字节，向 worker 提供共享只读值。
PreparedProjectPublication 独占原 ProjectStorage 的发布占用，移动只转移一次，旧载荷清理完成后才释放占用。
收据必须对应原计划身份和版本前提；Unknown、已写入但未采用等事实继续由原 Operation 持有。
PreparedProjectOpen 是一次消费的打开结果，包含写锁与准备挂载；它不是可修改的项目描述。

createProject 接收 ProjectBuildConfig 和原生目标路径，复用 Process CPU/Blocking/Main：
编码与摘要 → 原子取得新目录创建权 → journal 发布源文件 → 最后提交根清单 → Main 采用结果。
完成槽保存持久提交事实，提交后的取消不能改写为“项目未创建”。成功结果释放写锁后再交付。

现阶段要求目标目录不存在。失败恢复只处理 journal 中摘要匹配的文件，不递归删除目录；
根、空目录与持久锁可能保留。项目打开仍扫描源摘要和产物，按需读取属于后续工作。

`editor_storage` 为 STATIC；依赖 project 描述与 Process，不依赖 assets 的导入实现。

## File publication

ProjectStorage uses the shared [file publication kernel](../persistence/README.md).
FileArtifactStore and SaveExecution are owned there, not by ProjectStorage.

## 编辑器资产工作流

AssetImporter 借用 ProjectStorage 和执行设施，组合 engine/toolchain 转换与项目发布。
源打开、编译和保存保留固定版本、任务身份与关闭协议，窗口隐藏不终止已经接纳的任务。

纯格式转换位于 engine/toolchain；运行资产读取/解码位于 engine/process/asset_loading。
本模块只承担编辑器工作流，不建立第二份项目目录或 GPU 资源缓存。
通用 History 与会话位于 editor/editing；正式保存协议位于 activities/persistence。
旧 AssetSource/AssetSave 协议已删除。AssetImporter 继续拥有导入任务和完成事实，关闭状态由其自身的 AssetImportCloseStatus 表达。

`editor_assets` 为 STATIC，无独立资产 DLL；其安装 include 前缀仍为 lux/engine/editor。
