# Framework v2 最终生命周期补正记录

本轮完成审阅所指出的生命周期与 API 补正，停在 **Framework v2 独立复审**。
没有迁入具体工具，没有增加新的 Runtime、Manager 或信号派发体系。

## 版本与范围

| 项目 | 版本／位置 |
|---|---|
| 审阅基线 | `a83078ff830d1ead46d7a77a3cb7dce4b52e16a7` |
| 生产实现及冷构建 | `a13f4412feccdb47e79f91c2e02ee99a9eecc428` |
| 最终实现 | `af76310c600616cb5d9cd6dda52beb749d2fe56e` |
| lux-cxx | `0a0e7419fc7229df6e372cd35a540249f92250ef` |
| 工作区／分支 | `E:/SyncForder/CodeRepos/lux-engine`；`codex/editor-framework-v2` |
| 新安装 SDK | `E:/SyncForder/CodeRepos/install/Framework-v2-lifetime` |
| 依赖前缀 | `E:/SyncForder/CodeRepos/install/Framework-v2-dependencies` |

最终提交只把安装测试的 `CMAKE_EXPORT_COMPILE_COMMANDS` 设置移到 target 创建之前，
使夹具不依赖外部参数；与冷构建提交的生产源码完全相同。精确差异保存在外部
`final-lifetime/production-equivalence.json`，不将冷构建成绩冒充另一 SHA 的首次构建。
最终 SHA 重新通过 clean tracked 检查、配置、全量构建、二次无工作和适用测试。
lux-cxx SDK 经正式重新配置、安装，版本元数据为上述提交且 `dirty=false`。

## 补正与责任

| 范围 | 最终合同 |
|---|---|
| DockTree | 保存 opaque `PaneHandle`，由 Root ObjectId 和本地代际 PaneId 标识一次注册；不拥有或保存 Pane 地址。全部验证成功才发布布局。 |
| 结构通知 | `PaneChanged`、`ObjectRemoved` 不可复制；原 TSignal 拒绝 QUEUED，返回 `PAYLOAD_NOT_QUEUEABLE`。DIRECT 仅同步借用。 |
| 扩展回调 | lux-cxx 两种原回调包装器支持 noexcept 签名，共用存储／调用算法；Assembly、UI／服务／工具工厂、Capture、枚举显式约束。 |
| 错误声明 | Framework、Scene、Transform、RenderSystem、Script 的描述是唯一 canonical name 来源，ErrorId 由对应描述派生；失败路径仍只构造数值 Error。 |
| Object | `beginDestruction` 交换清空 ObjectId，基类析构不再次撤销旧身份。Element 先同步撤销结构请求，再使身份失效。 |
| UI API | 隐藏 generic mutator，私有化安全点探针；补齐 detached Pane 的 setModal affinity；删除 INVALID_ID，改用 DUPLICATE_PANE；维护重载为无参数 update。 |
| SDK 表面 | 删除 RootTestAccess 友元和原测试访问头，测试只用公开行为；清理相关无用 include。 |

Root 仍唯一拥有 Pane；父链仍非拥有；ObjectRuntime、共享 allocation 回收、CodeLease 和
GPU 退休保持原 owner。没有复制另一套 ID、消息队列、对象树或工厂目录。
Registrar 继续使用小 vector。

Assembly 继续作为同步 openProject 参数，不在这轮引入新的存储寿命。
PaneDescription.name 只提供输入唯一性与工厂参数，尚无持久名称绑定。
**SceneToolRegistrar 不属于冻结合同**，首次真实 SceneSession/SceneToolSet 前重新审查。

## 实际证据

修复前先构建真实旧 SDK 消费者，保留两个非零退出记录：

- `lr0-before-dock`：销毁旧 Pane 后在同地址创建新 Pane，旧捕获布局错误地接受了替代对象。
- `lr0-before-signals`：原 QUEUED 连接被接受，两类通知均在 Pane 已析构后交付了旧指针。

负例不解引用已释放内存，也不使用声明级替代实现。修后 `ui.composition` 及安装消费者
验证销毁、同地址替换、原对象重挂、跨 Root 转移、失效布局拒绝，以及 DIRECT 通知时
对象仍可借用、注册／焦点已经撤销、结构重入被拒绝。

