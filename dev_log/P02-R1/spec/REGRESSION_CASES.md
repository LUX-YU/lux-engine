# P02 R1 回归场景与测试插入示例

这些是待实施的测试要求。下面片段复用固定 P02 源码 `editor/tools/scene/model/test/scene_session.cpp` 内的 Fixture、take、object 等辅助对象，**不是独立可编译程序，也没有在本轮完整引擎环境执行**。将它们接入真实 CPU Scene 模型测试，增加 CTest 场景，不用简化的 Python 模型替代产品断言。

## R02-01：字段 → 删除组件 → 重加组件

将以下函数放入原测试文件的匿名命名空间中，并在 main 增加对应场景分支。使用现有 CMake 模式注册新测试名。

```cpp
void mixedComponentReplacement()
{
    Fixture f;
    f.create("mixed-component");
    const auto id = object("mixed-component");
    const auto schema = ecs::componentSchemaId("lux.ecs.Transform3D");
    const auto before = take(take(f.session->read()).component(f.ref(id), schema));
    const auto before_state = f.session->describe();

    ecs::Transform3D replacement;
    replacement.translation = Eigen::Vector3d{8, 9, 10};
    replacement.scale = Eigen::Vector3d{2, 3, 4};
    ecs::WorldEntityMap identities;
    const auto expected = take(encodeSceneValue(replacement, f.metadata, identities, 4096));

    auto batch = f.batch("field then component replacement");
    batch.edits.push_back(f.translation(id, 1));
    batch.edits.push_back(SceneRemoveComponent{{f.ref(id), schema, {}}});
    batch.edits.push_back(SceneAddComponent{f.ref(id), expected});
    const auto receipt = take(f.session->apply(std::move(batch)));
    assert(receipt.effect == editing::EEditEffect::CHANGE);
    assert(take(take(f.session->read()).component(f.ref(id), schema)) == expected);

    // 一个批次对应一次撤销，不能停在临时字段状态或“已删除组件”的中间状态。
    assert(f.session->undo());
    assert(f.session->describe().current == before_state.current);
    assert(take(take(f.session->read()).component(f.ref(id), schema)) == before);
    assert(f.session->redo());
    assert(take(take(f.session->read()).component(f.ref(id), schema)) == expected);
}
```

固定源码中 `Transform3D` 确有 translation/rotation/scale，默认 scale 为 Ones。预期比较完整 SceneComponentData，不只是 translation；这样可以发现未被字段修改的 scale 也被旧 scratch 回写丢失。

**修复前预期：** apply 可能成功，但完整组件与 expected 不同。记录实际差异，不把本文预期当成运行日志。如果结果不同，保留实际反证并追踪代码版本。

## R02-02：字段 → 删除组件

初始对象存在 Transform3D。一个 batch 先修改 translation，再 RemoveComponent。按有序列表语义，成功后组件不存在，Undo 一次回到原组件。

当前代码末尾试图向已移除的组件写回字段缓存，会返回失败；修改后不能只解决“重加”的路径而漏掉“最终不存在”的情况。

## R02-03：字段 → 删除 → 添加新组件 → 修改另一个字段

沿 R02-01，但在 AddComponent 后追加 scale 字段修改。

```cpp
batch.edits.push_back(SceneSetField::make<ecs::Transform3D>(
    {f.ref(id), schema, "scale"}, Eigen::Vector3d{5, 6, 7}));
```

预期：translation 保留新组件的 `(8,9,10)`，scale 为 `(5,6,7)`；旧组件的其它值不能混回来。仍只产生一次历史提交。再补重复字段更新用例，确认同一存在周期内最后一次字段值生效。

## R02-04：非法组件时序不产生半提交

一个 batch：先进行其他对象的有效字段编辑，再删除目标组件，最后对未重建的组件设置字段。这个 batch 应整体失败。

比较所有源对象编码、配置、History 当前状态、cursor、ObservationVersion 和 dirty。不能只检查目标组件仍在而漏掉其他对象的半次修改。

