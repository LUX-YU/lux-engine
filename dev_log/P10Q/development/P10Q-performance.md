# P10Q 性能记录

最终实现：`3c20910d05d635078ef9021a3a3318086a792b8d`。基线实现为 P10 R1；原始提交和依赖见 before/baseline.json。

RelWithDebInfo、同机、固定依赖；微基准预热 10 次、测量 100 次。真实画布长期回归另记 10k 轮（10 次预热、9990 次计时）。全部分位数来自实际原始输出，不按比例推算。
分配计数另跑计数段，范围为当前线程可执行文件／静态 archive 的 C++ new；不覆盖 DLL CRT、malloc、GPU。
长基线进程与构建存在时间重叠，因此墙钟受负载影响；不据此承诺固定提速百分比。

## BQ1：全图层级验证

旧算法对每个对象重复走祖先链；新算法在一次身份／父索引后做迭代三色遍历。正常引用的访问次数由 N² 深链变为 O(N+E)，调用栈深度恒定。
时间来自原 SDK；parent 读取计数单独使用固定 Git 原体重编的测试插桩，仅在实际读取前累加一次。插桩差异和原体哈希保留，不作为安装消费者资格。新索引使用 O(N) 额外内存；不声称分配减少。

| 输入 | 修复前 p50／p95／p99／max（µs） | 修复后 p50／p95／p99／max（µs） |
|---|---|---|
| 1000 chain | 14534.900 / 14811.000 / 14990.000 / 14990.000 | 54.500 / 61.100 / 74.800 / 74.800 |
| 1000 wide | 33.300 / 33.700 / 35.500 / 35.500 | 50.500 / 52.900 / 91.200 / 91.200 |
| 1000 roots | 1233.500 / 1363.200 / 1469.300 / 1469.300 | 54.600 / 56.200 / 61.200 / 61.200 |
| 1000 cycle | 31.000 / 31.300 / 33.600 / 33.600 | 54.900 / 56.300 / 63.700 / 63.700 |
| 10000 chain | 3354548.000 / 4856167.100 / 5951881.400 / 5951881.400 | 855.400 / 932.200 / 1027.800 / 1027.800 |
| 10000 wide | 338.400 / 360.900 / 396.900 / 396.900 | 755.300 / 826.700 / 944.100 / 944.100 |
| 10000 roots | 13779.800 / 14093.000 / 14389.400 / 14389.400 | 851.500 / 898.600 / 1071.800 / 1071.800 |
| 10000 cycle | 404.100 / 421.500 / 537.500 / 537.500 | 879.600 / 981.500 / 1047.100 / 1047.100 |
| 50000 chain | PARTIAL 66/100：127996728.000 / 175826032.100 / 190765031.000 / 190765031.000 | 5516.300 / 6189.900 / 6546.300 / 6546.300 |
| 50000 wide | 2303.300 / 2568.800 / 2710.900 / 2710.900 | 4563.500 / 4993.100 / 5281.500 / 5281.500 |
| 50000 roots | 122565.100 / 128917.400 / 192570.600 / 192570.600 | 5705.200 / 6348.900 / 6651.800 / 6651.800 |
| 50000 cycle | 2998.700 / 3824.000 / 4459.600 / 4459.600 | 5920.600 / 7085.000 / 8535.300 / 8535.300 |

旧版 50k 深链按用户明确指示结束，已有完整迭代的原始 stderr 保留；停止中的一轮不计入。未达到 100 个计时样本，因此性能项／XQ24 为 PARTIAL。上表若列部分分位数，只描述已完成样本，不能当作原计划的完整统计。

实际 parent 读取计数（单次计数段，不混入计时）：

### 修复前

```text
parent_reads n=1000 shape=chain count=500500
parent_reads n=1000 shape=cycle count=1000
parent_reads n=1000 shape=roots count=50500
parent_reads n=1000 shape=wide count=1999
parent_reads n=10000 shape=chain count=50005000
parent_reads n=10000 shape=cycle count=10000
parent_reads n=10000 shape=roots count=505000
parent_reads n=10000 shape=wide count=19999
parent_reads n=50000 shape=chain count=1250025000
parent_reads n=50000 shape=cycle count=50000
parent_reads n=50000 shape=roots count=2525000
parent_reads n=50000 shape=wide count=99999
```

