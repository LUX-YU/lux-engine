# P08 复审与 R1 定向补正指令

**日期：2026-09-30**  
**仓库：LUX-YU/lux-engine**  
**分支：`codex/editor-redesign-v4`**  
**本次验收 HEAD：`8f210a282109ec9e0dae3f32f88bc04359ef2c65`**  
**被验收实现：`6a08a75a162a056a4d1cef1777eae9759f593b3d`**  
**前阶段验收：`833d7efb18f8349cda9968ac3e4ea24ff2b54c60`**

## 0. 结论与严格范围

**P08 主体方向认可，暂不放行 P09。本轮仅补正 R08-B01：三类 interaction 将临时访问失败等同于会话失效，错误丢弃手势／选择。**

这不是重新设计 UI、SessionStore 或交互框架。已有离树构造、Root 准备—提交—通知、DetachedView 所有权、原子作者编辑、History、SessionState、运行与保存链应保留。正常修正应集中于三个 interaction 的 `cancel()`、`synchronize()` 及对应测试与契约说明。

旧 P08 的 PASS 收据和运行日志是该时点的历史记录，不能回写为新成绩，也不因本轮发现未覆盖路径而删除。P08 R1 另行提交实现和验收记录，完成后仍停在 P08。

### 0.1 本次审阅与运行边界

已经通过 GitHub 连接器核对分支／父链、P08 README／文件清单／检查器、三个 interaction 实现、SessionStore 访问与回收路径、真实 UI 挂卸代码、拥有单元及部分实际测试和 CTest 汇总。

独立运行的仅为**原 MaterialInteraction.cpp + 明确标注的访问／模型隔离头**。原 CPP 的 Git Blob SHA-1 已与连接器的 `6bb3c27ebc361d65c4fb319b86bb9387b3e68383` 完全核对；没有修改生产 CPP。隔离头负责注入 BUSY／STALE_SESSION，不是实际 SessionStore、MaterialSession、CodeLease、codec 或安装 SDK。GCC 14.2 的 O0/O2 各运行四场景，共八次。C++23 仅为隔离头使用 `std::expected`，不要求工程升级 C++20。

**没有独立执行完整引擎构建、139 项 CTest、PLAYER、十二组 SDK、GPU、实际的“关闭 B 导致 A 同步”回归，也没有运行整个归档检查器或重算所有远端日志哈希。** 实际 SDK 负例是本轮实施必须完成的事项。`probes/` 不能替代它。

## 1. 应保留的 P08 成果

1. 三类 interaction 拥有临时领域批次、选择与起始戳，只借用 typed Session access/key。Preview 不直接改作者，Commit 调原领域 apply；Material/Flow 持久布局仍由作者编辑管理。
2. Pane/Element 已有真正不登记 Root 的构造路径。Root 准备检查线程、dispatcher、树、重复 PaneId 和容量，提交前验证准备有效性。
3. commit 在通知前把准备记录移成局部唯一 owner；通知中替换外部令牌不会删除正在访问的记录。挂载事实与通知失败分开。
4. Root 只登记，LuxObject 保留非拥有父链；DetachedView 组合 code 与 Pane，移动赋值使用完整旧拥有单元，避免先释放 code 再析构 Pane。
5. 当前真实 UI 测试覆盖构造失败、容量拒绝、过期准备、Root 先销毁、draw／signal 关闭请求、代际拒绝和通知 FULL；并非只有 FakeHost。
6. 源树按各工具 interaction 与一个 view_api 主题组织。本轮不再搬目录或新增业务库。

上述认可不等于重新穷举全部 UI/ImGui/插件错误。报告披露的 OOM 项目终止契约、P10/P13 新产品 GPU 资格及旧 rooted 消费者期限不在本轮扩大。

## 2. R08-B01：错误种类丢失导致破坏性“失效清理”

### 2.1 涉及文件与符号

