# SR-5 C++↔Lua 值转换：待统一审阅的实施设计

本轮只交设计，没有修改生产转换代码。源码核查基于 `22304336`，本轮移动赋值补正不改变下述转换路径。
lux-cxx 固定 `3100f54d0743c5ed94a4ccf5943df04e933de255`；不升级依赖、不新增反射系统或万能 Variant。
下文“拟议”接口、注解和测试类型尚未实现；需统一审阅后才进入编码。

推荐第一批：严格 scalar、i32/u32 底层枚举、显式导出的有限嵌套值 record；先打通 typed 普通 Ability 双向转换，
以及引擎调用 Lua 的 record 输入和既有 Event payload 输出。字符串/对象图/任意非平凡跨挂起值不随之开放。
ScriptExecution 继续只管理既有最终结果、字节寿命和执行协议，不负责字段转换。

## 1. 当前真实链路与能力

下表路径相对仓库。`engine/domain/simulation/scripting/lua/` 简记为 LuaBackend，
`modules/function/script/` 简记为 ScriptModule；这些简称仅用于本文，不拟建目录或 target。

| 方向 | 当前源码/符号 | 已有、缺失与应改位置 |
|---|---|---|
| 引擎→Lua export参数 | [LuaScriptBackend.cpp](../engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp)：prepareMethod、LuaFunctionBinding.argument_marshallers、recordMarshaller、pushArgument、invoke/invokePreparedStep | bool/i32/u32/float/double直接push；record须CONST_REF且语义ID/名称匹配marshaller，调用时检查slot布局。只提供push；不能据此推论整个backend无读取能力 |
| Lua export→C++同步返回 | 同文件：prepareMethod的returns校验、readReturn、writeNumber、invoke | 已有严格scalar读取及slot写入；可恢复export有返回值时当前拒绝UNSUPPORTED_SIGNATURE。record新构造返回没有当前入口，首批不开放任意erased输出构造 |
| Lua→普通Ability参数 | [script_ability_lua.template](../modules/function/script/core/template/script_ability_lua.template)→[LuaScriptAbilityProjection.hpp](../engine/domain/simulation/scripting/lua/include/lux/engine/simulation/scripting/lua/LuaScriptAbilityProjection.hpp)：invokeLuaAbility、readLuaAbilityArguments、LuaAbilityScalar、LuaAbilityProjectionAccess::read/current | 正式路径是生成typed thunk，当前tuple的scalar读取后直达typed dispatch/provider；并非每次走通用反射。backend工厂还用supportedType拒绝非scalar贡献，两处都要闭环迁移 |
| 普通Ability→Lua结果 | 同projection：invokeLuaAbility、LuaAbilityProjectionAccess::push/succeed；backend的wrapAbility/pushWrapperFactory | 现有scalar返回与const scalar借用结果先复制为Value；结果要求trivially_copyable。Lua wrapper在C++ thunk退出后才error/yield。不能把模板返回值检查和prepare校验只改一处 |
| 可恢复Ability | startLuaAbility→[ScriptAbilityInvocation.hpp](../engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp)::invokeScriptAbilityAsync→Execution-owned结果→LuaBackend::pushResumeValue | 当前typed结果void或TypeDeclared且trivially_copyable；Lua可表示结果仍仅scalar，结果pass=VALUE/lifetime=AWAITABLE。异步参数BORROWED_STEP被正式validation拒绝；转换本身不取得跨挂起所有权 |
| Event payload→Lua | LuaBackend::prepareEvents、pushResumeValue的EVENT分支；ScriptExecution::completeClaimedEventWaiter/ResultWritePin | prepared来源定义布局；Execution先拥有projection/copy得到的结果，resume时才转Lua；record在resume时仍查marshaller目录并push。这是值表示边界，不能将Lua table塞入Event来源或运输ring |
| record注册 | [LuaScriptBackend.hpp](../engine/domain/simulation/scripting/lua/include/lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp)::LuaRecordMarshaller、LuaScriptBackendConfig；cpp工厂验证/State构造 | 当前仅push；工厂校验semantic ID/name/size/power-of-two alignment/函数，拒绝ID或名称重复，配置复制到backend稳定目录；context指向的用户资源仍需宿主维持有效 |
| 另一条通用Lua绑定 | [meta_lua.template](../modules/function/script/lua/cmake/template/meta_lua.template)、engine_lua_codegen.cmake、Lua.hpp/ScriptEngine | 已存在sol2 new_usertype对象/方法注册，不是当前ScriptSystem的typed Ability值转换。模板实际字段条件是visibility和类型拼写含不含`<`；不能把注释中的过滤愿望当成已落实的递归/opt-out规则 |
| 宿主/组件 | LuaBackend::HostHandle、hostHandle、getComponent、patchComponent、ScriptBehavior/ScriptHostApi | 已有对象身份和组件访问边界；不因新值转换而暴露Registry，Entity不展开字段、不换成WorldObjectId |

