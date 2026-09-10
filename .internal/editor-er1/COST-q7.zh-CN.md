# q7正常候选的有限成本记录

源码2d2650c5d498d4c74b0ba076dd3f5b322b6c13f7，正常RelWithDebInfo clean构建。沿用q6驱动、seed、尺寸与等待边界：100预热、500实测帧、8个单列验证帧，1600×900隐藏窗口、1024×576目标、单View、3资源、8ms节奏。每对独立进程并交替先后顺序，共五对；实际完成量及十张PPM完全匹配。不是完整Application事件循环/GUI操作成本，也没有GPU时间或p95资格。

| 批次指标 | 旧路径均值 | 新路径均值 | 单位 |
|---|---:|---:|---|
| work_wall | 0.08738692 | 0.08869704 | 秒 |
| wait | 4.34852444 | 4.35746378 | 秒 |
| work_cycles | 274688249 | 280861778 | cycles |
| wait_cycles | 288817856 | 320411995 | cycles |
| close_wall | 0.09991118 | 0.10653356 | 秒 |

本轮主动工作wall均值约增加1.50%；等待cycles五对均增加，均值约增加10.94%；关闭均值约增加6.62ms。保留这三项残余成本，不称为纯噪声，也不因图像相同宣称性能等价。

各对原始数值和delta见E:/lux-er1/q7/cost-summary.json，完整样本见cost-pairs/pairs.csv及逐进程JSON/log/PPM。所有离群值保留。q6旧有效样本及COST-q6.zh-CN.md不变；q7不反向撤销q6记录的等待cycles增加。五组有限样本不足以宣布整体性能等价、加速或稳定尾部分布。

## 资源owner静止关联规模

独立正常EXE使用真实Scene/RenderSystem/Renderer，但64/1024 Mesh均为空资源引用（UNREFERENCED）、零View，不启动资产上传。这隔离稳定owner遍历，而非READY资源/大型Scene GPU吞吐量。每种规模五个独立进程，100预热、500次实测owner更新；每次验证同一公开snapshot和revision，另记advanceScene/poll及实际关闭。此循环无8ms节奏，不能与上表UI帧数直接混算。

- 64 Mesh：500次owner更新均值0.00318376秒，五样本[0.0031681, 0.0032506, 0.0031646, 0.0031672, 0.0031683]。每cycle关联比较由准确源循环推导为2080；类型payload下界32768字节。
- 1024 Mesh：500次owner更新均值0.1477368秒，五样本[0.1461981, 0.1469599, 0.1468995, 0.1504292, 0.1481973]。每cycle关联比较由准确源循环推导为524800；类型payload下界524288字节。

比较次数是N(N+1)/2的源算法推导，未添加运行时计数器。下界仅包含ResourceRequest、两个AssetResult、预留unique_ptr槽及公开row的sizeof；排除shared控制块、allocator元数据、Registry、Outline、标签、RenderSystem、driver和RSS。不可据此声称完整保留内存或分配次数。公开snapshot的CPU引用保持至Session/Renderer关闭之后；这只读CPU数据不包含图像GPU借用。

静止快照复用不是O(1)：本轮记录了实际关联成本，未为了计时跳过准备、每帧复制全量资源或移除GPU保护。完整owner分配账目、匹配resize/retry到完成的尾部仍为PARTIAL。原始成本与故障诊断构建串行运行，诊断产物不参与正式计时。