| 验证 | 实际结果与记录 |
|---|---|
| lux-cxx | 全量／二次无工作；56/56；clang-cl 运行新增回调测试，含可抛返回转换拒绝、借用约束、SBO／堆存储和移动；`lr1-*`、`lr4-cxx-*` |
| Editor | 独立冷构建 1060 个动作；最终完整 43/43；`lr3-editor-*`、`lr4-editor-*` |
| PLAYER | 独立冷构建 1012 个动作；最终完整 33/33；`lr3-player-*`、`lr4-player-*` |
| 安装 SDK | 最终重装后 9/9，包括真实 Object／Error DLL、Context-only 消费者及桌面；`lr5-sdk-*` |
| 独立消费者 | object-core、object-ownership、ui-composition、services-core、services-tasks、spatial，9/9；`lr5-consumer-*-tests` |
| 编译拒绝 | 六类真实入口，同头正例及六个可抛签名负例；MSVC 源码／SDK，clang-cl SDK 全部成立；`framework.callback_contract`、`lr5-clang-sdk-callbacks` |
| 公共头 | 45 个安装公共头逐个 C++20／无 RTTI 编译；`lr3-public-headers` |
| GPU | 真实 UI transport、自动桌面 resize／最小化／在途关闭，以及外部 Render 插件；`lr4-editor-tests`、`lr5-sdk-tests`、`lr5-plugin-tests` |
| 闭包 | Editor 553、PLAYER 525、SDK 38 个编译单元及外部消费者；无 legacy／旧 SDK 回退／源码私有头补齐；`lr5-closure` |
| 归档 | 467 份构建／安装／测试元数据冻结；中文空格路径可核验，实际测试日志缺失／篡改被拒绝；`lr5-freeze`、`lr5-evidence-validation` |

完整构建使用 `all -j 4 -- -k 0`，构建与 GPU 验证串行。
原销毁、重入、输入一次交付、固定批次、焦点撤销和原子挂载断言保留。
原私有 drain 死亡探针改为公开 update 的 FRAME_OPEN 拒绝检查；非法析构仍要求异常退出。
维护计数相应覆盖公开 update 的完整阶段，没有删掉旧目标不得收到回调的断言。

修前失败、迁移期编译失败和 MSVC 对依赖 noexcept 模板写法的拒绝原样保留。
SDK 初次安装的提交前版本元数据已通过重新配置／安装校正；最终矩阵使用校正后的依赖。
没有通过削弱 noexcept 约束、添加兼容 API 或手改版本元数据获得通过。

## 保留范围与交付位置

源码及安装 SDK 原生输入接管仍为 **NOT_RUN_USER_DEFERRED**；自动桌面测试不替代它。
Linux、系统 IME、sanitizer、历史性能延期及旧 PARTIAL／FAIL 均保持原记录。
Android 只同步头，没有构建资格；没有补旧长测。

外部证据位于 `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2`：

- `runs/commands.json` 与 `runs/logs/lr*.log`：实际命令、SHA、退出状态与输出哈希。
- `final-lifetime/before/`：真实旧 SDK 失败夹具。
- `final-lifetime/verified-evidence/`：可搬迁核验的必要原始输出。
- `final-lifetime/qualification-af76310c/`：冻结构建及实际测试元数据。
- `final-lifetime/files-af76310c.txt`、`protection-final-af76310c.json`：完整文件变化与用户差异核对。

规定的三个 Engine include 前缀已同步；68 条同步记录逐项核对。lux-cxx 的两个回调头亦核对
Debug、RelWithDebInfo、Android/lux-cxx、开发前缀及资格依赖前缀，与源码字节一致。
历史两份 ZIP 哈希未改变。ProjectBuilder 补丁仍未应用，原用户文件 SHA256 为
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
Pane.hpp 的原注释缩进差异保持独立、未提交。临时 qualification 检出在冻结核验后清理。
实现与本记录分别提交，正常推送实施分支；不修改 main、不删除历史分支、不发布 release。
