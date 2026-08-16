#pragma once
/**
 * @file RenderError.hpp
 * @brief 渲染层的失败值:一个错误类型句柄 + 三个实参槽。
 *
 * 一次失败分成两半:
 *   - **静态的一半**(名字、消息模板、恢复建议、每个实参槽的语义)登记在
 *     RenderErrorRegistry 里,由 ErrorTypeId 索引,不随错误值搬运;
 *   - **本次发生的一半**只有三个 uint32,就在错误值里。
 *
 * 三个槽够用,是因为「期望值」总是契约的纯函数 —— 消费侧拿着契约索引自己就能算出
 * 「应该是什么」,只有「实际是什么」需要随错误传递。所以某个错误若需要四个以上
 * 实参,先检查是不是有一部分本该由契约推出来。
 *
 * 整体 20 字节、trivially copyable,于是同一个类型可以同时充当:
 *   - 渲染层 Expected<T> 的失败态
 *   - comm 回复里的 error 字段(直接 memcpy,两端之间没有翻译层)
 *   - 渲染线程自发上报事件的载荷
 *
 * 错误里**从不搬运字符串**:凡是「哪个资源 / 哪个槽 / 哪个格式」,一律传共享表里的
 * 索引或枚举值,由 EErrorArg 标注该槽查哪张表,消费侧解析成名字。两端读的是同一份
 * 引擎常量表,所以名字不需要过线。
 */

#include <lux/cxx/container/SlotMap.hpp>   // lux::cxx::SlotKey

