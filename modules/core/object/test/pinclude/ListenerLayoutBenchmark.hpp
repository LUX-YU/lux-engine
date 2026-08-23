#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lux::object::test::benchmark
{
    struct ListenerControl final
    {
        std::uint64_t* observed{nullptr};
        void (*invoke)(std::uint64_t*, int) noexcept{nullptr};
        bool connected{true};
    };

    inline void accumulate(std::uint64_t* observed, int value) noexcept
    {
        *observed += static_cast<std::uint64_t>(value);
    }

    class CandidateA final
    {
    public:
        void append(ListenerControl& control) { direct_.push_back(&control); }
        void pop() { direct_.pop_back(); }

        void notify(int value) noexcept
        {
            for (auto* control : direct_)
            {
                if (control->connected)
                    control->invoke(control->observed, value);
            }
        }

    private:
        std::vector<ListenerControl*> direct_;
    };

    struct DirectListener final
    {
        ListenerControl* control{nullptr};
        std::uint64_t* observed{nullptr};
        void (*invoke)(std::uint64_t*, int) noexcept{nullptr};
    };

    class CandidateB final
    {
    public:
        void append(ListenerControl& control)
        {
            direct_.push_back({&control, control.observed, control.invoke});
        }

        void pop() { direct_.pop_back(); }

        void notify(int value) noexcept
        {
            for (const auto& listener : direct_)
            {
                if (listener.control->connected)
                    listener.invoke(listener.observed, value);
            }
        }

    private:
        std::vector<DirectListener> direct_;
    };

    inline constexpr std::size_t kCandidateBExtraBytesPerConnection =
        sizeof(DirectListener) - sizeof(ListenerControl*);
} // namespace lux::object::test::benchmark
