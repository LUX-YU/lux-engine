# SR-5 权限补正：审阅摘要

本轮完成实际补正与一次有限成本检查，停在SR-5。生产身份 **f64cadde**；旧C0为6086e4a4。

- 每个普通/async Ability入口都检查初始core资格。can_reenter只决定转换后原资格重验。
- 已绑定但失效的authority必须拒绝；真正standalone仍受原backend frame/layout检查。
- 正式生成/runtime的retire/stop × scalar/zero四个独立修前进程均实际调用provider一次，修后两VM均为零。
- ACTIVE、同VM嵌套、Begin/End、普通输入错误后的合法重试，以及原七点清理/身份/pin/frontier/预算保持通过。
- 两个窄改有独立证据：直接写准入访问结果；输出操作不再初始化未使用的256字节错误路径。保护与对象清理未删减。

仅RelWithDebInfo，clean C1：Toolchain 108/108、Developer 123/123、Lua54受影响110/110，15安装消费者、13增量检查通过。
两种VM各五项独立资格命令通过；实际安装与生成输入有身份清单。六项Scene Lua保留既有已修正协议断言，本轮未重做。

| 最终成本 | 五对配对中位变化 | 绝对影响 |
|---|---:|---:|
| C0→C1 scalar Ability | +9.151% | +6.9321 ns/provider；30M调用+207.9628 ms |
| H→C1 scalar Ability | +25.648% | +16.7296 ns/provider |
| C0→C1两字段输出 | −5.419% | −13.7756 ns/输出；C1中位240.004 ns |
| protected手写诊断→C1输出 | +95.665% | +116.4941 ns/输出，保护次数1对3 |
| C0→C1 Lua coroutine | +8.099% | 整批均摊+78.8398 ns/实际新调用；backlog同为8,000 |

绝对成本均摊完整工作，不是单函数计时。完整八场景、所有配对和分配诊断见主报告；旧缺失errors保留null。
输出错误/backlog均为零。首次H运行的VS环境警告记录保留并排除，新进程五对全部纳入；没有按速度删除样本。
coroutine新增准入工作已定位到调用边界，但全部时间尚未独立分离；不把残余称为性能等价或全部必要安全成本。

建议接受正确性补正与有限调查结果；**进入SR-6仍需审阅方明确接受新增成本与残余**。本轮不再扩优化项目，不合并main。
主工作区七项未知修改、HEAD和固定依赖均保持；原证据包不改写。

[完整资格、测试、成本及限制](script-system-sr5-admission-correction-2026-09-07.zh-CN.md) ·
[新原始证据与下载](evidence/script/sr5/admission/README.md)
