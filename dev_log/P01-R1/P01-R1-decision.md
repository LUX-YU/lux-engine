# P01 R1 定向补正决议

规范基线仍是本地不可变 V4 原件及 P00/P01 的唯一施工账本。R1 来自用户明确授权的
《P01 复审与定向补正 R1》；输入提交 `a653a89bc3cfae25fbf2e0777b2b757b93583ba5`，
实现提交 `c3652237642085bf309620ab2b5229a6d90a0508`。不进入 P02。

## 类型索引补正

`IEditSession` 新增私有 `currentContent() const noexcept -> ContentStamp`，仅 `SessionStore`
可以调用。实现从已有会话身份与 History 当前状态读取标量；不分配、不复制来源字符串、不做 IO、
不通知、不修改准入、不发布操作，不新增 current 缓存。P02 以后真实 Session 直接实现此契约。

`describe()` 保留可抛异常的拥有型展示结果。Store 在既有 owner、代次、许可归属检查之后，
于 CallbackScope 内读取内容戳，匹配后才消费许可并回收槽位。完整描述不再出现在关闭提交路径。
原 `SessionState`、`PersistenceCheckpoint`、`EditGate`、History 及对象先于代码析构的规则均不变。

## 基础层门禁

沿用三目标已有 dependencies，并实际执行直接及传递检查。外部依赖精确限定到 compile_time、
container、identity 及其 stduuid 依赖；核对 imported 属性或来源目录，不以名字相同自动放行。
无法解析的链接目标报错。实际源码 include 也检查，不以 PRIVATE 或目标名不含 model 绕过。

CMake 导出沿用 alias、LINK_ONLY、配置条件及生成器依赖处理，补齐可达 imported 接口。
stduuid 原来在 identity 的兄弟目录作用域导入；检查作用域通过已经选定的 stduuid_DIR 读取
同一个包的接口，不伪造空叶子，也不重新搜索其它安装版本。

## 证据路径

同一个 `dev_log/P01/verify.py` 读取归档索引及 archive_log，不读取生产机器的绝对路径。
旧 AST 原始字节按原 SHA 归档；原 proof、原收据、原执行结果保持不变。缺失/篡改明确失败。
R1 收据核验只检查本次运行与不变量，并调用上述原验证器；不复制 AST 或历史算法验证。

唯一可变记录继续在 `.internal/editor-redesign`。`dev_log/P01-R1` 是补正验收冻结快照，
不成为另一份可继续修改的账本。桥仍仅供旧产品，最迟 P12 删除；C01/C03/C04 仍失败并保持原责任。
