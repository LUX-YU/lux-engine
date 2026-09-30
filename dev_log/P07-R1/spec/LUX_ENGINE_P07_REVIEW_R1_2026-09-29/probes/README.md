# 声明级探针——不是 Lux SDK 运行

`copyability.cpp` 保留了审阅版本两种 operation 的完整类声明；周边领域类型以最小声明提供。它只运行 C++ 类型特征，不调用这些类的析构或 TaskRuntime。

两编译器都输出 copy/assignment 为 1。move traits 为 1 可能来自右值退化复制，不能声称实际移动安全。

用 `-DREQUIRE_UNIQUE_OWNER -fsyntax-only` 编译时，八个目标契约断言均不成立，编译器非零退出。这个失败是预期的当前缺口，不是缺少依赖导致。

可复跑：

```sh
c++ -std=c++20 copyability.cpp -o copyability
./copyability
c++ -std=c++20 -DREQUIRE_UNIQUE_OWNER -fsyntax-only copyability.cpp
```

`results.json` 记录本次实际编译器、命令、退出码和文本日志哈希。`../tests/owner_contract.cpp` 是真实 SDK 用例，未在本环境编译。`../tests/before_runtime_sketch.md` 也是待实施的真实测试草图，不是运行证据。
