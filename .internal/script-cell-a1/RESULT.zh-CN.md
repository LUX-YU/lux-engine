# V5 / A1 ExecutionCell 实施结果

**IMPLEMENTATION_COMPLETE / CONTRACT_PASS / REGRESSION。** A0–A6 已实际完成；本候选不值得作为当前 v4 的替代，保留在实验分支供架构审阅。结构确实改变、已测语义保持，但主目标没有取得整体收益，预留内存明显增加。这是否定本候选，不是否定所有可能的 cell 设计。

## 1. 身份与冻结边界

- 实验分支 `codex/s6-execution-cell-a1`，从 `f92853ea8361a7dc90ce0da072497d3722c29f88` 创建；不合并原分支/main。
- 固定 A：`f7d2815bdd2025ee23a7c11449def822413f58e9` 的 v4 匹配镜像。起始44项与最终镜像 hash 复核，旧产物未修改。
- B 的实际完整资格/计时源码：`caf34bd307912450d894da922b59af4238154508`，独立 clean clone，唯一最终标准产物。本文和归档提交不是新测量身份。
- Windows x64、MSVC19.44.35228.0、RelWithDebInfo `/O2 /Ob1 /MD`，单 owner，affinity mask16。Lua5.5.1 + lux-lua55-v3-r2、INC原值、16MiB完整空页预算；Native ABI6。
- lux-cxx `3100f54d0743c5ed94a4ccf5943df04e933de255`、toolset `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`：204个实际安装文件再核验。VM DLL hash `34f11df8679a46b2d956462a99a9a3ba49ee29e88b85bb27b1c2b3c5a78bb29e` 两侧一致。
- VM patch、allocator、GC、codec、NativeFrameStorage、BoundedClassStorage、编译器/ISA/IPO及三份cooked基准资产均未改变。

完整字段、命令、产物 hash、配对分布见 [result.json](result.json)，源码/合同见 [NOTES](NOTES.zh-CN.md)，逐项证据见 [TEST_MATRIX.csv](TEST_MATRIX.csv)。唯一新原始包见 [证据索引](../evidence/script/script-cell-a1/INDEX.zh-CN.md)。原 main 的7项修改全部与既有登记同 hash，原开发工作树未操作。

## 2. 实际删掉与保留的工作

普通本地 Event/Timer 不再构造独立完整 AwaitableRecord；首次挂起在 provisional cell 中晋升 execution role，连续本地等待重新填同一 local slot。原独立 C/A 完整存储被一个稳定 C+A bank 和两个薄身份目录替换；只有一个 ready FIFO，没有双引擎选择器。

但没有消除所有管理操作：每次 A 仍需逻辑接纳与generation，C仍在backend返回后接纳；owner链、来源取消关系、selected wait关联、pin、权限及代次检查保留。来源直达 cell 后，公开句柄、外部完成和清理仍可能查目录。`attached_execution` 不等于 hosting cell，保留未绑定/额外/foreign token 的合法语义。

普通链为：`waitEvent → scopeFor → A目录 → provisional local cell → 来源登记 → backend返回 → C目录/原位晋升 → attachWaiter → claim/callback/copy → ready → take到栈上/归还A → backend.resume → unlink/hook解锁/C归还 → backend.destroy`。每次用户代码返回均按原权限重验，Timer queue-full 重试与 Event queue-full fault 不合并。

跨后端唯一必要C++接口变化是 EventWaitFactory 取得准确 `ScriptStepContext` 借用；CppStatic/Lua直接构造fixture已迁移，Native/FlowForge仍透传同一step。公开C++头与DLL必须配套重编译；Native ABI6、token字节与资产格式不变。统计口径变化（A目录 vs 全部bank）明确列在NOTES。

## 3. 独立结构观察

诊断使用同一源码的 HOTPATH_OBSERVATION=ON 构建；正式42次全部OFF。下表是1000实例、16预热+128帧的实测累计值，含预热；A没有新计数，保持null。常规Event的每次完整周期包含开始、等待、完成、恢复、销毁。

