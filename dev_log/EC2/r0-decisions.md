# EC2 R0：实施裁定

基线 `248adc4576943cab83976afd8d1d5f31b63b70a9`，EC1 实现 `137b8f441faa1dbb7dfa792b6f01b513327e6248`。
源码消费者、blob、逻辑 include 在 source-consumers.json；实际 File API 目标与编译/链接闭包在 baseline-target-graph.json。
这些是调查记录，不是本轮运行通过证明。唯一状态仍在原 migration-ledger.json 的 ec2 节点。

## EC1 补充包与剩余项

旧责任审计输入为 `54f8a563`。其 RA01–09 对应 EC2 RD01–08（RA09 属 RD05）。
RA10 已由 EC1 完成：EditExecutor 真正执行唯一历史算法；SessionPreparation 拥有准备输入；
SourceAuthoring/开放工厂/manifest v3、不可变目录和类型化意图继续沿用，不重新设计。

| 项 | 当前实际 owner/缺口 | 执行决定和退出要求 |
|---|---|---|
| RD01 | ProjectPublication 公开可改计划同时带 Storage reservation；Operation 在 PACKAGES 写回编码 | 固定只读 plan 与 move-only PreparedProjectPublication；worker 捕获 plan，不能释放 owner 占用；receipt 关联计划与前置版本。保留 ProjectWriteLease。 |
| RD02 | SystemElement 含描述、Feature 配方/provider 准备；Impl::build 直接读取控件 | 草稿归 authoring，注册相关准备归 activities；UI 捕获后调用唯一 prepare。保留未知配置、来源及 indexed-world 拒绝。 |
| RD03 | EditorArtifacts 实现编码、两张票据及 manifest 采用；ProjectPublicationOperation 已实现正式四阶段 | 派生活动拥有操作/结果；复用原 Operation 的包/manifest/采用，不保留第二顺序。Application 只接纳意图、展示和退出协调。 |
| RD04 | ResultsPane/WorkspacePane 借 AppImpl | workbench 正式视图借准确 provider，类型化意图给应用；不增加全量应用访问接口。 |
| RD05 | compile key 带 target，PreviewStore 接受 operation，产物可随意换字段 | 编译输入/预览采用分域；固定完成结果；保留单目标 runtime/retirement owner。配方抽离，不建缓存。 |
| RD06 | layoutResult 同时 status+list IO；LegacyWorkspaceImporter 混文件读取和纯解析 | 状态直接使用 WriteCoordinator；显式目录 IO；固定输入纯迁移计划。保留 selected、版本复查和 marker。 |
| RD07 | AssetImporter 多记录控制只有 Model 路径；Load/Encode 内 TOML recipe | 改 ModelImporter，提取 ModelImportRecipe 字节 codec；原 ModelCooker、Process、发布操作不复制。 |
| RD08 | ProjectOpenData 持独占写锁、manifest、mount/source digest | PreparedProjectOpen 私有完整拥有单元；封装准备和一次采用，不保留旧名 alias。 |

publishProjectFiles 的真实生产调用在 ProjectCreation，包含多文件 journal 与崩溃恢复；Operation 用现有
WriteCoordinator 做不可变包和 manifest。二者语义不同，保留新目录原 journal，不机械删除恢复算法。

## 脚本/资产真实链与最小补正

* ScriptRuntimeSystem 从 ScriptRuntimeHost 取固定 artifact resolver/backends；ScriptPreparer 验证 contract/schema，
  Construction 复制 capability 后 allocateIdentity/createBackend/prepareMethod；ScriptBindings 调用原执行区域；
  completion ingress -> executeStablePoint 恢复。ScriptInstances::revoke/finishRetirement 是原撤销/销毁位置。
* 当前 capability 只有全局 context/dispatch，Mount 持复制的 PreparedScriptApiCapability，无逐实例 prepare/revoke。
  补原 capability 的窄实例准备/撤销拥有关系；真实 ScriptInstanceId 由原 Construction 铸造。
  不用 caller_id/TLS/Lua 可写字段，scope 只保管本实例请求和结果；不改 timer 本地授权。
* 原 loadAsset<T> 组合 portSender -> CPU -> TAssetSerDeser，返回 shared_ptr；原 VfsAssetReadEndpoint 在 Blocking
  调 AssetVfsView::open。当前 ReadAssetImage 无 byte limit，IAssetProvider::open 同样无上限。
  上限沿原请求/VFS/provider 在读/解压前生效；结果保留额度与后端瞬时成本分别记录。
* 新资产叶子置 engine/scene/scripting/assets；组合 Process 与 script，不让 domain 依赖 Process。
  native/Lua 分离；固定可运输 handle/outcome + 原生 scope 保管非平凡结果。无第二 IO/codec/队列执行器。
* 正式宿主接线在 engine/scene/integration/script 的 ScriptRuntimeSystem/ScriptRuntimeHost；Editor Run 和
  PLAYER 都用这一个安装点。脚本资产 provider 不接收 Editor ProjectStorage；只固定 AssetReadPort。

| 能力 | 已有事实 | 本轮 |
|---|---|---|
| raw/typed asset read、结果观察/释放 | 未找到等价 ScriptAbility；已有 native read/codec | 新叶子组合，真实 Lua/PLAYER/Editor Run |
| Delay | DelayAbility + ScriptTimers/ScriptRealDelayProvider | 原实现复用组合验证 |
| 组件读取/命令 | DeferredScriptHost + ScriptBehavior + 原 ECS barrier | 真实组件用例，worker 不写 Registry |
| Physics2D 空间查询 | PhysicsQuery2D 生成贡献、Physics2DSystem publication | 继承，不新增 raycast 子系统 |
| 动画、音频、输入完整绑定 | 未取得完整正式绑定证据 | 不在本轮实现/完成声明范围 |
| 导入、编译、保存、布局/Pane | Editor 产品活动 | 禁止游戏脚本依赖 |

## 工程和验证

R1 先保留现有计划可变性编译证据及原 project 回归，再迁移全部调用。
R1–R9 分批做闭包，新增失败证据不伪称旧行为已重跑。EC2+STRICT 最终干净提交绑定；
原 EC1 Skeleton 三模式、PLAYER、SDK、受影响 UI/GPU 按包执行；旧免验不替代新增资产脚本运行。
不修改历史 dev_log，modules 公共头同步三个 include 前缀。原工作区用户补丁不应用、不提交。