`readAbilityArgument/pushAbilityResult` 是backend内部scalar adapter，后者也服务异步resume；
普通生成调用应沿typed projection迁移，不能只改这些同名内部函数后宣称全链路支持。
当前readStrictNumber与checkedLuaNumber已拒绝非number、非有限、整数小数部分、溢出；布尔要求Lua布尔，保留这些约束。

现有手写record消费者：LuaBackend `test/lua_backend_test.cpp` 的pushCollisionEvent、
`test/collision_event_chain_test.cpp` 的marshaller。二者的CollisionEvent是测试类型（body:i32、impulse:float），
不是已证明可直接展开的所有Physics领域事件。当前检索没有其他生产LuaRecordMarshaller注册。

## 2. 固定lux-cxx究竟提供什么

核查文件：`reflection/include/lux/cxx/reflection/runtime/{Declaration,Type,Marker}.hpp`、
`reflection/src/parser/parse_class_declaration.cpp`、`reflection/src/generator/{MetaGenerator,GeneratorHelper}.cpp`。

| 事实 | 当前可用内容 | 本设计的使用/限制 |
|---|---|---|
| CXXRecordDecl | fq_name、bases、constructor_decls、destructor_decl、field_decls、静态/普通方法、is_abstract/is_template | 取字段声明和显式导出事实，不把存在constructor节点等同可调用/无异常构造 |
| FieldDecl/Type | visibility、parent_class、offset；type ID/kind、const/volatile、size/align，指针/引用/record/enum类别 | 生成`value.field`及`decltype`typed调用，不使用offset。当前FieldDecl没有可直接依赖的is_bitfield/default-initializer语义；不猜构造默认值 |
| Marker与模板回调 | LUX_META、LUX_REFLECT_EXTERNAL(_ENUM)，decl_from_id/index、type_from_id、annotation_has/get_for_or、marked_record_decls | 复用现有解析/模板机制；新标记命名空间只定义值导出政策，不建新AST。外部类型需要显式列入拥有者输入，不能对整个include闭包自动授权导出 |
| 目标编译器判定 | 不能由parse-time size/offset或缺失字段推出目标ABI性质 | 生成static_assert/requires验证成员可访问且可取成员指针、聚合/构造表达式、nothrow move/destructor、实际sizeof/alignof、底层枚举范围。bitfield取成员指针失败，明确不支持；不用不可靠offset推断 |
| 增量生成 | MetaGenerator::processTargetFile收集on_included_file；depfile记录包含依赖；先验证投影再发布；publishOutputs比较内容相同则不重写 | 复用此单一路径并补真实字段依赖变化测试。不是声称只要顶层头不变就无需重新生成 |

ABI语义仍来自现有 [SemanticType.hpp](../modules/core/semantic/include/lux/engine/core/semantic/SemanticType.hpp)::TypeTraits/TypeDeclared
及 [ScriptAbility.hpp](../modules/function/script/core/include/lux/engine/function/script/ScriptAbility.hpp)::makeScriptAbilityValue。
转换策略不自行创造第二个semantic ID。枚举底层表示、record是否能作为具体调用/结果的ABI值须另外校验。
没有在固定依赖中验证到的能力（可靠的默认成员表达式求值、所有模板特殊化字段展开、bitfield宽度等）一律不作为首批前提。

## 3. 第一批类型 × 方向 × 寿命

符号：现=当前已有；拟=首批建议编码；延=明确延期。值codec可用不等于任意backend签名自动可用。

