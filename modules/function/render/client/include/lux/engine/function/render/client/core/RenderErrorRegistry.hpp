#pragma once
/**
 * @file RenderErrorRegistry.hpp
 * @brief 错误类型注册表:类型绑定、幂等、代化。
 *
 * 用法:
 *   ErrorTypeId id = errors.errorType<err::memory::OutOfMemory>();
 *   return lux::cxx::unexpected(makeError(id));
 *
 * **重复注册在结构上不可能**:errorType<T>() 以 lux::cxx::type_hash<T>() 为键做幂等
 * get-or-create,同一个 T 第二次调用返回同一个 id。这与 ResourceRegistry 保证「一个
 * 资源类型只有一份」用的是同一把钥匙。
 *
 * 剩下唯一可能的重复是「两个不同类型声明了同一个 name」:引擎自带的那批在编译期由
 * static_assert 拦住(见文件末),动态注册的在运行期 renderFatal —— 它是编码错误,
 * 不是运行期状况,不该用错误码表达。
 *
 * 线程模型:写极少(构造时一批 + feature 装卸若干条),读来自消费线程(拿到错误要
 * 显示或分诊时);渲染线程本身不读 —— 它只产生 20 字节的 RenderError,不做格式化。
 * 所以 shared_mutex 的争用实质为零,这是少数值得加锁的地方。
 *
 * 查询返回 ErrorTypeDesc **拷贝**而不是指针:内部存储在插入时会重排,而 desc 只有
 * 三个指针加两个小字段,拷贝比正确管理那个指针便宜得多。
 *
 * 注意 ErrorTypeDesc::name / message 指向本模块二进制里的静态字面量,**只在同进程内
 * 有效**。跨进程时改由 comm 拉一次 (id, name, message, recovery) 表,过线的
 * ErrorTypeId 本身与进程无关,协议和错误值都不用改。
 */

