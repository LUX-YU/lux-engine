#pragma once
/**
 * @file InlineRecord.hpp
 * @brief 定长内联记录 —— 高频事件的载荷打包工具(设计稿 §7.5-D)。
 *
 * 事件按值走队列;高频通道(日志是首个用户:LogRecord)把变长参数打进
 * 定长字节区,格式化/解码推迟到订阅端。关键取舍:**截断而不溢出** ——
 * 超出容量置 truncated 标记,换来「记录定长」这个让全链稳态零堆分配成立
 * 的前提(moodycamel 的块池只对定长 T 免堆)。
 *
 * 总线不强制使用:低频领域事件继续按值直存字段,简单优先。
 *
 * 布局约定:调用方自行决定字节区内容的编排(如「POD 直拷 + 字符串带 u16
 * 长度前缀」),InlineRecordReader 按同样的顺序读回 —— 写读顺序一致性由
 * 使用方保证(日志场景由调用点模板实例化出的 decode 函数指针钉死)。
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace lux::events
{
    template <std::size_t Capacity>
    class InlineRecord
    {
        static_assert(Capacity > 0 && Capacity <= 0xFFFF,
                      "size field is u16; keep records small — this is an "
                      "inline-args buffer, not a data pipe");

    public:
        static constexpr std::size_t kCapacity = Capacity;

        [[nodiscard]] std::size_t      size() const noexcept { return size_; }
        [[nodiscard]] bool             truncated() const noexcept { return truncated_; }
        [[nodiscard]] const std::byte* data() const noexcept { return bytes_; }

        /// 平凡拷贝类型:整块 memcpy。放不下 → 置截断标记并拒收(false)。
        template <class T>
            requires std::is_trivially_copyable_v<T>
        bool pack(const T& value) noexcept
        {
            return packBytes(&value, sizeof(T));
        }

        bool packBytes(const void* p, std::size_t n) noexcept
        {
            if (n > Capacity - size_)
            {
                truncated_ = true;
                return false;
            }
            std::memcpy(bytes_ + size_, p, n);
            size_ += static_cast<std::uint16_t>(n);
            return true;
        }

        /// 字符串:u16 长度前缀 + 字节。超长截断本体(前缀 = 实际写入数,
        /// 读侧无需感知截断即可正确前进),置标记,返回 false。
        bool packString(std::string_view s) noexcept
        {
            const std::size_t avail = Capacity - size_;
            if (avail < sizeof(std::uint16_t))
            {
                truncated_ = true;
                return false;
            }
            std::size_t n = s.size();
            if (n > avail - sizeof(std::uint16_t))
            {
                n          = avail - sizeof(std::uint16_t);
                truncated_ = true;
            }
            const auto len = static_cast<std::uint16_t>(n);
            std::memcpy(bytes_ + size_, &len, sizeof(len));
            if (n != 0)
                std::memcpy(bytes_ + size_ + sizeof(len), s.data(), n);
            size_ += static_cast<std::uint16_t>(sizeof(len) + n);
            return !truncated_;
        }

    private:
        std::uint16_t size_{0};
        bool          truncated_{false};
        std::byte     bytes_[Capacity];
    };

    /// 顺序读回:与写入同序。unpack 失败(记录被截断)返回 false,
    /// 调用方就地停止 —— 半条参数不是可用数据。
    class InlineRecordReader
    {
    public:
        InlineRecordReader(const std::byte* data, std::size_t size) noexcept
            : p_(data), end_(data + size)
        {
        }

        template <std::size_t N>
        explicit InlineRecordReader(const InlineRecord<N>& r) noexcept
            : InlineRecordReader(r.data(), r.size())
        {
        }

        template <class T>
            requires std::is_trivially_copyable_v<T>
        bool unpack(T& out) noexcept
        {
            if (remaining() < sizeof(T))
                return false;
            std::memcpy(&out, p_, sizeof(T));
            p_ += sizeof(T);
            return true;
        }

        /// 返回的 view 指进记录字节区 —— 只在记录存活期内有效。
        bool unpackString(std::string_view& out) noexcept
        {
            std::uint16_t len{};
            if (!unpack(len))
                return false;
            if (remaining() < len)
                return false;
            out = std::string_view(reinterpret_cast<const char*>(p_), len);
            p_ += len;
            return true;
        }

        [[nodiscard]] std::size_t remaining() const noexcept
        {
            return static_cast<std::size_t>(end_ - p_);
        }

    private:
        const std::byte* p_;
        const std::byte* end_;
    };
} // namespace lux::events
