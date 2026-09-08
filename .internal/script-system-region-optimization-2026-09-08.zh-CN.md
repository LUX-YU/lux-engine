# ScriptSystem 受控区域与激进优化实施记录

用户已批准本轮实际实现。入口 `5a309191f08f43e220a9309961f9de677de9d071`，
生产参照 `7b5e1dd4`（与 `f64cadde` 生产代码一致）；lux-cxx `3100f54d`、toolset `99c3d048` 不变。
原 main 的五项 tracked、两项 untracked 修改不纳入提交。原 SR-6 与 VTune 证据不重标、不重打包。

## 已批准契约

- 原生 System 按自己的调度契约直接读写组件；脚本的 ECS 修改通过封装延迟提交。
- 一个 script-capable Hook 区域是一批；执行中实例、绑定和 prepared 存储有效，区域末尾提交。
- 停止/销毁请求不立即失效；未捕获错误只停故障实例，其他实例继续。已接受命令不自动回滚。
- 唯一 stable Hook、真实 step、恢复预算、每次 pop 后同 frontier 接入与 Event claim/callback/complete 不变。
- 保留 Event 等待，优化来源、登记、路由和结果存储；七组件所有权与 backend C ABI 不变。
- 图外调用使用显式受控区域；公开边界与 Lua 保留必要错误检测，内部受信调用不重复防护。
- Lua 默认 record 可迁移拥有型 userdata，允许独立值编辑；子视图保活根值，不借用 ECS 暂存内存。
- 类型范围不扩展，自定义表示保持单一规则和方向；不增加非平凡跨挂起协议。

## 实施与验证状态

| 工作 | 状态 |
|---|---|
| 基线、环境与原始证据固定 | 独立基线 Toolchain 108/108、Developer 123/123；身份见外部 identity.json |
| 执行区域、延迟命令、停止与消费者 | 图外与 Scene 实际 Lua 路径通过开发检查；安装资格待最终源码 |
| 派发/调用/恢复热路径 | 待实施 |
| Event 等待 | 待实施 |
| FlowForge IR 与 frame | 待实施 |
| Lua 入口、userdata、生成安装 | 待实施 |
| 联合正确性、安装与配对成本 | 待实施 |

仅 RelWithDebInfo；all -j 4 -- -k 0，构建/测试串行。资格从 clean tracked commit 独立 clone。
最终报告分别记录实际实现、通过检查、成本、撤销实验与未完成项；本文件不是新资格通过声明。

### 首轮开发检查

`script-region-opt/logs/region-dev/build-11.log` 全量构建通过，`ctest-8.log` 受影响集合 75/75。
该目录位于 `E:/SyncForder/CodeRepos/build/RelWithDebInfo/`，使用当前开发工作树，不作 clean clone 资格。
保留此前失败日志：旧停止语义断言、遗漏显式区域入口、测试资产 prepared 容量不足和编译错误。
新增 Lua 夹具分别运行普通 stop 同批合法、真实嵌套 fault 后 provider 为零、输入错误 recovery，
以及 patch/destroy 接受后脚本报错仍提交。不可将未捕获实例错误与基础设施失败混用。
ResumeRing 容量 3、预算 2 的 17 次循环最终实际调用/恢复/析构均 35、backlog 0。

### Scene 组件命令边界

`ScriptRuntimeHost::components` 是原生装配方提供的冻结 typed 描述，`command_capacity` 固定命令数与
payload 字节上限。仅当完整配置包含启用 Entity scope 时才准备队列，未解析配置也计入。
`ScriptRuntimeSystem` 私有拥有 `EcsCommandBuffer` 与 `DeferredScriptHost`；不建立额外 TaskGraph 或 tick。
初始 BeginPlay、Hook/恢复、增量生命周期及关闭清理各借用 writer；所有借用结束后才允许 apply。
现有 Hook `committed` 边界依次是：Simulation 的原生命令 apply → 脚本命令 apply → 生命周期协调。
生命周期回调新登记的命令等待下个 Hook barrier；最终 shutdown 回调的命令在关闭完成后的串行边界提交。
冷安装失败仍是装配回滚，不宣称未成立 Scene 中的命令已经生效。

脚本 enqueue 容量不足返回 false，之前接受的命令不被 poison；提交时旧 Entity 或组件形状冲突拒绝该条，
并继续处理同批其他命令。原生命令默认严格错误策略不变。`commandStats()` 提供接受/拒绝/提交拒绝、
discard 与最后一个提交错误的值快照，不输出可写存储。

`scene-build-2.log` all 与 `scene-ctest-2.log` 9/9 通过。新增 `SCENE_DEFERRED` 在真实 NextStep 恢复中
提交 patch/destroy，同时允许另一实例 provider 继续；普通/故障两支各检查 2 次恢复、2 次 provider、
2 次 EndPlay、backlog 0，并在 0/1/2/4 worker 与 interpreter 既有配置中执行。
原生 System 直接写组件仍合法；脚本读取在当前区域看到旧值，barrier 后才应用自己的修改。

### Event 登记窄改

`eventSource` 复用执行索引的固定 mount slot，并在 Instances 中一次检查 ACTIVE、完整 instance 与 admission
scope/epoch/local index。结果预留到 waiter 提交之间无用户代码；Admission 仅是该短区间的不可复制预检值，
不获得长期调用权限。取消、copy pin、claim 截止序号和最终结果所有权不变。
广播路由固定为 endpoint 数组；定向路由仍有界哈希，直接使用插入返回的迭代器。
`event-ctest-2.log` 17/17；`event-ctest-3.log` 新增 37 次复用通过，高水位 2、74 次恢复、backlog 0。
成本待配对核验，不以源码删除行数宣称收益。