| 计数 | Lua Event B | C++ Sequence B |
|---|---:|---:|
| start / resume / complete | 144000 / 144000 / 144000 | 36000 / 108000 / 36000 |
| cell acquire / release | 144000 / 144000 | 36000 / 36000 |
| 原位晋升 / 额外execution创建 | 144000 / 0 | 36000 / 0 |
| local waits / 后续rearm | 144000 / 0 | 108000 / 72000 |
| boxed（external/无scope/布局/已占local） | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| A接纳 / 实际归还 | 144000 / 144000 | 108000 / 108000 |
| C接纳 / 实际归还 | 144000 / 144000 | 36000 / 36000 |
| 来源直达 / 来源目录解析 | 144000 / 0 | 108000 / 0 |

local独立完整结果池为零是源码布局事实；新统计并未给A编造零。Sequence是原NextStep/Event/simulation delay混合业务；local↔external过渡、extra/未绑定/foreign-target由独立真实runtime负例验证，不把普通诊断的boxed=0推广到所有调用。

Lua两侧都新建/恢复/解除root 144000个thread；384B档和736B档各144000次周期请求仍在。两侧总VM alloc=297477、realloc=88，fresh thread/stack没有少做。关闭ScriptSystem仅解除脚本资源，Lua VM仍可保留等待GC的对象，最终VM销毁合同由VM/后端测试验证。

## 4. 语义与真实消费者

同一新runtime测试分别用A历史头+固定DLL、B候选DLL执行，31条精确业务轨迹相同。覆盖C晚接纳的副作用/析构、A1 pin物理占槽、初始eager完成、重复消费者、旧context嵌套、stale pop预算、32次A1重装填、foreign target、COMPLETED留下wait，以及两方向local/external过渡。

补充同时耗尽Event/A，双方都优先返回WAITER_CAPACITY_EXCEEDED；同名Delay publication双方均AMBIGUOUS_PROVIDER、provider/backend=0。不能把当前不支持的builtin覆盖当成可执行旧合同。原catalog错context/dispatch测试和通用自定义异步provider测试保留。

七点重入、同步退休错误、Bindings回滚、实例复用、copy增长/退休、Event/Timer queue-full差异、真实step/frontier/预算、Scene及三后端旧断言继续通过。64B/64对齐payload在真实storage层验证分配失败1次、恢复成功、take值与回收；未额外为三后端各建64B Event资产。T25来源/清理排列按既有多个fixture分开覆盖，未声称穷举全部组合。public测试代次上限4、private手动终值覆盖拒绝wrap，不是运行数十亿轮。

## 5. 资源总账

实际MSVC布局：A完整C=72B、A结果=192B；B execution=88B、wait共用状态144B、local176B、boxed232B；union/header组合后的cell **320B**。boxed已包含在bank最大宽度内，不能再加一遍，也不能省略这份reserved。大payload spill另计。来源Event 88→112B、Timer104→128B、临时claimed snapshot28→56B；ready24→64B。

以C=A=ready=Event=Timer=external queue=10000、10000实例/方法、1个Event endpoint测量实际reserve，单位B。`dynamic`是向下层operator new提出的真实在用请求，包括STL对齐及array cookie，不是OS/RSS；容器owner对象单列避免重复计算。

| 所有动态backing | A | B |
|---|---:|---:|
| C完整池 | 840117 | 包含在bank/目录行 |
| A完整稳定池（10240物理槽） | 2335695 | 包含在bank/目录行 |
| cell bank + C/A薄目录 | 0 | 7120086 |
| ready ring | 240039 | 640039 |
| execution实例/方法索引 | 800078 | 800078 |
| Event来源、路由、claim、实例索引 | 1771361 | 2011361 |
| Timer来源、external capability槽、heap、实例索引 | 2400312 | 2640312 |
| shared transport、completion ring、tickets | 1045856 | 1040096 |
| 四owner对象（含嵌入容器，不重复计transport） | 1096 | 1224 |
| **上述合计** | **9434554** | **14253196** |

