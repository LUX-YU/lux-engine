# LUX Engine — P01 复审与定向补正指令 R1

日期：2026-09-28  
审阅分支：`codex/editor-redesign-v4`  
P00 输入：`a7234602aaeab86bb947214dcd2ea4eb1b314cfa`  
P01 实现：`f167cb803011c9b37fa40edcc7cfc4652e7958bf`  
P01 证据及本次审阅 HEAD：`a653a89bc3cfae25fbf2e0777b2b757b93583ba5`

## 0. 放行结论与边界

**本次审阅结论：PARTIAL；先完成本文限定的 P01 补正，再审阅放行 P02。**

这不是重做 P01，更不是重新设计整个 Editor。历史纯化、会话唯一所有权和限期兼容桥已经有实际实现；本轮发现一项新的关闭契约问题、一项基础层门禁缺口，以及一项证据可搬运性缺口。三项都在基础层或本阶段交接范围内，不应带入 Scene 作者模型后再解决。

实施方的原收据仍是当时运行结果的历史记录，不覆盖或删除它，也不把既有 35/35 CTest 日志改成失败。新增补正实现和独立证据，清楚区分“既有测试通过”与“审阅发现尚未覆盖的问题”。

### 本轮实际做了什么

- 通过 GitHub 连接读取固定提交的关键实现、头文件、CMake、检查器、测试、收据和已归档的 CTest 日志。
- 对 `SessionStore`、`SessionState`、checkpoint、permit、私有桥及若干旧工具采用路径进行静态审阅。
- 在本地运行**仓库原检查器**的小型图/源码夹具。检查器文件的 Git Blob 校验值与仓库一致：`f5491931e4380753b8a8e709e9e6898eedf8e77b`。夹具使用原规则中相关的 P01 字段，不声称复制了完整引擎构建图。
- 对验收脚本的 Windows 路径处理做了独立的路径表达式验证。

**没有在本轮环境完成引擎构建、完整 CTest、安装消费者、真实 GPU 或完整 `verify.py` 重跑。** R01 是由真实源码调用链确认的条件性缺陷，尚未在完整项目运行复现；R02 的小型检查器漏报已独立运行；R03 的路径限制由源码和本地路径表达式验证确认。不得混用这三种证据等级。

## 1. 已有实现中应保留的部分

| 项目 | 本次看到的实现 | 不应回退的边界 |
|---|---|---|
| 纯历史 | `editor/history` 的公开类型不再含保存 API、saved/clean 等字段；构建目标为 `edit_history`。 | 不把保存职责加回历史，不再复制历史算法。 |
| 会话所有权 | Store 的槽位持有 `unique_ptr<IEditSession>`；存在 RESERVED/PREPARED/PUBLISHED，发布前查询不可见。 | 不改成窗口拥有会话，不用额外共享 owner 规避失败路径。 |
| 代码保活 | 槽位先声明 `CodeLease`，后声明会话对象；回收中设置重入限制。 | 对象及其析构代码最后一次使用完成之后才能释放 lease。 |
| 保存基线 | `PersistenceCheckpoint` 独立比较 StateId、绑定版本和已采用发布序号；SessionState 组合它。 | 不使用通知版本代替 dirty，不重复保存当前历史状态。 |
| 准入 | EditScope 禁止复制/移动；ClosePermit、BindingChangePermit 是 move-only，移动后旧对象不再持有 gate。 | 不在具体业务侧重新增加一套并行 busy/closing 标志。 |
| 兼容桥 | `LegacyPersistenceState` 组合新 checkpoint，读旧工作副本的唯一历史，不做 IO；Material/Scene 的主要候选采用路径一起移动 history/checkpoint。 | 桥继续只供旧产品使用，最迟 P12 删除，不进入新模型 SDK。 |
| 已有测试 | 仓库归档日志记录 35/35，包含新增会话、scope 编译负例及 Scene 保存模式。 | 不删原用例、不降低断言、不用测试计数替代行为覆盖。 |

以上是本轮已读源码和日志所支持的判断，不等于重新穷举验证了全部 747 项成员处置或全部失败组合。

## 2. R01 — 关闭提交调用可抛异常的完整描述查询

### 2.1 位置与证据

- `editor/sessions/src/SessionStore.cpp`：`SessionStore::close()`，约第 215–232 行。
- 同文件：`SessionStore::describe()`。
- `editor/sessions/include/lux/engine/editor/sessions/IEditSession.hpp`：`SessionInfo`、`IEditSession::describe()`。
- `editor/sessions/include/lux/engine/editor/sessions/PersistenceCheckpoint.hpp`：`BoundSource::location`。