#include <lux/engine/function/render/client/core/RenderError.hpp>
#include <lux/engine/function/render/client/core/RenderErrorList.hpp>
#include <lux/engine/function/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>    // lux::cxx::unexpected(renderFailure)
#include <lux/cxx/compile_time/type_info.hpp>   // lux::cxx::type_hash
#include <lux/cxx/container/BasicSparseSet.hpp> // lux::cxx::SlotKeyAutoSparseSet

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC RenderErrorRegistry
    {
    public:
        /// 构造即登记 LUX_RENDER_ERROR_LIST 里的全部引擎错误类型,于是构造完成后整张表
        /// 就是完整的 —— 诊断面板 / 门禁可以立刻枚举「这个引擎会报哪些错」,而不必等到
        /// 某个错误第一次真的发生。
        RenderErrorRegistry();

        RenderErrorRegistry(const RenderErrorRegistry&) = delete;
        RenderErrorRegistry& operator=(const RenderErrorRegistry&) = delete;

        /// 取 T 的 id,没登记过就登记。幂等。
        template <ErrorType T> [[nodiscard]] ErrorTypeId errorType()
        {
            return acquire(lux::cxx::type_hash<T>(), errorTypeDesc<T>());
        }

        /// 归还 T 的槽位(动态 feature 卸载时调用)。槽位的 generation 随之推进,
        /// 之前发出去的 id 从此查不到,不会命中回收后落进这个槽的新类型。
        template <ErrorType T> void forget() noexcept
        {
            release(lux::cxx::type_hash<T>());
        }

        [[nodiscard]] std::optional<ErrorTypeDesc> find(ErrorTypeId id) const;

        /// 按稳定名解析 —— 测试断言、门禁规则、跨端查表走这条。
        [[nodiscard]] ErrorTypeId findByName(std::string_view name) const;

        /// 全表快照,供诊断面板 / 拉表用。按 id 升序未作保证。
        [[nodiscard]] std::vector<std::pair<ErrorTypeId, ErrorTypeDesc>> snapshot() const;

        [[nodiscard]] std::size_t size() const;

    private:
        using TypeKey = std::uint64_t; ///< lux::cxx::type_hash 的返回类型

        ErrorTypeId acquire(TypeKey key, const ErrorTypeDesc& desc);
        void release(TypeKey key) noexcept;

        mutable std::shared_mutex mutex_;
        lux::cxx::SlotKeyAutoSparseSet<ErrorTypeId, ErrorTypeDesc> types_;
        std::unordered_map<TypeKey, ErrorTypeId> by_type_;
        std::unordered_map<std::string_view, ErrorTypeId> by_name_;
    };

    /// 把一次失败渲染成人读文本:查 desc,按每一槽声明的 EErrorArg 展开实参,填模板。
    /// 可以跑在任意线程,也可以跑在消费侧进程 —— 名字在这里才被解析出来。
    /// 若 id 已失效(动态 feature 卸载后收到的旧事件),返回一条自证的占位文本。
    [[nodiscard]] LUX_FUNCTION_PUBLIC std::string
    formatRenderError(const RenderErrorRegistry& registry, const RenderError& error);

    // ── 程序级入口 ───────────────────────────────────────────────────────
    //
    // 错误类型是**代码的静态属性**,不属于任何设备或场景实例:同一个 T 在任何
    // RenderServer 里都是同一个错误类型,内容完全由 T 决定。所以这张表按程序作用域
    // 存在,而不是挂在某个实例上 —— 否则最早的设备创建代码(那时还没有任何实例)
    // 就无法报错,而把它穿进三十来个构造函数与 InitInfo 只是纯管线活,没有任何
    // 语义收益。
    //
    // 这与被它取代的 renderErrorCategory() 是同一个形状:程序级访问器 + 函数内静态。

    [[nodiscard]] LUX_FUNCTION_PUBLIC RenderErrorRegistry& renderErrorRegistry() noexcept;

    /// 取 T 的 id。首次调用登记,之后是一次带守卫的静态读 —— 每帧路径上也可以直接用。
    template <ErrorType T> [[nodiscard]] ErrorTypeId errorTypeOf() noexcept
    {
        static const ErrorTypeId id = renderErrorRegistry().errorType<T>();
        return id;
    }

    /// 组装一个失败值。实参按位置对应 T 声明的 args 槽。
    template <ErrorType T>
    [[nodiscard]] RenderError
    renderError(std::uint32_t arg0 = 0, std::uint32_t arg1 = 0, std::uint32_t arg2 = 0) noexcept
    {
        return makeError(errorTypeOf<T>(), arg0, arg1, arg2);
    }

    /// 失败点最常见的形状:`return renderFailure<err::asset::Invalid>();`
    /// 返回 unexpected<RenderError>,可隐式转成任意 Expected<T>。
    template <ErrorType T>
    [[nodiscard]] auto renderFailure(std::uint32_t arg0 = 0, std::uint32_t arg1 = 0, std::uint32_t arg2 = 0) noexcept
    {
        // 模板实参写显式而不是靠 CTAD:lux::cxx::unexpected 是**别名模板**
        // (using unexpected = std::unexpected<T>),而别名模板的类模板实参推导
        // 是 C++20 特性,libclang 的解析器不支持它 —— 编译器全都接受,只有
        // meta_generator 解析到这个头时会报
        //   "alias template 'unexpected' requires template arguments"
        // 且**不影响退出码**,于是表现为反射静默少生成而构建照常成功。
        return lux::cxx::unexpected<RenderError>(renderError<T>(arg0, arg1, arg2));
    }

    /// 这个失败是不是 T?用于把内部错误映射回线协议状态码那类分诊。
    template <ErrorType T> [[nodiscard]] bool isError(const RenderError& error) noexcept
    {
        return error.type == errorTypeOf<T>();
    }

    namespace detail
    {
        /// 编译期两两比对 LUX_RENDER_ERROR_LIST 里的 name。
        [[nodiscard]] constexpr bool engineErrorNamesUnique() noexcept
        {
            constexpr std::string_view names[] = {
#define LUX_RENDER_ERROR_NAME_ENTRY(T) T::name,
                LUX_RENDER_ERROR_LIST(LUX_RENDER_ERROR_NAME_ENTRY)
#undef LUX_RENDER_ERROR_NAME_ENTRY
            };
            for (std::size_t i = 0; i < std::size(names); ++i)
                for (std::size_t j = i + 1; j < std::size(names); ++j)
                    if (names[i] == names[j])
                        return false;
            return true;
        }
    } // namespace detail

    static_assert(detail::engineErrorNamesUnique(), "两个错误类型声明了同一个 name —— 见 RenderErrorList.hpp");

} // namespace lux::render