同时运行原 pluginSnapshot：普通局部字段编辑不允许解码无关插件组件。修补后不应退回“所有字段编辑都完整克隆整个场景”。

## R02-05：快照编码回调重入修改另一对象

复用 `model/test/plugin_snapshot.cpp` 的真实 ComponentSchema 与 ComponentCapture。测试中增加一次性编码钩子，保持插件组件和普通 Transform3D 属于不同对象。

实施顺序：

1. 建立真实 SceneSession 并添加插件对象 P 和 Transform3D 对象 Q；两者创建并提交后才启用钩子，避免源准备期间触发。
2. 先捕获一个无钩子的基准快照，记录 S0 内容戳、变化游标和 Q 的编码值。
3. 插件 encoder 进入时，取出并清空一次性钩子，再从本地拷贝/移动出的回调调用公开 session.apply，尝试修改 Q。不要在回调仍执行时销毁其自身闭包。
4. 外层调用 session.capture。
5. 记录 nested apply 是否被拒、错误原因、外层快照内容戳、当前会话内容戳、Q 的完整字段值和两个游标。

**修正后必须满足：**

- nested apply 被准入拒绝，错误语义为 BUSY/读取占用，不发生隐式排队；遵守实际 SceneEditError 包装，不能只接受任何无关错误。
- 外层快照成功且与 S0 一致，snapshot.content 与其对象编码来自同一提交状态。
- 读取本身不增加历史或观察版本，不改变 dirty。
- 钩子失效后正常 apply 能成功，证明 gate 已恢复。

**当前代码的条件性预期：** 没有占住读取 gate，nested apply 可以成功；外层使用 S0 参数继续返回包含 S1 字段的快照。测试必须保留实际观察，不得人为绕过 gate 或直接写私有 Registry 来“复现”这一问题。

## R02-06：codec 失败/异常后读取准入恢复

两种情况分别测试：

- codec 返回合法错误值；外层 capture 返回失败，作者源、历史、dirty 不变，后续正常编辑可用。
- codec 抛出普通测试异常；依照现有非 noexcept 边界的约定捕获或传播，作用域仍恢复，不以进程终止代替清理验证。

钩子与测试异常必须只存在于测试 schema，不给产品公开 API 增加“注入失败模式”。

## R02-07：读取期间关闭和重载采用

编码钩子中尝试 `SessionStore::prepareClose(current)`，应被同一 gate 拒绝。若关闭许可在外层 capture 前已获得，则 capture 应维持原来的拒绝语义。

私有测试的 PreparedSceneReload 可以事先准备；在读取回调内尝试 adopt 应拒绝、候选未消费、原会话未替换。外层退出后，在内容戳仍匹配时可正常采用。

不要通过公开 replaceSource/markClean 测试后门实现这些用例。P01-R1 的私有 currentContent 和关闭 permit 校验原样保留。

## R02-08：拥有型组件读取同样保护

通过 SceneReadView::component 触发相同插件 encoder，在其中重入对另一个对象的修改。该方法也必须在其完整 codec 调用期间保护源一致性。

objects/contains/parent 等不调用插件、只在同步作用域使用的借用，继续原有失效规则；不要将整个 View 变成长寿命共享锁或会话 owner。

## S01：目录收拢回归

- 同一 `edit_history`、`edit_sessions` target 和命名空间；公开 include 不改；source 文件只编译一次。
- 把 foundation 和 model 的既有禁止边逐条重新运行，包括经过 imported target、alias 和 LINK_ONLY 的传递边。
- 架构检查器明确识别 `editor/editing/history/`、`editor/editing/sessions/`，但不误把整个旧 editing 目录加入纯基础范围。
- 重新安装 SDK，从六组消费者重新配置、构建、运行。旧 `editor/history`、`editor/sessions` 不得因安装脚本或测试被自动复制回来。
- 验证 P00/P01/P01-R1/P02 旧收据时，源码存在性应查询当时的 implementation_sha；不能用“当前路径不存在”误判过去的历史事实，也不能全局跳过存在性和哈希检查。
- 本轮最终源码与证据新建快照，旧成绩、SHA 和日志不改写。
