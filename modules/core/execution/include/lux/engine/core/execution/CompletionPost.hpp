#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <stdexec/execution.hpp>
#include <utility>

namespace lux::execution
{
    template<class Error> using CpuResume = void (*)(void*, cxx::expected<void, Error>) noexcept;

    // Borrowed notification into the owner's pre-acquired completion landing place. Publication is the
    // producer's final access; only the owner-thread event consumer may resume or reclaim the operation.
    template<class Error> struct CompletionPost final
    {
        void* owner = nullptr;
        void (*publish)(void*, void*, CpuResume<Error>, cxx::expected<void, Error>) noexcept = nullptr;

        void operator()(void* continuation, CpuResume<Error> resume, cxx::expected<void, Error> result) const noexcept
        {
            publish(owner, continuation, resume, std::move(result));
        }

        [[nodiscard]] explicit operator bool() const noexcept { return publish != nullptr; }
    };

    // Finite operations borrow the existing owner's continuation landing place.
    // Queries forward through Sender adapters; they allocate neither a scheduler nor a queue.
    struct GetCompletionPost final : stdexec::forwarding_query_t
    {
        template <class Env> [[nodiscard]] auto operator()(const Env& env) const noexcept
        { return env.query(*this); }
    };
    inline constexpr GetCompletionPost getCompletionPost{};

}