### 修复后

```text
parent_reads n=1000 shape=chain count=1000
parent_reads n=1000 shape=cycle count=1000
parent_reads n=1000 shape=roots count=1000
parent_reads n=1000 shape=wide count=1000
parent_reads n=10000 shape=chain count=10000
parent_reads n=10000 shape=cycle count=10000
parent_reads n=10000 shape=roots count=10000
parent_reads n=10000 shape=wide count=10000
parent_reads n=50000 shape=chain count=50000
parent_reads n=50000 shape=cycle count=50000
parent_reads n=50000 shape=roots count=50000
parent_reads n=50000 shape=wide count=50000
```


分配计数（单次计数段，未混入计时样本）：

### 修复前

```text
hierarchy n=1000 shape=chain owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=1000 shape=cycle owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=1000 shape=roots owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=1000 shape=wide owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=10000 shape=chain owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=10000 shape=cycle owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=10000 shape=roots owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=10000 shape=wide owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=50000 shape=chain owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=50000 shape=cycle owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=50000 shape=roots owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
hierarchy n=50000 shape=wide owner-thread executable/static new calls=0 requested_bytes=0 (one count-only run; no timing qualification)
```

### 修复后

```text
hierarchy n=1000 shape=chain owner-thread executable/static new calls=1005 requested_bytes=65630 (one count-only run; no timing qualification)
hierarchy n=1000 shape=cycle owner-thread executable/static new calls=1005 requested_bytes=65630 (one count-only run; no timing qualification)
hierarchy n=1000 shape=roots owner-thread executable/static new calls=1005 requested_bytes=65630 (one count-only run; no timing qualification)
hierarchy n=1000 shape=wide owner-thread executable/static new calls=1005 requested_bytes=65630 (one count-only run; no timing qualification)
hierarchy n=10000 shape=chain owner-thread executable/static new calls=10005 requested_bytes=752429 (one count-only run; no timing qualification)
hierarchy n=10000 shape=cycle owner-thread executable/static new calls=10005 requested_bytes=752429 (one count-only run; no timing qualification)
hierarchy n=10000 shape=roots owner-thread executable/static new calls=10005 requested_bytes=752429 (one count-only run; no timing qualification)
hierarchy n=10000 shape=wide owner-thread executable/static new calls=10005 requested_bytes=752429 (one count-only run; no timing qualification)
hierarchy n=50000 shape=chain owner-thread executable/static new calls=50005 requested_bytes=3498861 (one count-only run; no timing qualification)
hierarchy n=50000 shape=cycle owner-thread executable/static new calls=50005 requested_bytes=3498861 (one count-only run; no timing qualification)
hierarchy n=50000 shape=roots owner-thread executable/static new calls=50005 requested_bytes=3498861 (one count-only run; no timing qualification)
hierarchy n=50000 shape=wide owner-thread executable/static new calls=50005 requested_bytes=3498861 (one count-only run; no timing qualification)
```


## BQ2：目录与任务

每个时间样本为 10 次 Root update，另有 1000 次稳定 update 分配计数。shared_buffers 表示与首个视图共用数组的消费者个数，不是物理数组个数。旧目录已无稳定帧全量读取；新目录的收益是多个消费者共享版本数组。旧 tasks-revision 路径已经避免稳定复制，不能与未提供 revision 的 tasks 路径混淆。

### 修复前