| 文件 | 直接修改符号 | 责任 |
|---|---|---|
| `editor/tools/scene/interaction/src/SceneInteraction.cpp` | `SceneInteractionGroup::cancel/synchronize` | Scene 手势与封闭来源选择 |
| `editor/tools/material/interaction/src/MaterialInteraction.cpp` | `MaterialInteraction::cancel/synchronize` | Material 手势与节点选择 |
| `editor/tools/flowforge/interaction/src/FlowInteraction.cpp` | `FlowInteraction::cancel/synchronize` | Flow 手势与节点选择 |
| `editor/editing/sessions/src/SessionStore.cpp` | **只读核验** `Impl::slot`、`find`、`close` | BUSY 与 STALE_SESSION 的原有区别，不应为此修改 |
| `editor/editing/sessions/include/lux/engine/editor/sessions/SessionStore.hpp` | **只读核验** `TSessionAccess::read/describe` | 错误原样转交，不绕过 Store |

三份 `cancel()` 均含有：

```cpp
auto owner = access_.read(key_);
if (!owner)
{
    gesture_.reset(); // 注释把这里解释成 closed/reused Store slot
    return {};
}
```

三份 `synchronize()` 均先使用 `!info` 触发 cancel，再使用 `!info` 清空选择并成功返回。代码没有区分访问暂不可用与目标确实失效。

**错误的不是“关闭之后需要清理交互”，而是用“任何访问错误”证明“会话已关闭”。**

### 2.2 原 Store 已经表达了这个区别

`SessionStore::Impl::slot()` 在本次版本中的次序为：

```text
非 owner 线程                 → WRONG_THREAD
Store 正在回收某个槽           → BUSY
错误 Store 域                 → WRONG_STORE
没有匹配 slot/generation       → STALE_SESSION
```

`TSessionAccess::read()` 将该错误原样返回。`find()` 对非 published 或找不到的身份报告 STALE_SESSION，对不匹配类型报告 WRONG_TYPE。

`SessionStore::close(B)` 在检查许可后执行：

```cpp
impl_->reclaiming = true;
--impl_->published;
impl_->slots.erase(...B...);
impl_->reclaiming = false;
```

在 B 的源对象、历史或最后 code owner 析构过程中，对**仍然活着的 A** 使用公开 typed access，也会暂时得到 BUSY。这是 Store 的一致性保护，不是 A 消失的通知。

### 2.3 实际可达的同线程场景

A 与 B 是同一个 SessionStore 中两个已发布会话；A 的 interaction 和外部 owner 一直存活：

1. A 选择节点／对象，Begin + Preview，保留一个尚未提交的手势；记录 A 的源、History、observed、dirty、checkpoint 和选择。
2. B 正常取得 ClosePermit 并关闭。使用真实 B 的代码保活 owner 析构回调，或真实源／memento 析构回调，在 owner 线程请求 A 的 `synchronize()`。
3. Store 正处于 `reclaiming`；对 A 的 describe/read 返回 BUSY。
4. 当前 `synchronize()` 调用 `cancel()`；cancel 再次得到 BUSY，却将 A 的 gesture 清掉并成功返回。
5. synchronize 随后清空 A 的 selection 并成功返回。
6. B 回收结束，A 仍可正常 describe/read，但用户的预览和选择已经消失。

这个顺序不需要非法跨线程、不需要删除 A 的 interaction、不需要重新使用旧代际，也不要求回调擅自改 Store。正确反应完全可以是 BUSY 并保留 A 的原状态，下一安全点由调用者再同步。

**预期只证明交互丢失与错误返回，不宣称磁盘数据损坏、作者源已经被改坏或内存崩溃。** 对自定义预览载荷，还可能出现未取得 A 读取准入就执行清理的问题；不要把这种附加风险夸大为本次已运行的真实插件崩溃。

### 2.4 为什么现有测试不足