#include <array>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::render
{
    struct ErrorTypeTag {};

    /// 错误类型句柄(index + generation),与 FeatureHandle / ViewHandle 同一族:
    /// 8 字节、trivially copyable、跨 comm 通道直接序列化。generation 保证动态
    /// feature 卸载后被回收的槽位不会让一个陈旧的 id 静默命中新类型。
    using ErrorTypeId = lux::cxx::SlotKey<ErrorTypeTag>;

    /// 调用方拿到这个错误之后**能做什么**。声明在错误类型上,不进错误值 ——
    /// 一个不认识该错误类型的通用客户端(编辑器 / 门禁)靠它就能决定如何反应。
    enum class ERecovery : std::uint8_t
    {
        Permanent,   ///< 同样的输入再来一次还是错,别重试
        Retryable,   ///< 瞬时:池满、环满、本帧资源不够;稍后重试可能成功
        NeedsInput,  ///< 需要改输入:重烘焙资产、改配置、换 feature 组合
        Bug,         ///< 引擎内部契约被破坏,该上报而不该由用户处理
    };

    /// 一个实参槽的解释方式。新增一种索引类型时,同时在 formatRenderError 里补上它的
    /// 解析分支 —— 枚举与解析能力一起长,不允许出现「声明了但格式化器不认识」的槽。
    enum class EErrorArg : std::uint8_t
    {
        None = 0,          ///< 该槽未使用
        Uint,              ///< 十进制计数:个数、容量、序号、句柄索引
        Hex,               ///< 十六进制:usage / flag 掩码
        VkResult,          ///< VkResult 的位模式(见 encodeVkResult)
        VkFormat,          ///< VkFormat 的数值
        BuiltinShader,     ///< EBuiltinShader 枚举值 → 内置着色器名
        LogicalResource,   ///< rdesc::logicalResourceIndex 的下标 → 契约资源名 + 它的规范位置
        DescriptorSlot,    ///< EDescriptorSetSlot → 引擎描述符槽名
        BindFrequency,     ///< rdesc::EBindFrequency → 域名
        /// `FeatureTypeId`(featureId() 的 FNV-1a 哈希)**的低 32 位**。
        ///
        /// 截断是实参槽只有 32 位所致。之所以仍然值得带上:这类错误说的是"另一个
        /// 特性"(与你冲突的那个、你缺的那个依赖、还在依赖你的那个),不带就只剩
        /// 一句"有冲突"。消费侧把自己已知的名字过一遍 featureId() 取低 32 位比对
        /// 即可还原是谁 —— 一个场景里几十个特性,低 32 位撞车的概率可以忽略。
        FeatureType,
        /// 编译图里 `original_graph.resources[]` 的下标。
        ///
        /// 图资源的名字是**用户自起的**(某个特性 create/import 时给的),既不是
        /// 契约条目也没有稳定编号,装不进 32 位实参槽。但下标是可解析的:客户端用
        /// `DumpRenderGraph` 拿到这张图的文本转储,按下标即可查回名字与描述。
        /// 换句话说,句柄指向的是一个**客户端本来就能取到**的东西。
        GraphResource,
        /// 同上,`original_graph.passes[]` 的下标。
        GraphPass,
    };

    /// 取 `FeatureTypeId` 中能装进实参槽的那一半(见 EErrorArg::FeatureType)。
    [[nodiscard]] constexpr std::uint32_t encodeFeatureType(std::uint64_t type) noexcept
    {
        return static_cast<std::uint32_t>(type & 0xFFFF'FFFFull);
    }

    inline constexpr std::size_t kErrorArgCount = 3;

    using ErrorArgs = std::array<EErrorArg, kErrorArgCount>;

    /// 一次失败。
    struct RenderError
    {
        ErrorTypeId                              type{};
        std::array<std::uint32_t, kErrorArgCount> args{};

        /// 无错误。默认构造的 RenderError 表示成功。
        [[nodiscard]] constexpr bool ok() const noexcept { return type.isNull(); }
    };

    static_assert(sizeof(RenderError) == 20);
    static_assert(std::is_trivially_copyable_v<RenderError>);

    /// 组装一个失败值。实参按位置对应错误类型声明的 args 槽。
    [[nodiscard]] constexpr RenderError makeError(ErrorTypeId   type,
                                                  std::uint32_t arg0 = 0,
                                                  std::uint32_t arg1 = 0,
                                                  std::uint32_t arg2 = 0) noexcept
    {
        return RenderError{type, {arg0, arg1, arg2}};
    }

    /// VkResult 是有符号枚举,直接 static_cast 到 uint32_t 会把错误码变成巨大的正数。
    /// 走这个函数保留位模式,格式化器再按 int32 读回来。
    /// (声明在这里是为了让失败点有一处统一入口;它不需要 vulkan.h。)
    [[nodiscard]] constexpr std::uint32_t encodeVkResult(std::int32_t result) noexcept
    {
        return static_cast<std::uint32_t>(result);
    }

    /// 注册表里保存的错误类型描述。三个指针指向静态字面量,所以整体 trivially
    /// copyable —— 查询返回拷贝而不是指针,调用方不必关心注册表内部的存储重排。
    struct ErrorTypeDesc
    {
        const char* name{nullptr};      ///< 跨会话稳定的标识,点分层级,如 "memory.out_of_memory"
        const char* message{nullptr};   ///< 人读消息模板,{0}/{1}/{2} 引用实参槽
        ERecovery   recovery{ERecovery::Permanent};
        ErrorArgs   args{};             ///< 每个实参槽的语义
    };

    static_assert(std::is_trivially_copyable_v<ErrorTypeDesc>);

    namespace detail
    {
        /// message 里被引用到的实参槽位掩码:出现 "{0}" 就置 bit 0,以此类推。
        [[nodiscard]] constexpr std::uint32_t errorMessageArgMask(std::string_view message) noexcept
        {
            std::uint32_t mask = 0;
            for (std::size_t i = 0; i + 2 < message.size(); ++i)
            {
                if (message[i] != '{' || message[i + 2] != '}')
                    continue;
                const char digit = message[i + 1];
                if (digit < '0' || digit > '9')
                    continue;
                mask |= 1u << static_cast<unsigned>(digit - '0');
            }
            return mask;
        }

        /// args 里声明为已用的槽位掩码。
        [[nodiscard]] constexpr std::uint32_t errorDeclaredArgMask(const ErrorArgs& args) noexcept
        {
            std::uint32_t mask = 0;
            for (std::size_t i = 0; i < args.size(); ++i)
                if (args[i] != EErrorArg::None)
                    mask |= 1u << i;
            return mask;
        }
    } // namespace detail

    /// 一个错误类型 = 一个空类型 + 四个 static constexpr 成员。
    ///
    /// 最后一条约束是**消息占位符与实参槽必须逐一对应**:多写一个 {n}、或者声明了一槽
    /// 却没在消息里用,都在 errorType<T>() 处编译失败,而不是在运行期打出一条含未替换
    /// "{2}" 的消息。
    template<typename T>
    concept ErrorType =
        std::is_empty_v<T>
        && requires {
            { T::name }     -> std::convertible_to<const char*>;
            { T::message }  -> std::convertible_to<const char*>;
            { T::recovery } -> std::convertible_to<ERecovery>;
            { T::args }     -> std::convertible_to<ErrorArgs>;
        }
        && (detail::errorMessageArgMask(T::message) == detail::errorDeclaredArgMask(T::args));

    /// 从错误类型取出它的描述。
    template<ErrorType T>
    [[nodiscard]] constexpr ErrorTypeDesc errorTypeDesc() noexcept
    {
        return ErrorTypeDesc{T::name, T::message, T::recovery, T::args};
    }

} // namespace lux::render
