# 验收矩阵：观察事实，不追求测试数量

下列 XL01–XL24 是观察主题，不是要求新增24个可执行程序。优先在原测试/SDK消费者加入必要断言或重新登记迁移后的场景。原行为可以复用证据方法，但最终修改过的实现必须在最终SHA实际运行。

## 1. 总判定

本轮PASS要求：五层归属真实、必要依赖负例命中、原行为不缩水、Windows及实际适用UI/GPU/安装通过、删除项闭合。

Linux/系统IME未测单列NOT_RUN；用户取消补满的旧慢算法样本保持原PARTIAL历史，不阻塞本轮。新增故障不能借未测平台或旧C编号掩盖。

## 2. XL01–XL06：输入与纯内容

### XL01 用户改动与历史快照

**动作：** 核对实际HEAD、输入SHA、用户文件bytes/diff。迁移后对比原工作区；对旧dev_log树执行Git对象差异检查。

**断言：** 原用户改动没有被stage、覆盖或丢弃；移位的tracked ProjectBuilder与用户补丁分别记录；旧快照无改动；前置与实现有清楚祖先关系。

**证据：** 两个工作区状态、旧/新路径对应、raw hash与tracked blob不同口径、历史tree/diff。

### XL02 editing唯一性与异构Store

**动作：** 用真实Scene/Material/Flow在同Store中创建；保存typed key，关闭并复用slot；验证历史/ContentStamp/许可。

**断言：** 不同模型独立历史；旧代际拒绝；current不来自重复缓存；关闭的无分配内容戳查询保持；IEditSession仍只必要多态。

**结构断言：** 原History/Session定义在新E0真实路径只一份，旧源不编译、无forward header；不链接具体模型到E0。

### XL03 三模型CPU独立消费

**动作：** 分别构建只使用安装E0+单模型的消费程序，实际编辑/捕获/撤销重做；审查configure与link闭包。

**断言：** 无Process、Runtime/composition、ImGui、Root、Vulkan、compiler/linker或旧Context；允许实际纯描述/schema/graph/codec。

**不能代替：** 完整Editor里未创建窗口不算独立闭包。

### XL04 作者状态和原子编辑

**动作：** 原Scene混合删除重建+字段、Material节点替换、Flow变量/签名/连接的正负批次；Undo/Redo与NO_CHANGE。

**断言：** 完整源编码、history cursor/revision、observed、dirty、binding/checkpoint符合原规则；失败不部分写入。不是只看返回值。

### XL05 身份与输入清理

**动作：** 原Flow高水位/耗尽测试、重载输入无绑定/BUSY/clone失败、codec读取重入编辑、最后code owner释放。

**断言：** 旧ID不命中新对象；正常Redo/显式恢复有效；数据先于code析构，READING外层不被嵌套操作解除。

### XL06 纯项目与布局

**动作：** ProjectBuilder验证合法/非法配置、Catalog共享快照与失败保留；LayoutPlan输入纯inventory，检查未挂载Root与dirty作者。

**断言：** Builder没有任务或IO（D01），项目纯target不因错误归类链接Process；plan无provider/open/rebind副作用；ViewInfo同一定义；ViewClose错误不反向引入workbench。

## 3. XL07–XL14：活动与可靠结果

### XL07 保存策略与具体角色分离

通用SaveService core consumer只认识ISaveSource/OwnedEncodeJob/WriteCoordinator，不include三种Session或FileArtifactStore。具体三个角色分别使用真实源捕获/采用。审查两个编译单元和链接闭包，不能只看目录名。

### XL08 保存与基线

捕获S10后继续编辑S12；发布S10，current仍S12且dirty。SaveAs保留History/作者ID/Flow发号高水位；ExportCopy不改checkpoint。关闭Session并复用槽位后，迟到published事实保留，不采用到新会话。

### XL09 回调与完成运输

复用P05/R1/R2的真实角色：describe撤销、accept嵌套adopt/ack、accept内collect别的编码完成。确保不会UAF、重复accept、丢完成或永久ENCODING；内层完成后外层dispatch仍有效。Task/TaskScope路径不可用手动shim替代正式资格。

### XL10 同目标写入与共享字节

乱序编码仍按票据发布；失败空洞正确结清；Unknown直到writer退休/结果确认才释放lane；已确认旧回执不破坏同源合法版本衔接；不同origin/外部变更冲突仍成立。原SharedBytes保留同一owner与预算，不重新复制大payload。

不重新跑所有大字节计时；保留1个真实产物和代表大小的owner/limit断言即可，除非本轮实际改了传输算法。

### XL11 Run与结果寿命

实际start/pause/step/stop；完成一张未确认，再排队两张停止，实例回收后第一次读取应得到COMPLETED/CANCELLED/CANCELLED。FAILED包含原payload/code寿命；单项/Run最终确认后失效。没有第二drive或由RunStore猜测结果。

### XL12 编译与预览

Material两个内容版本乱序完成，旧结果不覆盖新目标；最新失败显示准确stale状态。Flow链接失败后修改作者图，再retry仍使用原编译object。operation不可复制/按值移动，结果可共享；关闭View不取消服务仍拥有的编译。

### XL13 TaskMonitor脱UI

在真实Process上创建Monitor、不创建Pane/ImGui；多个订阅者观察任务变更，目录共享同一revision快照；一个订阅销毁不影响任务；取消由Runtime准入。实际安装/链接无view_api、desktop、GUI。TaskView另测借用同一Monitor，不各自抢observer。

