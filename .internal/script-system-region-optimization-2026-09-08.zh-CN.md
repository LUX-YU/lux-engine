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
| 执行区域、延迟命令、停止与消费者 | 首轮实现和图外真实 Lua 测试通过；Scene 命令装配仍待接通 |
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
