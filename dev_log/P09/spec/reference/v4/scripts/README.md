# 只读迁移审计工具

本工具检查随包 `manifests/audit-rules.json` 中列出的过期旧文件、旧 C++ 类型定义/别名、限定旧 API 定义、新模块对旧框架的直接 include，以及未登记的过渡文件。不会修改、移动或删除源码，不会提交 Git。

在包外的真实仓库运行（把路径改为实际位置）：

```sh
python scripts/audit_migration.py --repo /path/to/lux-engine --stage P12 --output /path/to/evidence/P12-static-audit-01.json
```

P00–P13 均支持。`--output` 可省略；提供时只允许新的 JSON 文件，不覆盖旧证据。返回值 0 只表示“未触发所列静态规则”，1 表示发现需要解决的项，2 表示工具或输入错误，绝不是通过。

每阶段更新过渡白名单时使用 `--manifest /path/to/actual-audit-rules.json`。必须是精确条目及删除期限，不许整片忽略 old/compat/tests。持久兼容数据文件不是过渡 C++ owner，不能因其包含旧名字符串就删除。

局限：没有解析完整 C++ AST，宏/typedef/重命名后的 God object 仍可能漏检，注释可能误报；不检查真实生成器/链接图、成员所有权和运行行为。使用 tracked 与 non-ignored untracked 文件，过期的精确路径另行直接检查；清理未知 ignored 代码仍由 P00 账本/人工核对负责。遇到 symlink 需人工盘点而非跟随到项目外。

辅助脚本自测：

```sh
python scripts/test_audit_migration.py
```

随包 SELF_TEST_RESULT.txt 是 **该工具自身** 的 9 项测试记录，不是引擎的任何测试、构建或性能成绩。自测只在临时 Git 仓库建立小文件，不访问或修改用户项目。