增 **4818642B（约4.60MiB，+51.07%）**，这是四组件及其全部owned backing，不是整个引擎内存增幅。B逻辑bank=6400000B、C目录320000B、A目录400000B；实际allocation多出的字节记录在上表。free/owner/epoch均在cell/目录内，没有另藏一份旧pool。A ticket按10240物理A准备，B按10000，transport少5760B，未改变公开逻辑配额。

初始prepare触及全部cell header，不能把未使用角色称为未预留。结束快照active cell=0；bulk cell峰值与不同地址touch数没有独立采集，result.json为null。单cell稳定复用和pin物理保活由断言验证，不能据结束为0推断峰值为0。

Instances/Bindings/Preparer、C++ frame arena/root、Native N×R与metadata、Lua VM仍按v4容量独立保留，并没有由cell取代。[v4资源账](../script-v4/RESULT.zh-CN.md)中的C++ 10000容量arena5440000B+metadata240280B、原FlowForge Event payload160000B及metadata1200272B继续适用（源码/配置未改）；这些不是本轮新测得的不同后端统一成本。LuaVM由本轮资源诊断另计，不能将跨时间峰值相加。

| 1000实例独立资源观察 | 峰值working set MiB A→B | 峰值private commit MiB A→B |
|---|---:|---:|
| Lua Event | 19.793 → 20.051 | 12.734 → 12.945 |
| C++ Sequence | 12.438 → 12.957 | 5.578 → 6.070 |

每进程仅6–7次5ms查询，记录Windows返回的进程累计峰值；有限小场景不能代表10000实例峰值。Lua末快照两侧active/idle页与待GC对象不同，A/B下层heap请求91/88、页供给88/85；总thread/stack请求相同。GC受地址与回收进度影响，不能把这点当成cell节省VM内存的证据；heap请求不是OS syscall，16MiB仍只约束完整空页。完整class/slack/retained记录保留。

## 6. 完整业务成本

每腿三对独立进程AB/BA/AB，所有42次有效，不筛除慢组、不另追3%门槛。以下A/B列分别是各侧成本中位，变化是各对B/A−1后取中位，不能直接将前列相除代替。Event/NextStep/scalar ROI20M调用；FlowForge Event ROI10M；record ROI1M；Sequence1000整帧（2.5M start、7.5M resume）；Physics2000整帧。原warmup/seed/budget/worker保持，Sequence恰逢完整序列边界，无额外drain帧。

| 腿 | 单位 | A → B | 三对变化 | 配对中位 |
|---|---|---:|---|---:|
| Lua Event | ns/完整周期或调用 | 720.396 → 739.447 | +4.69%/+2.64%/-1.38% | +2.64% |
| Lua NextStep | ns/完整周期或调用 | 718.545 → 759.574 | +5.68%/+6.03%/+12.94% | +6.03% |
| FlowForge Event | ns/完整周期或调用 | 336.666 → 376.357 | +9.85%/+12.07%/+12.04% | +12.04% |
| C++ Sequence | ms/整帧 | 2.754 → 2.904 | +4.84%/+6.17%/+1.27% | +4.84% |
| Lua scalar | ns/完整周期或调用 | 137.981 → 138.727 | +1.69%/-0.84%/+3.08% | +1.69% |
| ValuePose record | ns/完整周期或调用 | 1321.731 → 1320.286 | -0.11%/-1.84%/+0.09% | -0.11% |
| Physics mixed | ms/整帧 | 2.982 → 3.185 | +4.10%/+8.09%/+7.25% | +7.25% |

| 腿 | A三次完整ROI总秒数 | B三次完整ROI总秒数 |
|---|---|---|
| Lua Event | 14.494285 / 14.407924 / 14.167562 | 15.173902 / 14.788938 / 13.971753 |
| Lua NextStep | 14.370896 / 14.327454 / 14.592638 | 15.187833 / 15.191475 / 16.481609 |
| FlowForge Event | 3.392353 / 3.358109 / 3.366656 | 3.726380 / 3.763572 / 3.772008 |
| C++ Sequence | 2.770167 / 2.753588 / 2.611919 | 2.904374 / 2.923598 / 2.645132 |
| Lua scalar | 2.728444 / 2.759617 / 2.794065 | 2.774536 / 2.736433 / 2.880218 |
| ValuePose record | 1.321731 / 1.305702 / 1.342343 | 1.320286 / 1.281696 / 1.343553 |
| Physics mixed | 5.964810 / 5.893883 / 6.006388 | 6.209374 / 6.370570 / 6.441578 |