```text
catalog-1000-1.log
BQ2 catalog: 10 stable Root updates per sample entries=1000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.300 p99_us=0.300 max_us=0.300
catalog read_calls=1 copied_rows=1000 initial_distinct_buffers=1 stable_read_calls=0
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-1000-2.log
BQ2 catalog: 10 stable Root updates per sample entries=1000 views=2 warmup=10 samples=100 p50_us=0.300 p95_us=0.300 p99_us=0.300 max_us=0.300
catalog read_calls=2 copied_rows=2000 initial_distinct_buffers=2 stable_read_calls=0
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-1000-8.log
BQ2 catalog: 10 stable Root updates per sample entries=1000 views=8 warmup=10 samples=100 p50_us=0.700 p95_us=0.800 p99_us=0.800 max_us=0.800
catalog read_calls=8 copied_rows=8000 initial_distinct_buffers=8 stable_read_calls=0
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-10000-1.log
BQ2 catalog: 10 stable Root updates per sample entries=10000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.300 p99_us=0.300 max_us=0.300
catalog read_calls=1 copied_rows=10000 initial_distinct_buffers=1 stable_read_calls=0
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-10000-2.log
BQ2 catalog: 10 stable Root updates per sample entries=10000 views=2 warmup=10 samples=100 p50_us=0.300 p95_us=0.300 p99_us=0.300 max_us=0.300
catalog read_calls=2 copied_rows=20000 initial_distinct_buffers=2 stable_read_calls=0
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-10000-8.log
BQ2 catalog: 10 stable Root updates per sample entries=10000 views=8 warmup=10 samples=100 p50_us=0.700 p95_us=0.800 p99_us=0.800 max_us=0.800
catalog read_calls=8 copied_rows=80000 initial_distinct_buffers=8 stable_read_calls=0
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-1000-1.log
BQ2 tasks default: 10 Root updates/sample entries=1000 views=1 warmup=10 samples=100 p50_us=259.500 p95_us=514.400 p99_us=557.700 max_us=557.700
tasks shared_buffers=1 first_view_buffer_changes=1100 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-1000-2.log
BQ2 tasks default: 10 Root updates/sample entries=1000 views=2 warmup=10 samples=100 p50_us=988.500 p95_us=1120.000 p99_us=1146.000 max_us=1146.000
tasks shared_buffers=1 first_view_buffer_changes=1100 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-1000-8.log
BQ2 tasks default: 10 Root updates/sample entries=1000 views=8 warmup=10 samples=100 p50_us=1346.200 p95_us=1614.700 p99_us=1673.600 max_us=1673.600
tasks shared_buffers=1 first_view_buffer_changes=1100 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-10000-1.log
BQ2 tasks default: 10 Root updates/sample entries=10000 views=1 warmup=10 samples=100 p50_us=4860.500 p95_us=6539.600 p99_us=7066.700 max_us=7066.700
tasks shared_buffers=1 first_view_buffer_changes=1100 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-10000-2.log
BQ2 tasks default: 10 Root updates/sample entries=10000 views=2 warmup=10 samples=100 p50_us=9283.700 p95_us=11315.100 p99_us=14875.700 max_us=14875.700
tasks shared_buffers=1 first_view_buffer_changes=1100 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-10000-8.log
BQ2 tasks default: 10 Root updates/sample entries=10000 views=8 warmup=10 samples=100 p50_us=41419.400 p95_us=58566.900 p99_us=60986.400 max_us=60986.400
tasks shared_buffers=1 first_view_buffer_changes=1100 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-1000-1.log
BQ2 tasks revision: 10 Root updates/sample entries=1000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.300 max_us=0.300
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-1000-2.log
BQ2 tasks revision: 10 Root updates/sample entries=1000 views=2 warmup=10 samples=100 p50_us=0.300 p95_us=0.300 p99_us=28.100 max_us=28.100
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-1000-8.log
BQ2 tasks revision: 10 Root updates/sample entries=1000 views=8 warmup=10 samples=100 p50_us=0.700 p95_us=0.700 p99_us=0.700 max_us=0.700
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-10000-1.log
BQ2 tasks revision: 10 Root updates/sample entries=10000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.300 max_us=0.300
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-10000-2.log
BQ2 tasks revision: 10 Root updates/sample entries=10000 views=2 warmup=10 samples=100 p50_us=0.300 p95_us=0.300 p99_us=0.300 max_us=0.300
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-10000-8.log
BQ2 tasks revision: 10 Root updates/sample entries=10000 views=8 warmup=10 samples=100 p50_us=0.700 p95_us=0.700 p99_us=0.700 max_us=0.700
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
```

### 修复后