| 类型 | C++值→Lua值 | Lua→新C++值 | 普通typed Ability双向 | Lua export同步返回到erased slot | Event/异步结果跨挂起 | 原对象引用/身份绑定 |
|---|---|---|---|---|---|---|
| bool/i32/u32/float/double | 现 | 现 | 现，接统一规则 | 现 | 现，保留结果布局/寿命 | 不新增 |
| enum class，底层i32/u32 | 拟：整数 | 拟：声明枚举值集合 | 拟，需TypeTraits语义及投影检查 | 延；不能把底层slot当枚举对象随意写 | 首批延，只保留既有生命周期枚举通路 | 不新增 |
| 显式导出record，仅上述叶与有限嵌套record | 拟：raw table；替代手写push | 拟：全新typed临时对象 | 拟：参数value/const&，返回值受现有trivially-copyable要求；不增加任意非平凡ABI | 延：先保留record CONST_REF输入，返回需要独立构造slot协议 | 仅已有owned-layout许可的Event payload转Lua；新record异步Ability结果延 | 延；table是独立快照，不是写回原对象的代理 |
| 带不变量、非聚合或非standard-layout值类 | 用户typed规则可拟；无布局限制 | 用户nothrow工厂可拟；自动路径有条件 | 首批只接已满足既有签名ABI者，其余只做codec层验证 | 延 | 延；不可据trivially_copyable认定所指数据owned | 延 |
| std::string / string_view / char* | 首批延；将来string复制为Lua带长度字节串 | 首批延；将来只构造拥有字节的string，禁止返回悬空view | 延 | 延 | 延；需要可持有的字节存储/析构协议 | 禁止自动对象绑定 |
| Entity | 继续既有HostHandle/宿主能力 | 不接受Lua整数或table制造Entity | 新通用Entity参数首批延 | 延 | 仅已有完整runtime身份协议 | 身份规则单独设计，含runtime关联和代次验证；不展开EnTT内部位 |
| AssetId/资产引用 | 首批延；将来ID值与resident资产对象分开 | 首批延；文本ID不自动取得lease | 延 | 延 | ID可复制不意味着资产/代码保持驻留 | 不自动resolve、不引回World |
| raw pointer、组件引用、span、vector/map、unique/shared_ptr对象图 | 不默认展开 | 不自动构造 | 延/明确unsupported | 延 | 延 | 延 |

第一批实际消费者以两份CollisionEvent fixture迁移为基础，并增加拟议的PoseValue/VelocityValue嵌套测试。
暂不承诺Eigen向量、所有Transform/Physics类型都可自动反射；真正产品需要的少量引擎值类型逐个用现有semantic事实和规则纳入。
同样不把跨线程Lua访问视为转换方向：所有Lua API只在既有owner/VM线程执行，worker只提交既有owned结果。

## 4. 唯一规则体系与授权

拟议统一入口为ScriptModule Lua包中的`LuaValueCodec<T, Policy>`（仅类型规则选择），优先级按每个方向独立确定：
显式`LuaValueOverride<T, Policy>` → 内建叶规则 → 生成`LuaGeneratedValue<T>` → 带完整类型/字段路径的不支持诊断。
不另建marshaller工厂、反射解释器、脚本字符串规则注册三条并行体系。

用户规则按去cv/ref后的真实C++类型和显式Policy提供特化；semantic TypeTraits用于生成描述、注册/prepare匹配，不能仅凭同名字符串替代T。
每方向可独立实现，缺少read不妨碍push，但需要read的调用必须在生成/prepare时失败。规则能覆盖内建和默认table表示；
覆盖builtin的范围限定到显式选择的conversion policy/生成拥有者，禁止进程范围悄悄改变所有`int32_t`的表示。
首批一个backend配置只装配一种确定policy，各模块同semantic类型必须使用相同policy指纹，否则工厂拒绝歧义。
同一T的用户特化只能由一个正式头拥有；不同T占用同semantic ID、重复目录项、读写政策冲突都拒绝，不能按注册先后覆盖。

record必须显式opt-in；建议新的`luxlua::value`与`luxlua::field`注解，沿现有LUX_META解析。
只自动读取public非static字段，字段默认导出，可显式export=false或改Lua键名；键名重复生成失败。
private/protected字段即使标注export=true也报错，不能通过offset越权；用户规则调用类公开访问器另当别论。
父类自动展开、union、循环对象图首批不支持。被排除字段不参与table，读取新对象必须由构造策略明确初始化，不能留下未初始化字节。
依赖图按声明ID/真实T追踪，嵌套record也须opt-in；生成所有参与根的specialization，typed递归按字段的decltype调用。
值类型循环/未知叶/未导出依赖在准备业务之前失败；不生成运行时逐字符串查反射成员的解释器。

最小作者形状如下。`LUX_META`是真的现有宏；`luxlua::*`、Reader/Writer、Override及Result均为**本设计拟议API**，不是当前可编译承诺。
Reader/Writer只暴露受保护的值操作，不给完整State/Registry，也不让用户拿裸lua_State跨调用保存。