现有 Material 测试在同会话 withRead 中检查 begin 被 BUSY 拒绝，也检查目标真的关闭后 synchronize 清理。Scene 检查删除／重载后旧引用失效。这些都是正确测试。

缺少的组合是：**Store 暂时不可访问，但目标 A 仍有效。** 真正的目标关闭与暂时 BUSY 在当前代码中进入同一条分支，所以已有“关闭后清空”的正向测试反而不能证明这条错误分支安全。

## 3. 独立隔离结果（不是正式 SDK 复现）

`probes/source_identity.json` 固定原生产 CPP；`probes/include/` 全部是显式隔离替身，不可安装或并入生产目录。

| 场景 | 原 CPP 的 O0 / O2 结果 |
|---|---|
| 活目标访问被注入 BUSY，调用 cancel | 返回成功；overlay 消失；选择仍有一项；载荷释放；没有进入读取 scope |
| 活目标访问被注入 BUSY，调用 synchronize | 返回成功；overlay 与选择都消失；载荷释放；没有进入读取 scope |
| 正常活目标调用 cancel | 成功；overlay 清除；选择保留；清理发生在读取 scope 内 |
| 明确 STALE_SESSION 调用 synchronize | 成功清除失效手势与选择 |

BUSY 两场景按照“应拒绝且保留状态”的目标判定退出 1；正常／已失效对照退出 0。O0/O2 结果一致。两份对照说明应修改错误分类，而不是禁止所有 cancel 或所有失效清理。

请先运行 `probes/run.py` 理解其边界；正式 before 必须使用实际安装头、库、SessionStore 和三类模型，不能只重跑隔离探针就写 R1 PASS。

## 4. 规定的修正语义

### 4.1 不新增类型；准确使用现有错误

| 查询结果 | 允许动作 |
|---|---|
| 正常找到活会话 | 按原 Session gate 完成读取／清理／目标校验 |
| `BUSY` | 传回原错误；不清 gesture/selection，不改起始戳，不释放其载荷，不当成成功 |
| `WRONG_THREAD / WRONG_STORE / WRONG_TYPE` | 不视为关闭；保留并传回准确错误。在既有 owner 契约外不增加新的跨线程支持 |
| `STALE_SESSION` | 已确认该 typed 身份不再是活 published 目标，可以清除属于旧身份的交互；绝不切换到新 generation |
| info 成功但 ContentStamp/History 已改变 | 维持原严格冲突／失效同步策略；不把旧手势悄悄绑定到新内容 |

这里 STALE_SESSION 是依据当前 Store 实现选定的失效证明。后续发现其他明确终结码必须逐项论证，不能使用“除 BUSY 以外都清理”的默认分支。

无手势的 `cancel()` 仍可以是无副作用的幂等成功。Store 必须长于 access 和 interaction 的原前置条件不变；本轮不试图让引用访问已析构 Store。

### 4.2 cancel 的修改方向

在三个类型中，替换“read 任意失败即 reset”的分支。示意，不是完整可编译补丁：

```cpp
auto owner = access_.read(key_);
if (!owner)
{
    if (owner.error() != sessions::ESessionError::STALE_SESSION)
        return lux::cxx::unexpected(owner.error());
    // 只清属于确认失效身份的临时输入。
    discardConfirmedStaleGesture();
    return {};
}
// 保留原 read()/withRead() 路径；失败不清输入。
```

`discardConfirmedStaleGesture` 只是说明责任，不要求新增同名公开函数。可在现有函数中完成；若用私有 helper，应只有准确的清理职责，不再保存第二枚 busy/current/dirty。

清理扩展载荷时，优先将待清理批次移动到局部拥有对象、将 `gesture_` 置为无活动，再执行其最终析构；活会话路径的这个局部对象必须在原 withRead 回调内部析构。这样幂等清理与实际 payload 寿命清楚，不依赖 optional 实现何时翻转 engaged 标志。**这是清理拥有关系的局部整理，不是第二套准入机制。**

