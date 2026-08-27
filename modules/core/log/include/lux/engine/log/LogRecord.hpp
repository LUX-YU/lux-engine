#pragma once
/**
 * @file LogRecord.hpp
 * @brief 定长延迟格式化日志记录(统一事件系统 §7.5-D,档2)。
 *
 * 调用点**不格式化**:把「格式串指针 + 参数字节 + 解码函数指针」打进一条
 * 定长记录,格式化推迟到消费端(formatRecord,在 Log.cpp)。稳态零堆分配的
 * 四个支点:
 *   ① 格式串零拷贝 —— `std::format_string` 钉住编译期字面量,只存指针;
 *   ② 参数打包在栈上一次 memcpy 完成 —— 无 string/variant/类型擦除堆盒;
 *      解码靠调用点模板实例化出的静态函数指针(每调用点一个,零运行期注册);
 *   ③ 队列内核(moodycamel)的块池承接定长记录内存;
 *   ④ 超长参数截断而非溢出堆(truncated 标记)—— 日志不是数据管道,
 *      「记录定长」正是让全链零分配成立的前提。
 *
 * 头内只有 memcpy 级打包与 `std::make_format_args`(纯指针打包)——
 * **不引入格式化代码路径**(Log.hpp 文件头记录的 57× 目标文件膨胀实测)。
 *
 * LogRecord 是纯数据 struct:log 模块定义它**不等于** log 依赖事件系统 ——
 * 发布发生在宿主组装层注入的单出口回调里(方案B,见 Log.hpp)。
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::log
{
    enum class ELevel : std::uint8_t
    {
        Trace = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
    };

    struct LogRecord;

    /// 消费端回调:解码器重建实参后经它交出 `std::format_args`(实参活在
    /// 解码器栈上,只在本次调用内有效)。
    using EmitArgsFn = void (*)(const LogRecord&, std::format_args, void*);
    /// 调用点实例化的解码器:从记录字节区重建实参 → emit。
    using DecodeFn = void (*)(const LogRecord&, EmitArgsFn, void*);

    /// 参数区容量:绝大多数日志实参 < 200B;超长走截断(见文件头④)。
    inline constexpr std::size_t kLogArgCapacity = 232;

    /// 定长记录(≈288B):头部全部无所有权 —— format/category 是静态存储期
    /// 字面量,decode 是静态函数指针;跨线程/延迟携带安全。
    struct LogRecord
    {
        const char* format;   ///< 格式串字面量(fmt 风格)
        const char* category; ///< 静态模块 tag("render"/"asset"/…)
        DecodeFn decode;      ///< 重建 format_args 的解码器
        std::uint64_t seq;    ///< 全局原子自增(跨线程稳定排序键)
        std::int64_t ts_ns;   ///< steady_clock ns
        std::uint32_t tid;
        std::uint32_t format_len;
        std::uint16_t arg_bytes;
        ELevel level;
        bool truncated; ///< 参数区放不下,尾部实参不完整
        std::byte args[kLogArgCapacity];
    };

    namespace detail
    {
        // ── 参数分类:字符串族转「u16 长度前缀 + 字节」,平凡拷贝类型直拷,
        //    其余编译期拒绝(调用点显式转换:.string() / static_cast / …)。──

        template <class D>
        inline constexpr bool is_string_like = std::is_same_v<D, std::string> || std::is_same_v<D, std::string_view> ||
                                               std::is_same_v<D, const char*> || std::is_same_v<D, char*>;

        template <class D> [[nodiscard]] inline std::string_view asStringView(const D& v) noexcept
        {
            if constexpr (std::is_same_v<D, const char*> || std::is_same_v<D, char*>)
                return v ? std::string_view(v) : std::string_view();
            else
                return std::string_view(v);
        }

        inline void packBytes(LogRecord& r, const void* p, std::size_t n) noexcept
        {
            if (n > kLogArgCapacity - r.arg_bytes)
            {
                r.truncated = true;
                return;
            }
            std::memcpy(r.args + r.arg_bytes, p, n);
            r.arg_bytes += static_cast<std::uint16_t>(n);
        }

        inline void packString(LogRecord& r, std::string_view s) noexcept
        {
            const std::size_t avail = kLogArgCapacity - r.arg_bytes;
            if (avail < sizeof(std::uint16_t))
            {
                r.truncated = true;
                return;
            }
            std::size_t n = s.size();
            if (n > avail - sizeof(std::uint16_t))
            {
                n = avail - sizeof(std::uint16_t);
                r.truncated = true;
            }
            const auto len = static_cast<std::uint16_t>(n);
            std::memcpy(r.args + r.arg_bytes, &len, sizeof(len));
            if (n != 0)
                std::memcpy(r.args + r.arg_bytes + sizeof(len), s.data(), n);
            r.arg_bytes += static_cast<std::uint16_t>(sizeof(len) + n);
        }

        template <class D> void packOne(LogRecord& r, const D& v) noexcept
        {
            if constexpr (is_string_like<D>)
                packString(r, asStringView(v));
            else if constexpr (std::is_trivially_copyable_v<D>)
                packBytes(r, &v, sizeof(D));
            else
                static_assert(
                    is_string_like<D>,
                    "unsupported log argument type — pass it "
                    "pre-formatted (.string(), static_cast, ...)");
        }

        /// 解码后的存储形态:字符串族一律 string_view(指进记录字节区,记录
        /// 在解码期间存活);其余原型还原。
        template <class D> using ArgStorage = std::conditional_t<is_string_like<D>, std::string_view, D>;

        template <class D> [[nodiscard]] ArgStorage<D> unpackOne(const LogRecord& r, std::size_t& off) noexcept
        {
            if constexpr (is_string_like<D>)
            {
                std::uint16_t len{};
                if (off + sizeof(len) > r.arg_bytes)
                {
                    off = r.arg_bytes;
                    return {};
                }
                std::memcpy(&len, r.args + off, sizeof(len));
                off += sizeof(len);
                if (off + len > r.arg_bytes)
                {
                    off = r.arg_bytes;
                    return {};
                }
                const std::string_view v(reinterpret_cast<const char*>(r.args + off), len);
                off += len;
                return v;
            }
            else
            {
                D v{};
                if (off + sizeof(D) > r.arg_bytes)
                {
                    off = r.arg_bytes;
                    return v;
                }
                std::memcpy(&v, r.args + off, sizeof(D));
                off += sizeof(D);
                return v;
            }
        }

        /// 每个调用点实例化一份:pack 写入序 == decode 读回序,由同一模板钉死。
        template <class... Ds> struct Codec
        {
            static void pack(LogRecord& r, const Ds&... vs) noexcept
            {
                (packOne<Ds>(r, vs), ...);
            }

            static void decode(const LogRecord& r, EmitArgsFn emit, void* user)
            {
                std::size_t off = 0;
                // 花括号列表初始化保证从左到右求值(与 pack 的折叠序一致)。
                std::tuple<ArgStorage<Ds>...> vals{unpackOne<Ds>(r, off)...};
                std::apply([&](auto&... vs) { emit(r, std::make_format_args(vs...), user); }, vals);
            }
        };

        template <> struct Codec<>
        {
            static void pack(LogRecord&) noexcept
            {
            }
            static void decode(const LogRecord& r, EmitArgsFn emit, void* user)
            {
                // format_args 无默认构造(MSVC):零实参也经 make_format_args。
                emit(r, std::make_format_args(), user);
            }
        };
    } // namespace detail
} // namespace lux::log
