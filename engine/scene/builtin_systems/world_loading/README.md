# 显式分区世界加载

`WorldLoadingSystem` 是可选 SceneSystem。它把当前 WorldDescription 中的分区内容物化为运行时 Entity，不依赖 Camera、RenderSystem 或 Editor。SceneRuntime 内部实例拥有 Registry、Simulation 和系统，加载维护由其 SceneDriver 调用。尚未迁移的 Editor 宿主暂沿用原实例入口。

## 需求与身份

`Observer` 是普通组件，持有显式分区集合和必要性标志。修改组件后必须使用 Registry 更新通知。分区序号只在所属 WorldDescription 中有意义，不是跨版本的持久身份，也不是 Entity。组件通过正式 schema 保存，运行时请求、驻留表和资源句柄不编码。

宿主可以提供 bootstrap 集合，避免唯一 Observer 本身必须先被加载才能发出需求。多个 Observer 的集合取并集；任一必要需求使该分区成为必要项。静止需求不重复读取，仍被需要且源身份未变的在途结果不因其他 Observer 的变化而失效。

请求集合必须包含所需依赖分区。系统不会遍历磁盘猜测引用指向哪个分区，也没有新增对象到分区的持久索引。读取结果带源身份，过时结果不安装。

## 准备与物化

读取复用 Process 的 WorldPartitionLoadSender。整个必要集合完成读取后，WorldMaterializer 联合已驻留身份解析引用，先规划 Entity、解码全部组件并验证引用，再安装。正常格式、版本、容量和引用错误发生在 Registry 可观察变化之前。

安装阶段要求组件移动和已登记构造观察者不再拒绝业务输入，不在回调中结构性修改同一 Registry。这是明确的提交契约，不是通用原子事务；不能把任意外部回调副作用称为可自动回滚。

采用只发生在 Driver 允许结构变化的稳定边界。一个已开始的步骤等待发布时，加载器可以接收异步完成，但不改变该步骤正在借用的 Registry。必要集合尚未完整时阻止接纳下一步；维护返回主循环，不在 Main 阻塞等待 IO。

## 数据与加载策略的边界

`WorldResidency` 在同一模块中，由 Registry context 拥有唯一的身份映射、分区来源、成员、脏状态和保护记录。
`WorldLoadingSystem` 只维护需求、IO、观察连接和采用/卸载安排；业务读取 Registry 中的 WorldResidency，不借用加载系统。
分区方案筛选及读取依旧属于加载策略。WorldResidency 不自行决定需要哪个分区，不读取文件，也不依赖 Editor。

`WorldLoadingServices.initial_partitions` 可提供固定的已解码内容；安装阶段一次采用，成功后实例才 seal/公开。
普通异步读取与构造期采用复用 WorldMaterializer。未知 payload 保留在原分区源中。

结构编辑先 `prepareCreate`/`prepareErase`，再 `PreparedChange::commit`。准备失败或放弃不改变 Registry；
准备和提交之间由调用方持续保持结构独占，不能跨维护、异步等待、分区卸载或 owner 析构保存此对象。
提交不会返回可恢复业务失败；OOM 按工程约定终止。身份、实体、成员和脏状态统一提交，不开放可写身份表。
删除原始对象时，未知引用保持保守拒绝；新建对象撤销仍可执行。已知引用同时检查临时与持久对象。

## 驻留和卸载

卸载要求需求、脏内容、外部保护和活动跨分区引用全部解除。`PartitionRetention` 表达真实使用权，可以由编辑历史条目持有；Editing 本身不依赖世界加载模块。未知 payload 或无法遍历的组件引用采用保守保留，并返回保留理由。

已知组件引用通过已有 typed codec 走只读计数 archive：不构造序列化字节缓冲，也不复制组件。运行期临时反向表仅覆盖当前驻留 Entity，使用完整代次。没有需求的引用环可一起卸载，仍被保留分区引用的目标继续驻留。

## 异步寿命和预算

宿主提供寿命覆盖系统的根 TaskScope。系统析构断开组件观察并请求取消；operation state 独立保留源、输入与完成值，不捕获系统或 Registry。晚到结果释放自己的内容，根 TaskScope 由宿主最终关闭。内置代码所在模块必须覆盖最后一个 operation 的完成与析构。

限制分别覆盖在途数、读取字节、暂存、驻留分区、实体和组件计费（无每轮次数额度）。加载器统计 IO；WorldResidency 统计驻留内容并提供保留理由。组件计费是 inline 值大小，不包含 provider 容器的所有深层分配；读取暂存计费也不能当作解压过程的总峰值内存证明。

本模块目前提供显式集合策略，不解释空间范围、LOD、相机跟随或运行热替换政策。