```cpp
struct LUX_META(luxlua::value, export = true) AngleValue { float radians; };

struct GameLuaPolicy {}; // Explicitly selected by this backend's generated contribution.
template<> struct lux::script::lua::LuaValueOverride<AngleValue, GameLuaPolicy> {
    // Override the default record table with a numeric degree representation.
    static ConversionResult<AngleValue> read(LuaValueReader& input) noexcept {
        auto degrees = input.number<float>(); // Strict numeric check; no string coercion.
        if (!degrees) return unexpected(degrees.error());
        return AngleValue{*degrees * 0.017453292519943295f};
    }
    static ConversionResult<void> push(LuaValueWriter& output, const AngleValue& value) noexcept {
        return output.number(value.radians * 57.29577951308232f);
    }
};
```

此例演示规则选择，数值舍入/非有限和范围仍由统一叶规则验证；正式测试使用独立手写Lua预期，不能用同一codec制造期待值。
默认record输出的生成语句形如`writeField("velocity", value.velocity, LuaValueCodec<decltype(value.velocity), Policy>{})`，
读入则创建字段typed临时值再构造目标；不按offset做memcpy。无需整个类型standard-layout。

## 5. 输入、构造与事务边界——推荐的明确决定

| 决定 | 首批推荐语义 |
|---|---|
| table形状 | 只读raw字段并验证键集合，拒绝未知键/非字符串键；不触发`__index/__newindex/__pairs`。metatable可以存在，但不会作为缺失字段的数据源 |
| 缺失/nil | Lua raw table不能区分不存在和存nil，两者同为缺失错误；首批不推断C++默认成员表达式，也不隐式零填充。显式用户read规则可定义自己的默认政策并承担测试 |
| bool/number | bool只接受LUA_TBOOLEAN；number只接受LUA_TNUMBER，不接受数字字符串。整数要求有限/无小数/严格范围，负数不得转unsigned。Lua→C++ float允许通常的IEEE舍入但拒绝溢出及非有限；不要求每个十进制小数float精确可表示。C++→Lua默认保留当前float/double直接push（包括非有限值），不借统一规则偷偷收紧旧输出语义；需要双向有限值的类型以显式policy覆盖 |
| enum | 首批底层i32/u32；Lua用声明枚举值整数，拒绝集合外值；允许同值别名，不解释任意bitmask组合。更宽底层与字符串枚举表示由后续显式规则批准 |
| 聚合构造 | 所有输入先读入typed临时槽、检查完再`T{...}`；目标编译器证明is_aggregate和构造表达式。含被排除/需要默认字段的聚合不猜参数位置：转显式工厂或合格的默认构造策略 |
| 默认构造后赋值 | 仅显式选择该策略且T可nothrow默认构造、导出字段可安全赋值时生成；失败销毁未发布临时对象。不能要求所有类型变为默认可构造/可赋值 |
| 自定义工厂 | typed输入验证后调用`noexcept -> expected<T, ConversionFailure>`；工厂不操作Lua。非聚合/有不变量类型走此路；按构造flag精确析构，禁止以throw报告业务失败 |
| 更新旧对象 | 首批不支持。读取只产生新值；不会承诺任意对象原地更新的自动回滚。业务组件patch仍由既有能力执行 |
| 多参数失败 | 任何参数转换失败均不进入provider业务；已构造参数逆序销毁，Lua栈恢复入口top；返回结构化错误，含arg/result和嵌套字段路径 |
| 返回转换失败 | provider已执行的业务不自动回滚；报告结果转换错误，不能谎称“目标业务未调用”。要保证业务不发生只能在参数失败阶段或业务自身事务中实现 |

`ConversionFailure`拟含错误分类、语义类型、方向、参数/结果序号、有限字段路径；路径按固定最大深度/字节数截断并显式标记，
不能让错误文本构造再次无界分配。首批推荐深度32、每record至多64导出字段，prepare拒绝超限，运行期table键数有界扫描。
仅限制已选转换配置，不改ScriptSystem执行容量或恢复预算。容量值是需用户审阅的建议，不是偷偷新增生产默认值。

## 6. Lua栈、错误、重入与寿命

成功push净增加一个Lua值；失败push恢复入口top。read成功/失败都恢复入口top，并且只在成功时提交完整新T。
table是独立Lua值，不保留源record指针；输入table存活期间读取字段，构造的C++值不引用可变Lua table或其临时栈槽。
所有转换句柄只借用当前调用，不可跨yield保存。const T&参数临时值活到同步dispatch/async starter返回；
async provider若需保留数据必须遵守既有owned_value复制规则，不能把“codec能读”升级为“允许borrowed_step跨挂起”。