```text
catalog-1000-1.log
BQ2 catalog: 10 stable Root updates per sample entries=1000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
catalog shared_buffers=1 distinct_buffers=1 stable_first_buffer=1
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-1000-2.log
BQ2 catalog: 10 stable Root updates per sample entries=1000 views=2 warmup=10 samples=100 p50_us=0.300 p95_us=0.300 p99_us=0.300 max_us=0.300
catalog shared_buffers=2 distinct_buffers=1 stable_first_buffer=1
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-1000-8.log
BQ2 catalog: 10 stable Root updates per sample entries=1000 views=8 warmup=10 samples=100 p50_us=0.600 p95_us=0.700 p99_us=0.700 max_us=0.700
catalog shared_buffers=8 distinct_buffers=1 stable_first_buffer=1
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-10000-1.log
BQ2 catalog: 10 stable Root updates per sample entries=10000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
catalog shared_buffers=1 distinct_buffers=1 stable_first_buffer=1
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-10000-2.log
BQ2 catalog: 10 stable Root updates per sample entries=10000 views=2 warmup=10 samples=100 p50_us=0.200 p95_us=0.300 p99_us=0.300 max_us=0.300
catalog shared_buffers=2 distinct_buffers=1 stable_first_buffer=1
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
catalog-10000-8.log
BQ2 catalog: 10 stable Root updates per sample entries=10000 views=8 warmup=10 samples=100 p50_us=0.600 p95_us=0.600 p99_us=0.700 max_us=0.700
catalog shared_buffers=8 distinct_buffers=1 stable_first_buffer=1
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-1000-1.log
BQ2 tasks default: 10 Root updates/sample entries=1000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-1000-2.log
BQ2 tasks default: 10 Root updates/sample entries=1000 views=2 warmup=10 samples=100 p50_us=0.200 p95_us=0.300 p99_us=0.300 max_us=0.300
tasks shared_buffers=2 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-1000-8.log
BQ2 tasks default: 10 Root updates/sample entries=1000 views=8 warmup=10 samples=100 p50_us=0.500 p95_us=0.600 p99_us=1.600 max_us=1.600
tasks shared_buffers=8 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-10000-1.log
BQ2 tasks default: 10 Root updates/sample entries=10000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-10000-2.log
BQ2 tasks default: 10 Root updates/sample entries=10000 views=2 warmup=10 samples=100 p50_us=0.200 p95_us=0.300 p99_us=0.300 max_us=0.300
tasks shared_buffers=2 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-10000-8.log
BQ2 tasks default: 10 Root updates/sample entries=10000 views=8 warmup=10 samples=100 p50_us=0.500 p95_us=0.600 p99_us=0.600 max_us=0.600
tasks shared_buffers=8 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-1000-1.log
BQ2 tasks revision: 10 Root updates/sample entries=1000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-1000-2.log
BQ2 tasks revision: 10 Root updates/sample entries=1000 views=2 warmup=10 samples=100 p50_us=0.200 p95_us=0.300 p99_us=0.300 max_us=0.300
tasks shared_buffers=2 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-1000-8.log
BQ2 tasks revision: 10 Root updates/sample entries=1000 views=8 warmup=10 samples=100 p50_us=0.500 p95_us=0.600 p99_us=0.600 max_us=0.600
tasks shared_buffers=8 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-10000-1.log
BQ2 tasks revision: 10 Root updates/sample entries=10000 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
tasks shared_buffers=1 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-10000-2.log
BQ2 tasks revision: 10 Root updates/sample entries=10000 views=2 warmup=10 samples=100 p50_us=0.200 p95_us=0.300 p99_us=0.300 max_us=0.300
tasks shared_buffers=2 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
tasks-revision-10000-8.log
BQ2 tasks revision: 10 Root updates/sample entries=10000 views=8 warmup=10 samples=100 p50_us=0.500 p95_us=0.600 p99_us=0.600 max_us=0.600
tasks shared_buffers=8 first_view_buffer_changes=0 (address-change count; not all allocator calls)
1000 stable updates: owner-thread executable/static C++ new calls=0 requested_bytes=0 (not all DLL/CRT/GPU heap)
```


## BQ3：画布身份

隔离身份映射微基准与真实 GraphCanvas 10k churn 分开。真实回归覆盖 Root 路由鼠标、在途输入、作者 ID、不复活旧 UI ID、首帧及后续 pan/zoom/selection。后端重建仅在安全点；没有重编号作者身份。

### 修复前

