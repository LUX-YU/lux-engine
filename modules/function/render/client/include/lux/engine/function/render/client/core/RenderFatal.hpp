#pragma once
/**
 * @file RenderFatal.hpp
 * @brief 不变量被破坏时的立即终止。
 *
 * 与 assert 的区别:**所有构建配置都在**。assert 在 NDEBUG 下整条消失,而编辑器实机
 * 跑的正是带 NDEBUG 的 RelWithDebInfo —— 用 assert 守一条结构不变量,等于在真正发布的
 * 配置里没有守卫。
 *
 * 与抛异常的区别:渲染线程上没有人接,栈展开只会让现场比 abort 更难查。
 *
 * 用它的判据很窄:**继续执行会产生未定义行为**(不是「画面不对」)。凡是调用方还能
 * 做点别的、或者程序还能有意义地跑下去的情形,都应该返回 Expected 而不是终止。
 */

#include <lux/engine/function/visibility.h>

#include <source_location>
#include <string_view>

namespace lux::render
{
    /// 打印 what + 调用点,然后 std::abort()。
    /// 位置由 std::source_location 自动带上,调用点不需要写宏。
    [[noreturn]] LUX_FUNCTION_PUBLIC void
    renderFatal(std::string_view what, const std::source_location& where = std::source_location::current()) noexcept;

} // namespace lux::render
