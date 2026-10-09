# Render Kernel 登记合同

FrameExtensionRegistry 是唯一扩展 slot authority。slot 0 无效，32 个有效 slot 对应帧数组的 1–32。
空名/非法名称拒绝；同名返回原 slot，即使容量已满。Kernel 不再维护第二套扩展映射。

KernelDeclaration 是不可变的名称与函数声明，KernelRegistration 另携带明确的 code_lifetime。
KernelRegistry 在 Render 装配阶段采用声明，保存稳定地址的 RegisteredKernel、拥有型扩展名称、
已解析 slot 和代码 pin。相同函数与扩展的重复登记幂等；不同定义冲突，不替换既有记录。
批量登记若后项失败，已成功的前缀仍登记并保活；这不是可回滚插件事务。

接受后的代码至少保活到进程 Registry 结束；无 unregister、热替换或卸载协议。
空 code_lifetime 只表示代码链接寿命覆盖整个进程。宿主必须在 Registry 结束前结清图、执行与 GPU owner。
不同 RenderRuntime 的冷登记在原 Registry 内串行化。有效 KernelId 范围对应固定 owner 数组；
成功前缀以 release/acquire 发布，只追加、从不替换。数值查询不加锁、不复制 shared_ptr，
已有图可以继续读取稳定条目；forEach 固定本次已发布前缀，回调不在锁内执行。
名称解析仍是冷路径。每个注册项有一个稳定 allocation，没有第二套发号或退休目录。

内置 Mesh、Shadow、Utility 源文件提供唯一声明表，经 builtinKernelDeclarations 强引用汇总。
生成的 FeatureFactory 只引用表，不在静态初始化期间登记。现有服务端 Feature 准入消费该表并传入
同一注册请求的代码 pin，随后才能创建/使用该 Feature 的图。Render 核心不反向链接内置 Feature 实现。
不经过 Feature 准入的底层使用者，必须在图使用前显式 registerKernels。

RenderFeatureRegistration/FeatureFactory 的二进制布局变化对应 Render exports v2，旧 v1 明确拒绝；
内置导出、实际加载端和外部样例同步重编。游戏、脚本与 Scene 的独立导出版本没有因此升级。
KernelDescriptor 现在是 render_vulkan 的正式公共头；原 sinclude 定义删除，无转发头。