不能把 live 批次移到函数外作用域，导致 callback 返回、gate 退出后才析构。

### 4.3 synchronize 的修改方向

先分类 `describe()` 的失败，再决定是否有足够依据使状态失效：

1. BUSY 等非终结错误立即传回，原选择与手势保持。
2. STALE_SESSION 才进入原失效清理；按当前 synchronous owner 契约处理，不去寻找同位置的新会话。
3. 正常 info 后继续保留现有内容戳／历史换代处理。
4. 后续 read、withRead、运行来源查询中的准入失败，不应被当成“查无对象”；本轮先核对实际调用中是否返回可分类错误，不能为了统一改写所有 bool contains 或重新设计 RunStore。

必须同时改 `cancel` 与 `synchronize`。只修 cancel 会在“没有活动手势但仍有选择”的 BUSY 同步中继续丢选择；只修 synchronize 则直接调用 cancel 仍然错误。

### 4.4 明确禁止的修法

- 不取消 SessionStore 的 `reclaiming` 保护，不允许回收过程中重新取得可写 Store 引用。
- 不添加 `forceRead/peekIgnoringBusy`，不绕过 SessionState 或 typed key。
- 不把 BUSY 改名为 STALE_SESSION，不吞掉错误返回成功，不无限循环等待。
- 不给 interaction 再加一个独立会话有效性缓存或忙标志，避免再次双写真相。
- 不在原失败回调里“自动重新 begin”或重新读取新基线来重建已丢手势。
- 不修改既有 interaction 析构的 owner 契约，以便允许在任意 active callback 中直接删除对象。
- 不修改 Root／LuxObject／mount／detach、History、运行、保存或编译算法来补偿这项分类错误。

## 5. 逐文件实施与删除清单

| 路径 | 动作 | 本轮必须消失的旧逻辑 |
|---|---|---|
| SceneInteraction.cpp | 修改 cancel、synchronize；按需局部整理清理 owner | 所有把 `!read/!describe` 无条件当作会话已关闭的分支 |
| MaterialInteraction.cpp | 同上 | 同上 |
| FlowInteraction.cpp | 同上 | 同上 |
| 三个 interaction/include 头及 README | 有必要时补充 BUSY 原状态保留、STALE_SESSION 清理及重试边界 | 不能保留与行为冲突的“任意失败都同步完成”说明 |
| 三个 interaction/test/interaction.cpp | 在现有 target 增加真实回收回调／失效对照 | 原正常 cancel、提交、冲突、布局、关闭与重载断言保留 |
| `cmake/installed-consumers/interaction-views/interactions.cpp` 及原 CMake | 安装后使用新语义验证三个实际模型，必要时追加场景参数 | 不用源码私有头补齐 SDK；不删除原 detached UI 测试 |
| `.internal/editor-redesign/` | 更新唯一活动账本与覆盖 | 不另外建立 docs 下可变账本 |
| `dev_log/P08-R1/` | 冻结 before、实现收据、命令、日志、哈希与验证器 | 不覆盖原 dev_log/P08 或历史快照 |

**本轮不要求新增／删除任何生产文件、业务 target、包、DLL 或公共类型。** 旧错误分支与误导注释应直接替换；不移到 `.old`、另建 compat helper 或留 `#if 0`。已有 rooted ctor/UI 适配白名单及 P12 期限不变。

## 6. 实际回归要求

以下是五组回归，不要求分别创建五个 target。每项日志要标记模型、操作、原 Store 错误、交互结果、手势／选择是否保留及作者状态；不要只打印 PASS。

### R08-R1-01：另一会话回收期间同步不能使活目标失效

对 Scene、Material、Flow 的实际 A 分别执行。创建同一真实 SessionStore 中的真实 B（可统一使用 MaterialSession），在 B 的最后代码 owner 析构中同步 A。

