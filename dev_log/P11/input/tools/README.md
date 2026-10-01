# 只读残留盘点工具

`inventory_remaining.py` 只读取指定 Git commit 的对象。它不读取用户改动作为资格源码，不删除文件，不改 index/branch，不 fetch/push，也不访问安装目录。

```text
python tools/inventory_remaining.py --repo <仓库路径> --ref <明确提交> --output <仓库外的新报告路径>
```

输出包含旧根中的 tracked 文件、最终五层之外的一级目录、旧符号的候选匹配。匹配可能是生产代码、负例或架构规则；必须人工分类，不能把字符串出现直接当作违规，也不能把零匹配直接当作整个阶段通过。

退出 0 只表示盘点成功。输出路径不得在仓库内且不得已存在；审核后由实施者将需要的结果纳入唯一施工账本。指定 ref 与用户当前检出可能不同，报告会写实际 commit。

工具不检测所有同义改名、运行时回落、C++模板隐含依赖、Root生命周期或插件 ABI；这些由实际构建与行为验收处理。

`test_inventory_remaining.py` 只在临时合成 Git 仓库中测试报告和保护行为，不是 lux-engine 测试。
