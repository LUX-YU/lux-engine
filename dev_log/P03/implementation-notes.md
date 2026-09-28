# P03 施工决定与交接

起点 e0f440067792dc661e99a4f861a4116eeab4d304，分支 codex/editor-redesign-v4，初始工作区干净。
本轮只 P03；未修改 main，不授权 P04。启动包 SHA256：1e318b24a611c18680f78440e8b2ef81b50e460542394d8bd52d6916da313d52。
包中 reference/P03_material_authoring.md 与既有 spec-v4/phases 原件逐字节相同。

唯一可变账本仍在本目录；本文件解释决定，不另建成员账本。阶段末冻结 dev_log/P03。

MaterialSession 由 SessionStore 独占，复用唯一 SessionState 和 History。private currentContent 不调用 describe。
read 提供同步 withRead，capture/encode/重载准备均在原 gate 中。无第二份 current/dirty/busy。
纯编辑算法已从 MaterialEditor::Impl 删除。旧侧仅组装领域意图，保留项目目录检查、旧 UI/预览/编译/IO 接线，详见迁移账本的 p03_disposition。
旧产品仍有自己的源和历史，未安装另一份同步 MaterialSession；新模型不反向依赖旧 owner。

普通值批次只暂存指定字段；结构批次临时深复制图，顺序解释，然后历史仅保留受影响节点/连接/位置差异。
这是明确的结构候选成本，不宣称零拷贝或 RSS 限额。槽引用按照最终图验证，允许同一批次同步修订节点和声明。
已有内存图及 codec 支持的类型/环/中间图规则保留，不扩充磁盘格式。未知节点种类原 codec 本来拒绝；没有跳过未知内容后保存成功路径。

现有 Node::clone 是虚函数，但所有内置节点 final 阻断了动态 clone/destructor 的寿命验证和合法特化。
去掉内置节点 final，仅允许保持原 kind/字段 schema 的派生实现；Node 的 ConstructionKey 仍封闭，未引入任意新种类或插件注册框架。
快照/逆操作/源外部持有 CodeLease，move assignment 保证节点析构返回后才释放代码。
同 owner lease 去重；源保守保活已引入模块至重载/关闭，快照独立保活。modules 公共头已同步三个安装前缀。

初次全量测试发现：新错误映射丢失原 GraphTopologyError::INVALID_TYPE，已保留原断言并修正映射；日志 P03-dev-ctest.log 保留。
同次 sessions_scope_compile 缺 MSVC 标准库环境，最终 runner 在 VS DevShell 下执行，不改测试。
C01/C03/C04 持续保留 FAIL，分别 P09/P12、P11、P12，不修正、不改判定。
