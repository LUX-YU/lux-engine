#pragma once

#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace lux::ui::detail
{
    class NullTerminatedText final
    {
    public:
        static constexpr std::size_t kLocalCapacity = 256U;

        explicit NullTerminatedText(std::string_view value)
        {
            if (value.size() < local_.size())
            {
                if (!value.empty())
                    std::memcpy(local_.data(), value.data(), value.size());
                local_[value.size()] = '\0';
                data_ = local_.data();
                return;
            }
            heap_.assign(value.data(), value.size());
            data_ = heap_.c_str();
        }

        [[nodiscard]] const char* c_str() const noexcept { return data_; }

    private:
        std::array<char, kLocalCapacity> local_{};
        std::string heap_;
        const char* data_{local_.data()};
    };

    [[nodiscard]] inline const char* dataOrEmpty(std::string_view value) noexcept
    {
        static constexpr char empty[] = "";
        return value.empty() ? empty : value.data();
    }
} // namespace lux::ui::detail
