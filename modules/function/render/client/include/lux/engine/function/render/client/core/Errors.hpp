#pragma once
/**
 * @file Errors.hpp
 * @brief 渲染层统一的 Expected<T>。
 *
 * 失败态是 RenderError(错误类型句柄 + 三个实参槽,20 字节 POD),而不是
 * std::error_code —— 后者带一个指向本进程静态对象的 error_category*,塞不进
 * trivially-copyable 的 comm 载荷,于是每个 handler 都只能把它压成一个 uint32,
 * 错误信息在跨线程那一层被抹平。RenderError 直接 memcpy 过线,没有可衰减的地方。
 *
 * 失败点的写法(见 RenderErrorRegistry.hpp):
 *   return renderFailure<err::asset::Invalid>();
 *   return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(res));
 *
 * 分诊的写法:
 *   if (isError<err::asset::UnsupportedFormat>(r.error())) ...
 */

#include <lux/engine/function/render/client/core/RenderError.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>

#include <lux/cxx/compile_time/expected.hpp>

namespace lux::render
{
    template<typename T>
    using Expected = lux::cxx::expected<T, RenderError>;
}