```text
canvas-ids.log
before mapping retained=30001 old_source=18446744073709451615 10k rounds new_calls=30047 requested_bytes=4479017; isolated mapping cost, not UI compaction qualification
mapping 10k rounds warmup=10 samples=100 p50_us=2056.300 p95_us=3135.000 p99_us=3457.300 max_us=3457.300
```

### 修复后

```text
canvas-ids.log
after mapping retained=1317 old_source=0 10k rounds new_calls=30304 requested_bytes=4112009; isolated mapping cost, not UI compaction qualification
mapping 10k rounds warmup=10 samples=100 p50_us=1600.300 p95_us=2298.100 p99_us=2374.800 max_us=2374.800
canvas.log
BQ3 actual setGraph plus Root layout/draw: warmup=10 samples=9990 p50_us=3902.500 p95_us=9661.500 p99_us=10794.600 max_us=17721.700; retained identities <=4096
```


## BQ4：冻结字节

实际 Material 编译结果被用于冻结传输资格，扩展 payload 只测运输开销，不声称扩展字节是合法的 Material 编码。记录物理字节 owner 和重复复制；这不是整个进程 RSS。真实 IO、取消与 Unknown 由原保存回归另行验证。

### 修复前

```text
bytes.log
BQ4 actual Material publication admission (sized transport payload) entries=1048576 views=1 warmup=10 samples=100 p50_us=264.900 p95_us=306.500 p99_us=360.000 max_us=360.000
payload_aliases=0/110 copied_payload_bytes_per_admission=1048576
BQ4 actual Material publication admission (sized transport payload) entries=16777216 views=1 warmup=10 samples=100 p50_us=2441.500 p95_us=2729.800 p99_us=2872.700 max_us=2872.700
payload_aliases=0/110 copied_payload_bytes_per_admission=16777216
BQ4 actual Material publication admission (sized transport payload) entries=67108864 views=1 warmup=10 samples=100 p50_us=10212.700 p95_us=11735.200 p99_us=15007.200 max_us=15007.200
payload_aliases=0/110 copied_payload_bytes_per_admission=67108864
```

### 修复后

```text
bytes.log
BQ4 actual Material publication admission (sized transport payload) entries=1048576 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
payload_aliases=110/110 copied_payload_bytes_per_admission=0
BQ4 actual Material publication admission (sized transport payload) entries=16777216 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
payload_aliases=110/110 copied_payload_bytes_per_admission=0
BQ4 actual Material publication admission (sized transport payload) entries=67108864 views=1 warmup=10 samples=100 p50_us=0.200 p95_us=0.200 p99_us=0.200 max_us=0.200
payload_aliases=110/110 copied_payload_bytes_per_admission=0
```


## BQ5 与候选路径

WriteCoordinator 保留小容量线性扫描。Flow 选择的整图冻结已由实测证明重复，改为复用选择来源戳，仍逐次通过原 gate；没有更换容器。Outliner、冻结投影保留实际实现；未证明替换方案能抵消维护和身份成本时，不改为新容器。StableSlotMap 地址稳定不等于回调期间可删除，既有 gate 必须保留。

### 修复前