不能仅用C++栈RAII guard解释Lua错误安全。raw访问不会运行metamethod，但table/string分配、栈增长/GC仍可能失败或执行用户finalizer。
推荐有限`LuaValueReader/Writer`让所有可能失败的Lua操作在受保护C trampoline中执行，以status/expected返回；
用户codec不直接调用lua_error/lua_yield，也不获得裸VM接口。typed对象和构造flag由保护调用**外侧**的转换frame拥有，
trampoline内只放平凡借用/索引，不能在可能longjmp的Lua API之上放`std::string`、expected<T>或任意非平凡临时。
纯C++工厂产生的expected/T须先提交外侧slot并销毁临时，之后才能再次调用Lua API。

生成路径可将一个已知形状的push批处理为一个typed trampoline，前提仍是所有需要析构的对象都在外侧。
自定义Writer使用受保护的有限操作；不以减少保护次数为由暴露可长跳越过C++析构的接口。
VM栈容量预检、trampoline取用/安装与错误提取也必须纳入目标LuaJIT/Lua54实机失败注入；不把`lua_pcall`包住一处调用当全边界证明。
错误转成原Lua wrapper的`ok,message`协议，C++转换/参数/结果对象销毁后才在Lua层error/yield；不替换VM，也不全仓改RAII。

每个调用/递归重入有独立frame，借用prepare时确定的类型ops；最大并发深度沿现有execution_depth_capacity有界预留。
用户规则、工厂和GC均视为可能重入：允许其他合法实例/方法调用；禁止yield转换；保留当前调用票据，用户边界后重验身份/权限。
不得跨用户代码缓存ACTIVE，不以sameIncarnation重新授权。失败/取消时frame先退出再让Execution结束原等待；不添加stable点。
新的字段转换仅发生在Lua backend/Lua值模块，EventWaits/Timers/Ingress继续处理有限源与transport，最终字节仍只有Execution一个owner。

## 7. 生成→prepare→调用、安装与迁移

复用现有 `lux_add_codegen_job / lux_codegen_add_validation / lux_codegen_add_projection / lux_target_add_codegen`。
正式 [engine_script_ability_codegen.cmake](../modules/function/script/core/cmake/engine_script_ability_codegen.cmake)::lux_script_abilities
现在从显式SOURCES/LOGICAL_PATHS生成`.ability.generated.hpp/.ability.lua.generated.hpp/.ability.native.generated.hpp`及schema writer。
新增值投影放ScriptModule的Lua包 `cmake/template/`，建议`.lua.value.generated.hpp`和对应validation；
共享Lua读写/保护操作放该Lua包include/src，复用`lux::engine::function::script_lua` target；
runtime-specific prepared access仍留`lux::engine::simulation::simulation_script_lua`。不新增DLL。通用Script/Simulation头不include Lua私有实现。

类型头/自定义规则头是显式生成输入；只为被选根、实际方法方向及依赖闭包生成，不枚举全工程类型×语言×方向。
拟议一个`lux_script_lua_values(TARGET ... SOURCES ... RULE_HEADERS ... LOGICAL_PATHS ...)`薄CMake入口，复用现有job，不另启动扫描服务。
每个类型只有一个生成拥有者；公共生成头的安装由其拥有target显式安装到正式include，Lua模板/helper由Lua包安装导出。
Ability生成使用同一值codec及已安装类型头；构建工具依赖仍是BUILD_TOOL，不进入runtime link。

编译生成header时用目标编译器实例化选择、访问、构造和ABI约束，产出有限只读描述：semantic ID/name、size/align、方向、policy指纹、typed ops。
工厂/prepare复制并验证冷目录，按具体export/Ability/Event需求固定有限ops入口；provider实例/代码lease维持原owner。
descriptor不是资产身份缓存，不按AssetId或裸地址永久保留转换资格；目录释放晚于所有借用它的实例/prepared，遵守现有backend目录有效期。
Event目前resume按type ID查marshaller，应改为prepare时固定push op；来源owner不参与此优化，转换ops也不取得事件/执行容器。

增量失效覆盖：包含的字段类型、注解、用户规则头、模板、编译选项、logical path及semantic schema变化。
用户规则即使没被parser深入分析也必须显式作为生成/编译依赖；使用现有depfile与publishOutputs，不在生成后手工改文件。
源类型/表示policy变化须重新生成和prepare，首批不提供热重载；schema/wire改变另按现有协议判断，不能仅改type name忽略表示变化。

