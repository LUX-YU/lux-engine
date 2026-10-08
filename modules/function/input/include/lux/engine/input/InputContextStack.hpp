#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::input
{
    class InputContext;

    namespace detail
    {
        struct InputActivation;
        struct InputContextStackState;
    } // namespace detail

    enum class EInputActivationError : std::uint8_t
    {
        DUPLICATE_CONTEXT
    };

    // One activation owns membership; neither stack nor context is kept alive.
    // Either endpoint may die first, making the remaining activation empty.
    // Destruction and all access use the same thread as Input evaluation.
    class LUX_FUNCTION_PUBLIC InputContextActivation final
    {
    public:
        InputContextActivation() noexcept;
        ~InputContextActivation() noexcept;
        InputContextActivation(InputContextActivation&&) noexcept;
        InputContextActivation& operator=(InputContextActivation&&) noexcept;
        InputContextActivation(const InputContextActivation&) = delete;
        InputContextActivation& operator=(const InputContextActivation&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class InputContextStack;

        explicit InputContextActivation(std::unique_ptr<detail::InputActivation> activation) noexcept;

        std::unique_ptr<detail::InputActivation> activation_;
    };

    using ActivationResult = lux::cxx::expected<InputContextActivation, EInputActivationError>;

    // Non-owning action order. Priority belongs to the activation and stays fixed;
    // equal-priority activations preserve insertion order, with the newest evaluated first.
    // Returned context borrows last only until the next membership change.
    class LUX_FUNCTION_PUBLIC InputContextStack final
    {
    public:
        InputContextStack() noexcept;
        ~InputContextStack() noexcept;
        InputContextStack(const InputContextStack&) = delete;
        InputContextStack& operator=(const InputContextStack&) = delete;
        InputContextStack(InputContextStack&&) = delete;
        InputContextStack& operator=(InputContextStack&&) = delete;

        [[nodiscard]] ActivationResult activate(InputContext& context, int priority = 0) noexcept;
        [[nodiscard]] bool contains(const InputContext* context) const noexcept;
        [[nodiscard]] InputContext* top() noexcept;
        [[nodiscard]] const InputContext* top() const noexcept;
        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

        // Ordered from lowest to highest priority. index must be less than size().
        [[nodiscard]] InputContext& operator[](std::size_t index) noexcept;
        [[nodiscard]] const InputContext& operator[](std::size_t index) const noexcept;

    private:
        std::shared_ptr<detail::InputContextStackState> state_;
    };
} // namespace lux::input
