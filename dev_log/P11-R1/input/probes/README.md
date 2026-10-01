# 探针范围

`control_flow_witness.cpp` 是缩小的 C++20 控制流模型，不是完整生产代码，也未链接 Lux SDK。

- 保留了 prepareSource 的输入先声明、guard 后声明次序。
- 保留了 ContributionRegistry active 与 CommandRegistry calling/dispatching 彼此独立的关键关系。
- fixed 分支只是说明性对照，不是仓库补丁。
- 结果不计入 Windows、Linux 引擎、实际 DLL、GPU 或 SDK 资格。
- 完整命令和编译器版本在 results.json 中；二进制不随包分发。

重跑示例：
```sh
c++ -std=c++20 -O2 -Wall -Wextra -pedantic-errors control_flow_witness.cpp -o witness
./witness cleanup current-shape # 目标断言不成立，退出 1
./witness cleanup fixed         # 说明性对照，退出 0
./witness batch current-shape
./witness batch fixed
./witness failed_candidate current-shape
./witness failed_candidate fixed
```