实际调用关系：

```cpp
SessionResult<void> SessionStore::close(ClosePermit& permit) noexcept
{
    // 检查 Store、代际、owner 和 permit ……
    const auto current = describe(permit.stamp_.session);
    // 比较内容戳后才释放许可、销毁对象 ……
}

virtual SessionInfo IEditSession::describe() const; // 不是 noexcept
```

`SessionInfo` 包含拥有字符串的 kind 与 binding。这里为了比较一个 `ContentStamp`，会调用完整描述逻辑，可能复制路径等字符串，也允许派生实现抛出异常。

### 2.2 影响及最小触发

候选准备和发布成功后，令测试会话的 `describe()` 在后续调用时抛出 `std::runtime_error`，然后正常取得 ClosePermit 并调用 `close()`。按照当前调用链，该异常会穿过 noexcept 边界，导致 `std::terminate`，而不是返回 SessionResult 或由外部错误处理接住。

这不依赖假设“插件恶意破坏内存”，也不必制造全局内存耗尽：`describe()` 当前签名本来就允许抛异常。长来源地址的复制分配是另一条现实的失败途径。

本轮未在完整引擎内运行此场景；实施方必须先加入确定性的回归，保留修复前结果，再修复。

### 2.3 本轮固定采用的设计

**把关闭校验与展示性描述分开；关闭只读取无分配的内容戳。**

允许并要求对原 P01 接口草图做这一处小补充，记录为设计决议，不新增管理器或另起一套会话协议：

```cpp
class IEditSession {
public:
    virtual ~IEditSession() noexcept;
    [[nodiscard]] virtual SessionInfo describe() const = 0;

private:
    friend class SessionStore;
    [[nodiscard]] virtual ContentStamp currentContent() const noexcept = 0;
    [[nodiscard]] virtual SessionResult<ClosePermit>
    prepareClose(ContentStamp expected) noexcept = 0;
};
```

`currentContent()` 的契约：只读当前会话身份和历史状态身份；不复制字符串，不查询 UI，不执行 IO，不通知 observer，不发布其他操作，不改变 gate。具体实现必须通过已有历史/内容状态取得标量值，不能维护第二个会变旧的 current 副本。

将 `SessionStore::close()` 改为：在既有 owner 线程、代际、permit owner 检查后，在受保护的回调范围内读取 `currentContent()`；核对与许可内容戳一致；验证成功后才消费许可和回收槽位。维持回收期间拒绝 Store 修改和原有 lease 析构顺序。不得仅为了绕开查询而删除内容戳校验。

完整 `describe()` 仍用于展示性查询和允许失败的准备阶段。其可能抛异常的契约保留并写清，不能简单把该虚函数也标成 noexcept 来隐藏问题。

### 2.4 必须修改的文件/类型

| 文件/类型 | 动作 |
|---|---|
| `IEditSession.hpp` | 增加 private 标量查询；补充分配、重入及无副作用契约。 |
| `SessionStore.cpp` | 删除 close 内对完整 describe 的依赖；保留 owner、代际和许可校验。 |
| `editor/sessions/test/sessions.cpp` | FakeSession、OtherSession 及其他实际派生测试类型实现标量查询；增加故障测试。 |
| 类型索引与施工规范 | 记录这一接口补正，P02 的 SceneSession 以后直接实现新契约。 |
| 安装消费者 | 使用新的公开头与库重新构建；不得用转发别名维持旧虚表。 |

### 2.5 验收新增项

| ID | 场景 | 修正后必须观察到 |
|---|---|---|
| X01-R1-01 | prepare/publish 后，完整 describe 被设置为抛异常；正常 prepareClose/close。 | close 不调用完整 describe，成功关闭；无 terminate。修复前失败保留。 |
| X01-R1-02 | 记录 describe 调用次数，绑定一个长来源地址，再关闭。 | close 不增加描述调用次数；关闭无需复制该来源地址。 |
| X01-R1-03 | 关闭内容戳不匹配、错误 owner、移动后旧 permit、重复消费、槽位复用。 | 对象未误删，旧许可不解锁或删除新对象。保留原测试，不另造更弱替代。 |
| X01-R1-04 | 独立 describe 抛出后，恢复查询或对其他槽位进行合法操作。 | CallbackScope 已恢复，不留下永久 callback_depth/BUSY。 |
| X01-R1-05 | 关闭与放弃已准备候选，检查对象→历史/source→lease 的既定责任。 | 修正不改变已经验证的生命周期顺序。 |