完整总量、每帧CSV、p50/p95/p99/max及错误范围在result.json/原始包。各组最终ready backlog为0；Sequence/Physics帧内存在原有in-flight等待，并非全部调用同步完成。可观测累计errors=0；A的Sequence只有旧业务/资源/shutdown断言，没有累计failure输出，因此该字段为null。正式VM/EXE-local可选分配计数关闭时为未采集；Physics原EXE-local计数为0不代表全部DLL零分配。没有独立wall-clock恢复延迟直方图；恢复时机由真实step协议测试证明。

**没有主目标收益。** Event方向混合，NextStep、FlowForge、Sequence与Physics三对都更慢。scalar两慢一快、record小幅混合，不声称等价。v4 scalar/Native metadata旧债务继续登记，本轮增量单独归为当前候选成本，不用旧债务豁免。

## 7. 采样、机器码与资格

对B只补一次软件Hotspots，完整子进程CPU19.638259s、elapsed21.856173s，包含setup/warmup/oracle/shutdown；业务30M Event与checksum930010000通过。自耗：Awaitables::admit0.580928s（2.96%）、Continuations::tryEmplace0.314880s（1.60%）、waitEvent0.265301s、attachWaiter0.186419s、resumeOne0.140878s；luaV_execute0.707738s、traversestrongtable0.616084s、luaH_Hgetshortstr0.532432s。新接纳/晋升仍是可见成本，并非把所有损耗都放到resume。

实际DLL反汇编保留；admit入口VA0x18008c970仍建立0x78字节局部栈区、检查/计算薄目录容量，随后处理稳定header/角色与失败分支。减少旧目录解析没有让整个入口成为一次直接store。更宽cell/来源/ready、额外定位和管理是由源码/布局支持的候选解释；**单侧软件样本不能证明全部新增回退的微架构归因**。没有PMU/cache-miss结论，不据此追加无界inline搜索或VM改动。

最终独立clean clone：Developer **127/127**、Toolchain **111/111**，两个all `-j4 -- -k0`成功、第二轮无工作；冻结VM合同 **4/4**；真实安装消费者 **15/15**；增量 **13项**（含预期生成拒绝，按expected_success验收）；两条迁址链 **2/2**。迁址时原source、两个build、原SDK/tools/VM前缀均暂时不可用，恢复后身份再核验。public头与qualification clone原始hash相同；无private cell/owner头泄露。所有后端、真实provider字段值、错误恢复/生命周期和关闭清理旧断言保留。

初期失败和驱动无效记录原样保留，见result.json.invalid_attempts：回调签名/测试构造/错误重入endpoint已修正；观察驱动遗漏-S被CMake拒绝；补充trace驱动未匹配-c被命中门禁拒绝；header审计曾误将开发工作树换行差异当身份差异，最终按clean clone原始hash核验。正式42次没有无效重测或挑选快组。观察构建已恢复OFF，未覆盖正式安装。

## 8. 结论与停点

本轮验证了“本地等待由执行cell承载”可以实现，并保持已测的复杂等待、失败及重入合同；没有证据表明此实现优于v4。它减少了普通路径的独立完整记录，但更多预留、宽目标和管理操作没有换来CPU改善，建议**不采用当前候选**。

该结论不表示所有cell布局已经穷尽，也不把旧架构视为性能极限。只在实验分支提交/推送，等待A1架构审阅；不继续VM/source-link合并、allocator/codec/frame或其它阶段。未验证Windows ARM64/Linux/Android、其它编译器或真实游戏资产；不恢复Lua54/JIT，不发布tag，不合并main。
