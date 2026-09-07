# SR-2 修正门槛证据

修正提交：`8145598c18421d03da1dac21251200af3290e27d`，修前生产代码：
`1a7fbd60f47a115b7b01528075550b77c1a3c194`。

- [原始日志和源码副本](SR2-correction-evidence.zip)
- [逐文件哈希](files.json)、[归档 SHA-256](SHA256SUMS.txt)

单配置与混合批次修前分别全量构建，均在 `reclaimed` 断言失败；两份 fixture 的 main
首项不同，归档中保留各自源码和日志。修后 lifecycle 包含这两个场景和五种准备失败点，
CTest 通过。85 项受影响测试在 VS 环境下重跑，79 项通过，六项历史 Scene Lua 用例仍在
`activeContinuationCount() == 0U` 断言失败（line 489，`0xc0000409`）。

本包是工作树迭代证据：确实重新全量构建，不作为独立 clean clone qualification。
首次补充 fixture 的枚举拼写编译失败，以及未加载 VS 环境运行负向编译测试的失败均保留；
有效结果分别是 `green-build-2`、`green-lifecycle`、`green-affected-vs`，没有删除失败日志。
后续修正 SR-2 参照与 SR-3 候选的独立构建、安装和测量将另包交付。