- A 先有非空选择、非空有效 preview、固定起始 stamp。
- B 用真实 ClosePermit 关闭；回调首先用公开 typed access 证明 A 此时得到 BUSY。
- 回调调用 A.synchronize，不销毁 A/interaction、不通过私有后门写 Store。
- 目标是返回 BUSY，overlay／selection／起始 stamp／未清理 payload 均保持。
- 回收结束后 A 可正常读取；再次 synchronize 不丢仍然有效的手势；随后 commit 恰一条历史，证明不是只有返回码修好了。
- 修复前预期出现成功且丢手势／选择，保存真实记录；没有出现时追查夹具，不硬编码期望输出。

### R08-R1-02：直接 cancel 的 BUSY 路径

同一真实回收时机，直接调用 A.cancel，分别覆盖三类交互。

- 非空手势必须保留并报告 BUSY；不得执行预览 payload 的最终清理。
- B 回收完后，显式 cancel 成功；零作者历史变化、选择按原 cancel 语义保留。
- 同时验证“无手势 cancel”为幂等无副作用，不因这项测试被改成有伪失败或伪历史。

### R08-R1-03：没有活动手势的选择，同样不能因 BUSY 丢失

A 只有非空选择，没有 gesture。B 回收回调内 A.synchronize 仍应返回 BUSY并保留选择。这个场景专门防止只修 cancel 后误以为 synchronize 也安全。

回收结束后正常同步保持有效选择；真正删除对象后再同步才移除对应选择。

### R08-R1-04：真正失效、内容冲突与新代际隔离不能回退

- 正常 cancel 保持原 withRead 清理。
- 目标 A 真正关闭后，用仍存活的 Store 和旧 key 同步，明确 STALE_SESSION 清理；取消不得意外命中新会话。
- 原槽复用，新代际具有合法的同数值 NodeId／同 UUID；旧 interaction 不得绑定或清理新会话。
- 同一会话删除目标、History 换代／重载、其他编辑导致起始戳冲突，按原契约保持严格拒绝／同步清理。
- 原节点位置持久化、Undo/Redo、没有伪历史继续成立。

### R08-R1-05：原 gate、清理寿命和整体验收

- 有活手势时从实际模型 withRead 回调尝试 cancel/synchronize，检查 BUSY 不破坏输入；外层读取结束后正常操作可继续。
- 载荷清理回调只用于验证 owner/gate 和实际析构计数；不要宣称进行了真实 DLL 卸载测试。
- 原 139 项行为、P01～P07/R1/R2、X08-01～06、真实 Root 生命周期、八项 operation 编译负例继续运行。
- Editor/PLAYER all 构建及二次无工作；PLAYER 11 项保留；SDK 重装，原十二组消费者保留原 56 项及新场景；实际依赖负例仍使用原失败规则。
- 显式 `LUX_EDITOR_MIGRATION_STAGE=P08`；实现与验收均绑定实际最终 SHA。

## 7. 真正 SDK 复现的推荐夹具

附 `REAL_SDK_TEST_SKETCH.md` 给出以原 Material Fixture 为基础的接线。关键技巧不是修改 Store，而是把一个携带 callback 的普通 shared owner 作为 B 的 reservation CodeLease，使其最后引用在 B 槽回收时释放。

这种测试使用真实 Store、两个真实作者会话和原 typed access；它只是模拟代码／载荷 owner 的最后清理回调，不加载／卸载实际 DLL。C++ 回调和 A interaction 都保持在合法 owner 线程及存活期内。

before 先在 P08 生产提交上加入测试并取得实际失败。不要把本包隔离头复制进测试 target，也不要把 before 强制设为 WILL_FAIL 后称为功能通过。

## 8. 交付格式与停点