```text
records.log
BQ5 actual WriteCoordinator capacity=16 lookups_per_sample=1600 p50_us=11.600 p95_us=12.900 p99_us=15.300 max_us=15.300
BQ5 actual cancel/ack erase capacity=16 p50_us=2.200 p95_us=2.400 p99_us=2.500 max_us=2.500
BQ5 actual WriteCoordinator capacity=64 lookups_per_sample=6400 p50_us=88.100 p95_us=96.700 p99_us=101.000 max_us=101.000
BQ5 actual cancel/ack erase capacity=64 p50_us=31.300 p95_us=32.300 p99_us=37.700 max_us=37.700
BQ5 actual WriteCoordinator capacity=256 lookups_per_sample=25600 p50_us=940.900 p95_us=1021.000 p99_us=1100.200 max_us=1100.200
BQ5 actual cancel/ack erase capacity=256 p50_us=482.200 p95_us=487.900 p99_us=548.500 max_us=548.500
candidate-paths.log
FlowInteraction synchronize n=100 selected=1 warmup=10 samples=100 p50_us=103.200 p95_us=111.800 p99_us=120.400 max_us=120.400 new_calls_per_sample=925 requested_bytes_per_sample=255351 (owner executable/static C++ new only)
FlowInteraction synchronize n=100 selected=16 warmup=10 samples=100 p50_us=104.100 p95_us=113.500 p99_us=145.600 max_us=145.600 new_calls_per_sample=925 requested_bytes_per_sample=255351 (owner executable/static C++ new only)
FlowInteraction synchronize n=100 selected=64 warmup=10 samples=100 p50_us=103.700 p95_us=112.500 p99_us=169.000 max_us=169.000 new_calls_per_sample=925 requested_bytes_per_sample=255351 (owner executable/static C++ new only)
SceneSession frozen capture n=100 selected=0 warmup=10 samples=100 p50_us=12.500 p95_us=13.800 p99_us=49.600 max_us=49.600 new_calls_per_sample=24 requested_bytes_per_sample=9903 (owner executable/static C++ new only)
Outliner stable Root update n=100 selected=0 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
Outliner explicit rebind rows n=100 selected=0 warmup=10 samples=100 p50_us=16.500 p95_us=18.300 p99_us=25.300 max_us=25.300 new_calls_per_sample=237 requested_bytes_per_sample=65435 (owner executable/static C++ new only)
SceneProjection stable update n=100 selected=0 warmup=10 samples=100 p50_us=0.000 p95_us=0.100 p99_us=0.100 max_us=0.100 new_calls_per_sample=0 requested_bytes_per_sample=0 (owner executable/static C++ new only)
SceneProjection changed field plus frozen rebuild and retirement n=100 selected=0 warmup=10 samples=100 p50_us=345.900 p95_us=428.700 p99_us=457.700 max_us=457.700 new_calls_per_sample=72 requested_bytes_per_sample=86329 (owner executable/static C++ new only)
FlowInteraction synchronize n=1000 selected=1 warmup=10 samples=100 p50_us=884.800 p95_us=1126.400 p99_us=1190.500 max_us=1190.500 new_calls_per_sample=9034 requested_bytes_per_sample=2162582 (owner executable/static C++ new only)
FlowInteraction synchronize n=1000 selected=16 warmup=10 samples=100 p50_us=811.100 p95_us=1053.100 p99_us=1117.200 max_us=1117.200 new_calls_per_sample=9034 requested_bytes_per_sample=2162582 (owner executable/static C++ new only)
FlowInteraction synchronize n=1000 selected=64 warmup=10 samples=100 p50_us=826.700 p95_us=1293.600 p99_us=1438.100 max_us=1438.100 new_calls_per_sample=9034 requested_bytes_per_sample=2162582 (owner executable/static C++ new only)
SceneSession frozen capture n=1000 selected=0 warmup=10 samples=100 p50_us=33.600 p95_us=52.400 p99_us=111.800 max_us=111.800 new_calls_per_sample=24 requested_bytes_per_sample=53103 (owner executable/static C++ new only)
Outliner stable Root update n=1000 selected=0 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=183.900 max_us=183.900 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
Outliner explicit rebind rows n=1000 selected=0 warmup=10 samples=100 p50_us=225.100 p95_us=279.900 p99_us=443.700 max_us=443.700 new_calls_per_sample=2048 requested_bytes_per_sample=492580 (owner executable/static C++ new only)
SceneProjection stable update n=1000 selected=0 warmup=10 samples=100 p50_us=0.000 p95_us=0.100 p99_us=0.100 max_us=0.100 new_calls_per_sample=0 requested_bytes_per_sample=0 (owner executable/static C++ new only)
SceneProjection changed field plus frozen rebuild and retirement n=1000 selected=0 warmup=10 samples=100 p50_us=976.900 p95_us=1092.700 p99_us=1177.800 max_us=1177.800 new_calls_per_sample=72 requested_bytes_per_sample=180007 (owner executable/static C++ new only)
```

### 修复后

