# P10Q 只读盘点工具

`inventory.py` 使用 Python 3.10+ 和 Git。它读取指定工作树的 tracked 文件及当前差异，可选读取已有 CMake File API reply；不会拉取依赖、检出分支、修改源码、配置或构建项目。

## 使用

下面的路径是示意，替换为实际包位置、仓库根和证据输出路径：

```sh
python path/to/package/tools/inventory.py --repo path/to/lux-engine --output path/to/evidence/inventory.json --base b583e7ffe20e7a1ac55c7119d6a13ac337ebb323
```

默认扫描 `editor`、`engine/process`、`modules/core/object`。可用重复 `--scope` 改选相对仓库路径。输出包含 HEAD、工作区状态、文件哈希/行数、启发式候选以及实际范围说明，不把当前工作区自动视为干净 HEAD。未跟踪文件仅在 Git 状态中出现，不扫描其内容。

已有配置生成了 codemodel reply 时，可加：

```sh
python path/to/package/tools/inventory.py --repo path/to/lux-engine --output path/to/evidence/inventory-with-targets.json --cmake-reply path/to/build/.cmake/api/v1/reply
```

工具不会自行生成 File API 请求；实施环境须沿用项目已有 CMake 查询/配置流程。读取的 TYPE 是相应配置元数据，不代表程序已经加载这些库，不能代替实际链接和运行时装载分析。

## 返回与限制

返回 0 表示盘点完成且匹配范围内文件都可扫描；返回 1 表示报告已写出，但有缺失、符号链接或非 UTF-8 等未扫描项；返回 2 表示输入/Git/JSON/IO 错误。输出文件默认不得已存在，`--force` 仅允许重写显式输出文件，仍禁止覆盖 tracked 源码或历史材料。

关键词会命中注释和字符串，短文件只是候选。不要把该脚本注册成“通过即架构正确”的自动门禁；真正门禁须使用编译图、AST/调用关系、行为测试和人工所有权审阅。

## 已执行的自检

```sh
python path/to/package/tools/inventory.py --self-test
```

随包 `SELFTEST.json` 只来自临时合成 Git/JSON 夹具，检查 Unicode 路径、分类、候选定位、CMake TYPE、固定基线、内容不修改和缺失文件处理。**没有对 lux-engine 检出运行，没有引擎测试成绩。** 自检的临时仓库自动销毁。
