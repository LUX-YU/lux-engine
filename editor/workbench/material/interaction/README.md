# material interaction（P08）

该 STATIC target 只保存选择和一份临时领域批次，不拥有 Session、History、Registry 或保存基线。
SessionStore 必须活得更久；Scene 的 RunInspectAccess 是值形式的非拥有能力，其 RunStore 也必须活得更久。
构造、读取和交互在同一 owner 线程。活动交互 owner 不可复制或移动。

- `begin` 捕获既有 ContentStamp；重复开始或领域 gate 占用时拒绝。
- `preview(candidate)` 仅在原 gate 的 READING 范围内交换临时覆盖并清理旧 payload；拒绝时输入仍归调用方。
- `overlay()` 是同步只读观察，后续交互会使它失效，不得保存可写领域借用。
- `commit` 严格验证起始戳，并只调用一次原 Session::apply；冲突保留手势，交给取消或同步处理。
- `cancel` 在同一读取 gate 内清理覆盖；忙时不强行解除 gate。必须先在安全点结束交互，再析构 owner。
- `synchronize` 在作者删除、重载、关闭和运行停止后清理失效身份；不自动重定基线。
- `cancel/synchronize` 只有收到明确的 `STALE_SESSION` 才能走旧身份清理。Store 正在回收其他会话时的
  `BUSY` 及其他访问错误原样返回，保留选择、手势、起始戳和输入所有权；不会把临时拒绝解释为目标关闭。
- 活目标的取消仍在原 `withRead` gate 内执行：先移出批次并解除活动手势，再在该作用域销毁输入。
  真正旧身份不借用新代际会话；没有手势的显式取消保持幂等。

节点布局仍通过原领域编辑保存；相机、hover、画布 pan/zoom 不进入共享选择组。
正式视图消费这些交互能力；旧产品的即时 UI guard 和输入转换桥已删除。