旧LuaRecordMarshaller本轮不删；下一轮实现完成时一次迁移两份实际fixture和新typed Ability/Event消费者，
用同一`LuaValueOverride<T, Policy>/LuaGeneratedValue<T>`生成唯一ops目录并删除旧配置入口/类型，**不保留永久legacy/generated双轨或兼容别名**。
普通scalar读取也接入同一叶规则，保留现有错误行为；通用sol2对象绑定可以继续承担原对象绑定职责，不能作为ScriptSystem值转换fallback。
backend descriptor/C ABI不扩展；若erased record返回构造必须改ABI，那属于延期项，需要额外批准，不夹带在首批。

## 8. 下一轮最小编码次序与独立预期验收

| 步骤/真实入口 | 新用例与通过条件 |
|---|---|
| 值投影/strict叶与用户规则 | 从实际测试头生成PoseValue{velocity:{x,y},mode}，Lua字面table与手算值验证双向；AngleValue覆盖默认table；只push规则在要求read时生成/prepare失败 |
| 构造/失败事务 | 缺字段/nil/未知键/非法键、数值字符串/负unsigned/NaN/Inf/窄化/无效enum精确失败；第2个嵌套字段失败时第1个已构造对象析构一次，provider调用0；返回转换失败业务调用1 |
| 访问/依赖 | private导出、bitfield、pointer/const或不可赋值字段、未opt-in嵌套类型、循环/未知模板形态给出独立诊断；非standard-layout且公开typed访问合法的规则用例通过，不用offset |
| 生成typed Ability | 扩展真实LuaRuntimeTestAbility风格的拟议EchoPose方法，走`.ability.lua.generated.hpp`与provider；保留已有scalar/bool/range/const-result错误断言，不用手写Lua adapter伪装生成链路 |
| 旧record入口闭环 | lua_backend_test、collision_event_chain_test用实际生成输出替代手写marshaller；独立Lua字面assert(body==...,impulse==...)；原projection/复制/Event/预算/pin不变 |
| Lua错误/重入 | 分配器在table/string/stack增长各点故障注入；外侧frame析构计数与栈top恢复；GC/custom规则合法嵌套调用、退休/关闭重验、禁止yield，失败后下一次调用正常 |
| 结果寿命 | 重置原Channel、改原C++record后Lua已push快照不变；既有跨step owned结果回归保留；首批禁止的非拥有view/非平凡异步结果做编译或prepare负例 |
| 增量与安装 | 改被include字段类型/注解/规则头触发重生成，第二次no-op输出mtime/hash不变；改模板/编译宏也失效；隔离安装消费者移开source/build后仍可生成编译执行，私有头不泄漏 |

LuaJIT是首批必测实际VM；若共享Lua值模块的修改声称兼容Lua54，必须加对应真实VM验证，否则明确未覆盖。
普通record转换会创建Lua table并可能分配；性能分转换本身、真实Ability/export整条调用、cold prepare、VM与DLL/EXE分配账目，
不得许诺递归table转换零分配。用相同工作量独立进程比较新增转换成本，既有Event债务和LuaUpdate/prepare成本继续单列。
本轮设计未运行转换性能或假造生成产物。

## 9. 待统一审阅的少数产品决定

1. 是否批准首批范围：scalar/有限enum/opt-in嵌套record，优先typed普通Ability双向与现有record输入/Event输出；字符串、Entity新值接口、record异步Ability/erased返回延期。
2. 是否批准strict raw table、缺失/nil错误、拒绝多余键、i32/u32枚举集合，以及建议深度/字段上界；需要宽松表示的类型通过显式用户规则覆盖。
3. 是否批准统一typed规则及一次迁移删除LuaRecordMarshaller入口；自定义规则只用受保护Reader/Writer，不拿完整VM/Runtime权限。

这些是编码前要统一批准的设计决定，不是本轮已实现能力。固定lux-cxx的缺失事实由目标编译器/明确unsupported处理，
不自动要求依赖升级。A的生命周期修正独立交付；B完成不授权SR-5生产转换编码，更不授权SR-6或冻结框架。


SR-5 实施授权后的接口细化与实际支持边界见
[首批实施记录](script-system-sr5-implementation-2026-09-07.zh-CN.md)。
单一表示按完整 Rule 选择，方向不能分别 fallback；const 聚合与工厂构造采用该记录的具体实现。
本设计的未来扩展不构成任意类型、对象绑定或跨挂起所有权已经实现的声明。