### XL14 Workspace文件与恢复范围

旧Alpha/Beta合法布局都迁移，只selected的内容进入RecoveryManifest；空/失效选择和IO/BUSY分类保持；目标已存在同源用户修改不覆盖；marker前中断可重启。真实文件写/删继续共享WriteCoordinator，不能通过直接remove绕过在途责任。

## 4. XL15–XL19：工作台

### XL15 通用widgets与viewport

GraphCanvas使用原node-editor，领域Graph/History不进入widgets。真实画布有界整理后作者布局、pan/zoom、选择和旧UI ID失效仍正确。Material与Scene共享viewport而非互相链接工具UI；测试后端真实按ViewHandle隔离。

### XL16 interaction CPU与访问错误

三类interaction不需要窗口编译/运行。回收另一个Session的cleanup回调访问仍活目标时BUSY保持手势/选择/来源；selection-only也保持。真实STALE才清理旧身份；wrong thread保持原错误。不要把错误归一成成功或BUSY。

### XL17 正式UI草稿/排队来源

实际Flow属性草稿S0，外部合法提交S1，display刷新后Apply应拒绝旧草稿，原S1完整编码/历史不变；Revert后新捕获可正常提交。Material/Flow Canvas信号在S0入队、BEGIN前S1变化同样拒绝。BUSY恢复同一phase/payload/based_on，不重启和rebase。

不能用手工给模型传旧expected的单元测试替代这条真实UI缓存链。

### XL18 离树/挂载/关闭

真实Pane/Element在离树构造不注册Root；容量/工厂失败保留旧UI；提交后通知再请求关闭进入后批；payload/挂载token析构不让执行栈悬垂。DetachedView移动赋值先清旧节点再释放code。永久关闭错误停止自动重试，BUSY保持原记录。

### XL19 新双视口与原生输入

正式SceneView+ViewHost+DesktopShell，两View同作者、独立相机/尺寸/高亮；从一个编辑另一Undo；关闭一窗只退休它的资源。保留实际GPU输出回读/拾取/validation日志。Windows原生输入/capture/焦点按原脚本经过新桌面。两个旧显式GPU模式分别记录，不冒充新双View。

IME没运行就NOT_RUN。编译默认CPU模式的consumer即使目录叫GPU也不能计GPU通过。

## 5. XL20–XL24：概念、依赖和工程收尾

### XL20 概念的真实正反编译

Material/Flow真实actions是正例，共用同一deliverInput。错误Result、bool返回、仅&&可调用、非void expected等负例命中约束。内部概念不为测试强装进SDK；SDK使用公开View链接实际生产实例化。不能说concept证明snapshot深层拥有。

### XL21 动态边界与代码寿命

原Store异构角色、保存撤销、encode job错误/取消/最后owner、View factory所有权仍通过。检查同一对象只经过一个必要动态边界，不新增虚接口后再包函数表。共享身份/元信息必要DSO边界不被STATIC改动复制。

旧C03按P11原故障合同保留；这不豁免本轮新工厂或保存边界自己的生命周期回归。

### XL22 安装、头和生成

全新install prefix；改动后的public headers逐个C++20独立编译；正向SDK模型/活动/工作台按真实需要链接；旧已删除headers/package不参与。删除生成输出后能正确再生，第二轮无工作；无法从旧SDK偶然找到缺失include。

### XL23 依赖结构

运行N01–N14；检查源/target/安装/实例化四张表一致。实际未知边不能默认为叶；对同夹具修复后的正例也运行。新层源不能依赖原Context/transition。测试依赖不得倒灌production。

### XL24 删除、旧岛与文档

每条MOVE/DELETE都有Git差异、consumer更新和旧文件退出；SPLIT每个符号有唯一新位置。零消费者旧target/头删除，不留alias。旧产品剩余清单有P11/P12责任且无新增消费者。根README区分新链与旧入口；.internal是唯一施工材料，历史收据原样保存。

## 6. 失败时如何处理

| 情况 | 正确动作 | 不允许 |
|---|---|---|
| 新代码正例失败 | 保存第一次日志，修代码/真实配置后重跑 | 改断言弱化含义 |
| negative因缺包失败 | 先恢复正例依赖，再测试禁边 | 将非零退出当PASS |
| GPU/系统能力不可用 | 标BLOCKED或NOT_RUN，说明是否当前必测 | 偷换CPU/Fake模式 |
| Linux没环境 | 按用户范围NOT_RUN，不阻塞 | 报跨平台通过 |
| 原历史C01/C03/C04仍失败 | 保留原结果/责任 | 挂上新的本轮缺陷延期 |
| 旧性能样本没满 | 保留66/100历史与范围变更 | 伪造剩余样本或重启无关长测 |
| 用户修改冲突 | 保留bytes、隔离tracked实现、报告重定位 | reset或纳入本轮提交 |

## 7. 计数与结果汇总

结果清单应按XL主题映射实际test名/命令，而不是要求总数固定。可以保留一个行为映射文件：old_test→new_test→old_assertions→new_assertions/重定位说明。修改公开API导致测试源码变化时说明等价观察，不盲目要求字节相同；未变测试体则原样保留。

最终记录一个完整结果向量：结构PASS/FAIL，Windows各组，安装各组，GPU各模式，Linux/IME/ASan范围，旧故障，性能是否涉及。总PASS不覆盖NOT_RUN，也不把允许延期的未测项重写成成功。