不允许的处理：仅去掉 close 的 noexcept 使异常无定义地向产品退出流程传播；把 describe 改成 noexcept；catch 后返回成功；通过删除测试或把进程终止声明为预期通过；添加一个平行缓存内容戳来减少调用。

## 3. R02 — P01 基础层的允许依赖尚未实际成为门禁

### 3.1 当前检查器具体缺什么

`editor/tests/architecture/rules.json` 已写出 `editor_contracts`、`edit_history`、`edit_sessions` 的 dependencies，但 `check_editor_boundaries.py::inspect()` 主要使用这些条目的 name/path 识别“新目标”，并未把 dependencies 当成允许依赖约束执行。

它确实检查了若干重要错误，例如 engine→editor、新模块→若干旧目标、模型→UI、跨私有头、桥禁入和过期路径。问题不是“检查器完全无效”，而是：

- 基础层目标名不是 `_model`，不会获得模型专用 UI 规则。
- 基础层访问 SceneRuntime 没有对应禁止规则。
- `edit_history → edit_sessions` 等反向关系不因违反表中 dependencies 而报错。
- 新目标未被列入 legacy 的错误依赖，不会因为是错误的职责边界就自动被禁止。

### 3.2 本轮独立执行的小型结果

使用未经改动且 Git Blob 匹配的检查器；图/源码输入为针对相关 P01 规则的最小夹具，不是完整引擎，也不是实际给仓库添加这些依赖。

| 输入 | 架构期望 | 当前检查器结果 |
|---|---|---|
| edit_sessions → edit_history | 允许 | 无 findings，符合预期 |
| edit_sessions → editor_context | 拒绝 | 有 findings，符合预期 |
| edit_history → edit_sessions | 拒绝 | 无 findings，漏报 |
| edit_sessions → scene_composition | 拒绝 | 无 findings，漏报 |
| edit_sessions → modules/function/ui 的 ui target | 拒绝 | 无 findings，漏报 |
| edit_sessions → 中间目标 → ui | 拒绝 | 无 findings，漏报 |
| sessions 源文件 include `lux/engine/ui/Pane.hpp` | 拒绝 | 无 findings，漏报 |

**没有据此声称当前 edit_sessions 已经依赖 UI 或 Runtime。** 已读的基础层 CMake 仍然比较干净；这里是防止后续回流的门禁缺口。

### 3.3 限定修正范围

只补齐 P01 三个基础目标以及它们的直接/传递边界，不要求本轮实现全能 CMake 解释器。

当前允许关系以实际 CMake 为起点：

```text
editor_contracts : 无 Editor/运行时/UI 服务依赖
edit_history     : editor_contracts；必要的 lux-cxx compile_time
edit_sessions    : editor_contracts、edit_history；现有 identity、container 等精确基础依赖
```

标准库、当前已使用的资源身份小类型和容器支持不应被粗暴禁止。外部依赖以精确 target/规范化 alias 分类，不允许用“所有 engine 都允许”“PRIVATE 不算依赖”或未解析目标直接跳过的方式绕过基础层规则。

对 Editor 内部目标实际使用 dependencies 作为直接依赖约束；对 UI、GPU、SceneRuntime、ProjectStorage、旧 Context/工具以及 transition 的禁边检查其传递闭包。正常共享基础依赖不得被误报。

保留现有 CMake 导出、alias、LINK_ONLY、私有头和配置检查，不换成只扫描一条字符串的新脚本。新增负例应复用仓库既有“真实 CMake 夹具→导出→检查器”的路径，补充实际源码 include 负例。

### 3.4 文件与验收

修改 `rules.json`、`check_editor_boundaries.py`、`test_editor_boundaries.py` 及确有必要的 CMake 导出支持；更新规则说明。没有理由修改 Scene/Material/Flow 业务或放宽既有规则。

新增测试 X01-R2-01 至 X01-R2-05，分别覆盖上表五项漏报；每项要求修复前可证明漏报、修复后返回非零且命中正确规则，修复依赖后同夹具通过。另保留合法基础依赖和原 Context 禁边两个对照。

不要把所有未知失败都当成成功阻断；缺第三方包、JSON 格式错误和架构违规必须分开记录。

## 4. R03 — 证据验证仍使用生产机器的绝对路径

### 4.1 位置和具体表现

`dev_log/P01/history-member-proof.json` 的 raw_inventory 仅给出：

