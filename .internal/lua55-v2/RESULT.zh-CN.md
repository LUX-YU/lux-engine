# Lua55 整链路实施 v2

状态：实施中。P0—P8 为连续子提交，当前记录不代表最终资格。

基线源码 `a79141cbb64b1d23af5f697bb981f84707225b4a`；实际旧资格
`55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644`。保留该资格的 Lua55 可运行镜像，
不重测旧 VM。开发使用既有隔离源码和受控构建槽位，不操作原始 main。

固定安装依赖：lux-cxx `3100f54d0743c5ed94a4ccf5943df04e933de255`、toolset
`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。Lua55 为官方 5.5.1，归档 SHA
`1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`。

## 接口变化登记

- 活动运行时只支持 Lua55；删除 `LUX_LUA_VM` 选择器、`ELuaExecutionPolicy` 和 JIT 字段。
  显式传入旧选择器会在根配置失败。当前 SDK 直接要求 `LuxLua55::Runtime`。
- 原 v3 多 VM 驱动移入历史诊断目录，原始内容不变；本次不会执行它们。
- 重复 interpreter CTest 删除，原业务测试与 Scene worker 覆盖保留。
- Event/Ability 为 C 函数；内部 Lua wrapper 调试帧删除，替换 `coroutine.yield`
  不拦截引擎原语。普通 Lua coroutine API 保持原行为。

## 进度

P0 (`42ffbf9d4`)：Developer 全量构建、83 项受影响 CTest、第二轮无工作通过。
依赖 C IPO 探测与 smoke 通过，安装到独立 `install/o/v2/lua55`。
P1 (`cea6bbcc` 及 fixture 修正至 `c611ca0b`)：原有 Lua 受影响测试 16 项通过，
两项新增恢复转换用例通过；立即退休 provider=0，延迟 stop provider=1，均 root 1/释放 1。
P2：不可变 CodecPlan、VM 内字段名 root、plain 树单次保护读写与 typed 外层构造已实现，验证中。
P3—P8：待完成，最终报告将登记代码、测量、未采用实验和明确限制。
