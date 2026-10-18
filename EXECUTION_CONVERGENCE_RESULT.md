# Execution 收敛：实施与验证结果

本轮仍在施工。精确起点：`6c9e95fc247a82e90430e6b503060896e735516f`。最终独立 Verification 尚未开始；不得把下面的阶段施工检查当作最终 V。

## 阶段 A

已删除 WorkSender/CpuSender/CpuSchedule 及绑定工厂；CreateEffect 和 Native CpuPreparation 直接使用宿主 CpuScheduler。保留非拥有 CompletionPost 与原 Native 回收时序。测试拒绝改为真实 Scheduler 队列饱和；准备暂停复用现有 Native 编译期 instrumentation，生产发布路径无额外分支。

施工 Release 全量 all 编译、第二轮 no-work 通过。首次 146 项测试中 130 项通过、16 个 Native 可执行程序因缺 Core Execution DLL 以 `0xc0000135` 启动失败。修正测试 CMake 的实际 DLL closure 部署后，32/32 Native 测试通过，包含此前 16 个失败项目。没有通过 PATH 或旧 SDK 补 DLL。

首次编译失败还包括旧公共头清单残留和测试命名空间迁移遗漏，均修复；原始失败日志保留。ASan all 编译、第二轮 no-work 和全量 146/146 测试通过（211.54 秒），未发现 ASan 诊断。

所有日志：`E:/RenderV2Rebuild/Execution/evidence/a/`、`evidence/a-asan/`。最终提交将另提供可携带索引与 SHA-256，不仅引用本机路径。

实际命令入口为外部 `E:/RenderV2Rebuild/V12/run-material.ps1`，本轮 Source=`E:/RenderV2Rebuild/Execution/source`，Build=`E:/RenderV2Rebuild/Execution/build/a-r` 或 `a-asan`：

```powershell
cmake -S <source>/cmake/render-v2-bootstrap -B <build> -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -Dstdexec_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stdexec `
  -Dstduuid_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stduuid `
  -Dtomlplusplus_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/tomlplusplus `
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> -V --output-on-failure -j 1 -R .
```

ASan flags `/fsanitize=address /Zi`；`ASAN_OPTIONS=alloc_dealloc_mismatch=1`。每次实际配置的全部参数和返回码均在外部日志中。

## 保护与历史边界

719 Legacy blobs 及主工作树 settings、Graph.hpp、另一 V12 工作树用户 RenderServer.hpp 的三个 SHA 均一致，见 `evidence/protection-a.json`。仅在隔离分支施工，未触碰原工作树；E0/E0-B/E0-C 报告不改。

旧 C1–C5 性能 PARTIAL、历史 Debug graph.edit 失败及 Root TOOLCHAIN BLOCKED 保持原判。当前未完成 Scope S/C 真实 Native 对比、最终独立 Debug/ASan/no-exceptions/GPU 矩阵及性能配对，均不得标 PASS。

## Scope C 候选检查点

`counting_scope` 内置停止源、spawn 状态所有权与独立 CommandCompletion 已接通所有异步 Command，包括真实 CreateEffect、GPU 上传和 Readback。施工 Release 与 ASan 均完成 all、第二轮 no-work 和全量 146/146 测试；ASan 214.18 秒。该结果尚非独立 V，S 方案仍待实际对照。

删除 PendingOperation 的 connected state 指针/删除器、PendingSlots、CompletionId、每请求停止源与 shutdown 快照。结果记录为 224 B（旧 Pending 272 B），不代表总请求内存下降；spawn 状态及分配仍需实测。同步终态测试改为允许标准协议规定的 inline 析构，并继续断言精确析构、Producer 不再读取状态和 Host/DLL 尾栈安全。

证据：`evidence/c/`、`evidence/c-asan/`。唯一 CompletionQueue、ResponseOutbox、Native CompletionPost、Transfer 状态机及 GPU 退休算法保留。当前没有 Scope 的生产性能 PASS 声明。