```text
E:\SyncForder\CodeRepos\build\p01-evidence\ast\inventory.json
```

而 `dev_log/P01/verify.py` 直接读取 `Path(proof["raw_inventory"]["path"])`。这个地址可用于记录“原始证据在哪里生成”，却不能成为换机器后的唯一获取方式。

脚本还使用 `Path(x["log"]).name` 识别 Windows 日志路径。在 POSIX 环境下，反斜杠不会被当作目录分隔符，该表达式不会得到 `architecture-current.log`。本轮已独立运行这个路径表达式并确认差异。

这是证据包可搬运性问题，不证明当时 AST/测试结果造假；也不应因此删除原始路径或重新生成与原 SHA 不对应的所谓替代原始记录。

### 4.2 修正内容

1. 将 P01 验证实际消费的 AST inventory 作为固定归档文件交付，或作为明确可取得、带哈希的配套证据包交付。优先使用 `dev_log/P01-R1/evidence/P01-inventory.json` 这样的仓库相对归档地址。保留原始路径为 provenance。
2. 在补正证据索引中增加 archive_path（或等价明确字段），关联原 proof 和其原始文件 SHA-256；原 proof 的事实与哈希不改写。让同一验证器通过索引/显式 evidence root 找到归档，不复制一套平行验证逻辑。
3. 日志定位统一使用现有 archive_log，不再按宿主平台解析生产机器上的 Windows `log` 字段。确需解析原始路径时使用显式平台路径规则，但不能拿它作为默认文件读取位置。
4. 缺少归档证据时输出明确的“证据不完整”并非零退出，不改成跳过、不只验证几个 JSON 中写着 PASS。
5. 原收据、原测试失败记录和当时的实现 SHA 保留；补正记录引用它们，不能悄悄改写历史测试结果。

### 4.3 验收

- X01-R3-01：复制仓库及所需归档证据到不同根目录，保留所需 Git 对象，运行纯证据验证；不访问实施者 E 盘路径。
- X01-R3-02：原始路径使用 Windows 风格，在另一平台或路径模拟测试中仍按 archive_path/archive_log 找到正确文件。
- X01-R3-03：移走或篡改一份所需归档，验证必须失败且指出具体证据，不应返回 PASS。

这里要求的是轻量证据验证可搬运，**不是追加整个引擎的 Linux/Android clean-clone 或完整 IME 资格任务**。

## 5. 执行顺序、改动白名单和交付

### 5.1 顺序

```text
固定 a653a89… 基线和证据
    → 添加 R01 / R02 的失败用例并保存修复前结果
    → 修正关闭查询契约
    → 补齐三基础目标的门禁
    → 修正归档与路径解析
    → 运行原 P01 回归和新增用例
    → 独立提交实现、独立提交验收材料
    → 停在 P01，交由复审
```

实现前先检查远端和工作区。若已有用户新修改，不 reset、不覆盖、不 force-push；按真实祖先关系记录输入。无需从 P00 重建，也不重写已经推送的 P01 提交。

### 5.2 本轮允许修改的责任范围

- `editor/sessions`：R01 的窄接口及 Store 关闭实现、相关测试。
- `editor/tests/architecture`：R02 的规则、检查器和负向夹具。
- 直接受新接口影响的安装消费者，以及必要的导出更新。
- `dev_log`、本地唯一施工账本/规范和新证据：R03 及补正交接。

不得为了方便引入 `SessionStoreV2`、第二组 owner/保存状态、新的 Context、永久桥、P02 SceneSession 空壳。P01 现有纯历史算法、checkpoint 规则和桥的 P12 截止时间保持。

### 5.3 必须保留并执行的回归

原 X01-01～06 及本阶段相关 Q 场景；受影响的完整 CTest 集合；五组安装消费者；实际 P01 依赖图检查；原有作用域复制/移动负例。新增用例按 R01/R02/R03 映射记录，不把测试数当唯一门槛。

最终明确使用 `LUX_EDITOR_MIGRATION_STAGE=P01`。修改安装接口后重新构建、安装并重新配置消费者，不能只跑之前已生成的二进制。第二轮无新增工作作为增量构建证据保留，不把它当成完整性证明。

原 C01、C03、C04 继续保留原失败判定，按既定阶段处理，不顺手修补：

| 旧缺陷 | 责任阶段 |
|---|---|
| C01 布局恢复 | P09 / P12 |
| C03 旧命令查询代码保活 | P11 |
| C04 菜单连接失败未阻止创建 | P12 |