```text
records.log
BQ5 actual WriteCoordinator capacity=16 lookups_per_sample=1600 p50_us=14.100 p95_us=14.400 p99_us=16.700 max_us=16.700
BQ5 actual cancel/ack erase capacity=16 p50_us=2.200 p95_us=2.200 p99_us=2.300 max_us=2.300
BQ5 actual WriteCoordinator capacity=64 lookups_per_sample=6400 p50_us=114.900 p95_us=117.300 p99_us=124.100 max_us=124.100
BQ5 actual cancel/ack erase capacity=64 p50_us=28.200 p95_us=30.100 p99_us=34.400 max_us=34.400
BQ5 actual WriteCoordinator capacity=256 lookups_per_sample=25600 p50_us=965.000 p95_us=1053.500 p99_us=1069.400 max_us=1069.400
BQ5 actual cancel/ack erase capacity=256 p50_us=422.800 p95_us=458.500 p99_us=474.000 max_us=474.000
candidate-paths.log
FlowInteraction synchronize n=100 selected=1 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
FlowInteraction synchronize n=100 selected=16 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
FlowInteraction synchronize n=100 selected=64 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
SceneSession frozen capture n=100 selected=0 warmup=10 samples=100 p50_us=11.900 p95_us=13.900 p99_us=16.200 max_us=16.200 new_calls_per_sample=24 requested_bytes_per_sample=9903 (owner executable/static C++ new only)
Outliner stable Root update n=100 selected=0 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
Outliner explicit rebind rows n=100 selected=0 warmup=10 samples=100 p50_us=17.400 p95_us=19.800 p99_us=22.200 max_us=22.200 new_calls_per_sample=237 requested_bytes_per_sample=65435 (owner executable/static C++ new only)
SceneProjection stable update n=100 selected=0 warmup=10 samples=100 p50_us=0.000 p95_us=0.100 p99_us=0.100 max_us=0.100 new_calls_per_sample=0 requested_bytes_per_sample=0 (owner executable/static C++ new only)
SceneProjection changed field plus frozen rebuild and retirement n=100 selected=0 warmup=10 samples=100 p50_us=210.100 p95_us=278.500 p99_us=375.000 max_us=375.000 new_calls_per_sample=72 requested_bytes_per_sample=86329 (owner executable/static C++ new only)
FlowInteraction synchronize n=1000 selected=1 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
FlowInteraction synchronize n=1000 selected=16 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
FlowInteraction synchronize n=1000 selected=64 warmup=10 samples=100 p50_us=0.100 p95_us=0.200 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
SceneSession frozen capture n=1000 selected=0 warmup=10 samples=100 p50_us=32.700 p95_us=33.200 p99_us=39.900 max_us=39.900 new_calls_per_sample=24 requested_bytes_per_sample=53103 (owner executable/static C++ new only)
Outliner stable Root update n=1000 selected=0 warmup=10 samples=100 p50_us=0.100 p95_us=0.100 p99_us=0.200 max_us=0.200 new_calls_per_sample=1 requested_bytes_per_sample=32 (owner executable/static C++ new only)
Outliner explicit rebind rows n=1000 selected=0 warmup=10 samples=100 p50_us=181.800 p95_us=247.100 p99_us=300.700 max_us=300.700 new_calls_per_sample=2048 requested_bytes_per_sample=492580 (owner executable/static C++ new only)
SceneProjection stable update n=1000 selected=0 warmup=10 samples=100 p50_us=0.000 p95_us=0.100 p99_us=0.100 max_us=0.100 new_calls_per_sample=0 requested_bytes_per_sample=0 (owner executable/static C++ new only)
SceneProjection changed field plus frozen rebuild and retirement n=1000 selected=0 warmup=10 samples=100 p50_us=647.300 p95_us=734.900 p99_us=774.900 max_us=774.900 new_calls_per_sample=72 requested_bytes_per_sample=180007 (owner executable/static C++ new only)
```

## 保留方案与覆盖范围

- SessionStore／RunStore／ViewHost 的唯一 owner、generation 和执行保护未替换；没有因为容器名更新就采用新算法。
- SmallVector／StableSlotMap 仅完成真实依赖资格与容量测量；不把 UUID 用作稠密 key，不把 SBO 当全局零分配保证。
- 全快照投影的内容变化成本仍存在；本阶段未引入组件级增量投影框架。
- Raw samples、命令、失败尝试、依赖和机器信息随归档保存。缺少的实际测量必须在 receipt 标为 PARTIAL，不能用源码推导冒充实测。