- 保护 `ProjectBuilder.cpp` 原修改；不能将资格检出干净等同于用户主工作区无改动。
- 原 P08 139/139、PLAYER 11/11、SDK 56/56 的日志保留为历史；新成绩重新绑定 R1 最终实现。
- 原 C01（P09/P12）、C03（P11）、C04（P12）保留既有判定及责任，不挂新编号延期。
- 不扩张到 P09 布局、P10 桌面 Host、全新冷构建、Android 或完整新产品 GPU 认证。
- before 生产 SHA／补丁／命令／退出码明确；负例内容在复现后冻结。after 原断言不能缩减。
- 实现先提交，`dev_log/P08-R1` 验收后提交，正常推送实施分支；不得 force-push。
- 输出 PASS/PARTIAL/BLOCKED，说明每个迁出／替换的分支、全部消费者、已保留原场景及测试边界。结束后停在 P08 等待复审，不自动进入 P09。

## 9. 可直接交给实施方

> P08 主体方向认可，暂不进入 P09。仅执行 P08 R1：三类 interaction 的 cancel/synchronize 不能把临时 BUSY 或其他访问错误解释为会话已失效。
>
> 先用真实 SDK，在同一 SessionStore 正常回收 B 的代码／源 owner 回调中访问仍活着的 A，保留原实现错误清空预览／选择的失败记录。覆盖 Scene、Material、Flow，并检查没有手势但有选择的分支。
>
> 仅明确的 STALE_SESSION 进入旧身份清理。BUSY 等错误原样返回，原手势、选择、起始戳、载荷与作者状态保持。活目标清理继续经过原 Session gate；真正关闭／重载／代际复用的清理仍成立。不绕过 Store，不增加平行 busy/Session/History 或失效缓存，不改 UI/运行/保存/执行器算法。
>
> 修改限于三个 interaction 的错误分支、必要契约及实际回归。保持目录、target、包名和原适配白名单。执行五组 R08-R1、原 139 项、PLAYER、十二组 SDK、八项 operation 编译负例和原依赖/真实 UI 回归，显式 P08。
>
> 保护 ProjectBuilder.cpp、原快照和 C01/C03/C04；实现与独立验收分别提交并推送，停在 P08，不进入 P09。

## 10. 固定源码与报告来源

以下均固定本次验收 HEAD。网页行号可能随展示改变，以文件和符号定位，不把工具 JSON 的包装行号当成 C++ 源码行号。

- [S1 P08 验收](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/dev_log/P08/README.md)
- [S2 SceneInteraction.cpp](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/tools/scene/interaction/src/SceneInteraction.cpp)
- [S3 MaterialInteraction.cpp](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/tools/material/interaction/src/MaterialInteraction.cpp)
- [S4 FlowInteraction.cpp](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/tools/flowforge/interaction/src/FlowInteraction.cpp)
- [S5 SessionStore.cpp](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/editing/sessions/src/SessionStore.cpp)
- [S6 SessionStore.hpp](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/editing/sessions/include/lux/engine/editor/sessions/SessionStore.hpp)
- [S7 原 Material interaction 测试](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/tools/material/interaction/test/interaction.cpp)
- [S8 原 Scene interaction 测试](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/tools/scene/interaction/test/interaction.cpp)
- [S9 Root.cpp](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/modules/function/ui/src/Root.cpp)
- [S10 DetachedView](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/views/api/include/lux/engine/editor/views/IViewHost.hpp)
- [S11 UI lifecycle 测试](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/editor/views/api/test/lifecycle.cpp)
- [S12 原 CTest 汇总](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/dev_log/P08/logs/ctest.log)
- [S13 原验收检查器](https://github.com/LUX-YU/lux-engine/blob/8f210a282109ec9e0dae3f32f88bc04359ef2c65/dev_log/P08/check_receipt.py)

规范基础：原 `P08_interaction_and_detached_ui.md` 与 `LUX_ENGINE_P08_START_AFTER_P07_R1_2026-09-30.md`。本轮要求是把“确定失效才清理”的语义落实到既有错误类型，并非新增另一种交互模式。