### 5.4 收据和交接

建议补正证据目录使用 `dev_log/P01-R1/`，作为一次补正运行快照，不成为第二份施工账本。原 `dev_log/P01/` 保留为历史快照。若为使旧验证器可搬运而修改其脚本/补充归档，提交中明确区分“验证器修复”与“原执行结果未改变”。

收据包括输入/实现完整 SHA、前置证据、三个问题的处置、修复前失败、修复后命令与日志校验、到期删除检查、唯一 owner、无越界说明。状态只用 PASS / PARTIAL / BLOCKED。

只有 R01 修复、R02 有实际负例拦截、R03 证据可取得且验证可搬运之后，才建议放行 P02。原旧缺陷不影响这一决定。

## 6. P02 的条件性准备说明

本节不构成当前开始 P02 的授权。补正复审通过后，继续原 P02 文档，不重排阶段。

P02 的 SceneSession 直接实现修正后的窄会话接口；组合 SessionState、作者源和唯一历史；复用已有 WorldObjectId。不要通过新标量查询又引入第二份 current/dirty 缓存。模型目标不得引用 UI、SceneRuntime、ProjectStorage、旧 SceneEditor/Context 或 LegacyPersistenceState。新增加的 P01 门禁继续运行，并为 scene_model 增加其自身精确边界。

本阶段的旧工具仍属于迁移过渡，不因类名仍存在就全部删除；按既有到期清单处理。目标是无窗口、无 GPU、无运行实例的真实场景编辑与冻结快照，不是新外壳转发到旧 Impl。

## 7. 可直接发送给实施 LLM 的指令

> 本轮只执行《P01 复审与定向补正 R1》，不进入 P02，不修改 main。以远端实际 HEAD 和 `a653a89bc3cfae25fbf2e0777b2b757b93583ba5` 的祖先关系确认输入，不重置用户修改。
>
> 保留既有纯历史、SessionStore 唯一所有权、SessionState checkpoint 和私有桥。只处理三个问题：关闭提交的 noexcept/完整 describe 混用；P01 三基础目标的直接及传递依赖漏报；验收脚本对原机器路径的依赖。
>
> 先保留新增失败证据，再按文档增加私有无分配 currentContent 关闭查询、修改 close、补充真实 CMake 负向夹具、补交 AST inventory 归档并按归档路径验证。不得把 describe 直接标 noexcept，不得删除内容戳校验，不得 catch 后返回成功，不得放宽架构规则或跳过缺失证据。
>
> 完成原 P01 回归与本文新增用例，最终运行 P01 门禁；对变更后的 SDK 重新安装并重建消费者。C01/C03/C04 保持旧失败与原责任，不顺手修复。独立提交实现和验收记录，正常推送实施分支，停在 P01 等待复审。

## 8. 固定源码与证据索引

全部链接固定在本次审阅 HEAD，不随分支移动。

- [S01 SessionStore.cpp](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/sessions/src/SessionStore.cpp#L193-L233)
- [S02 SessionStore.hpp](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/sessions/include/lux/engine/editor/sessions/SessionStore.hpp)
- [S03 IEditSession.hpp](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/sessions/include/lux/engine/editor/sessions/IEditSession.hpp)
- [S04 SessionState.cpp](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/sessions/src/SessionState.cpp)
- [S05 PersistenceCheckpoint.cpp](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/sessions/src/PersistenceCheckpoint.cpp)
- [S06 LegacyPersistenceState.cpp](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/transition/LegacyPersistenceState.cpp)
- [S07 基础测试 sessions.cpp](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/sessions/test/sessions.cpp)
- [S08 check_editor_boundaries.py](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/tests/architecture/check_editor_boundaries.py)
- [S09 rules.json](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/tests/architecture/rules.json)
- [S10 verify.py](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/dev_log/P01/verify.py)
- [S11 history-member-proof.json](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/dev_log/P01/history-member-proof.json)
- [S12 原 CTest 日志](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/dev_log/P01/logs/final-ctest.log)
- [S13 P01 原收据](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/dev_log/P01/receipt.json)
- [S14 edit_history CMake](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/history/CMakeLists.txt)
- [S15 edit_sessions CMake](https://github.com/LUX-YU/lux-engine/blob/a653a89bc3cfae25fbf2e0777b2b757b93583ba5/editor/sessions/CMakeLists.txt)

配套复现脚本与结果见同包 `reproductions/`。它们是本次审阅的小型探针，不是替代引擎构建的测试成绩，也不应直接成为产品源码。
