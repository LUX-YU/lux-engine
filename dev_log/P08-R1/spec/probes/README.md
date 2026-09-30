# 隔离探针说明

`MaterialInteraction.cpp` 是当前 P08 生产文件的精确副本，已核对 Git Blob SHA-1；`include/` 中的头则完全是隔离替身。`main.cpp` 通过替身返回 BUSY，观察原生产函数如何处理错误。

执行：

```sh
python run.py
```

要求 g++ 支持 C++23 的 `std::expected`。这仅是替身选择，不要求引擎升级标准。脚本构建 O0/O2，分别执行 busy-cancel、busy-sync、live-cancel、stale-sync。原源码的前两个场景按修复目标判定退出 1；两个对照退出 0。

`results.json` 是本次实际运行日志索引；`source_identity.json` 固定源码身份。script 的总退出 0 表示观察符合“原实现会违反 BUSY 保持契约”这一复现预期，不等于生产功能通过。不能把这个隔离测试装进源码工程替代真实 SDK 测试。

局限：未编译真实 SDK/SessionStore/MaterialSession/CodeLease/模型 codec，不执行真正的 B 回收，未加载 DLL，未运行 UI/GPU。真实 B 回收可返回 BUSY 的依据来自本轮读取的 SessionStore::close 与 Impl::slot，而不是隔离头本身。正式负例必须按 REAL_SDK_TEST_SKETCH.md 的方向取得实际记录。

包内不提供生成的二进制，运行脚本会在本目录重新生成。
