#include <lux/engine/input/InputContext.hpp>
#include <lux/engine/input/InputContextStack.hpp>

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

namespace lux::input::detail
{
    struct InputContextStackState final
    {
        InputContextStackState() noexcept = default;
        InputContextStackState(const InputContextStackState&) = delete;
        InputContextStackState& operator=(const InputContextStackState&) = delete;
        InputContextStackState(InputContextStackState&&) = delete;
        InputContextStackState& operator=(InputContextStackState&&) = delete;

        std::vector<InputActivation*> entries;
    };

    // Stable allocation owned solely by the public activation. These links are
    // non-owning indexes; revoke removes both before either endpoint can disappear.
    struct InputActivation final
    {
        InputActivation(InputContext& target, std::shared_ptr<InputContextStackState> owner, int order) noexcept
            : context(&target), state(std::move(owner)), priority(order)
        {
            const auto position = std::upper_bound(
                state->entries.begin(),
                state->entries.end(),
                priority,
                [](int value, const InputActivation* entry) noexcept { return value < entry->priority; }
            );
            state->entries.insert(position, this);
            next = context->activation_head_;
            if (next)
            {
                next->previous = this;
            }
            context->activation_head_ = this;
        }

        ~InputActivation() noexcept
        {
            revoke();
        }

        InputActivation(const InputActivation&) = delete;
        InputActivation& operator=(const InputActivation&) = delete;
        InputActivation(InputActivation&&) = delete;
        InputActivation& operator=(InputActivation&&) = delete;

        void revoke() noexcept
        {
            if (!context)
            {
                return;
            }
            if (previous)
            {
                previous->next = next;
            }
            else
            {
                context->activation_head_ = next;
            }
            if (next)
            {
                next->previous = previous;
            }
            std::erase(state->entries, this);
            context = nullptr;
            previous = nullptr;
            next = nullptr;
            state.reset();
        }

        InputContext* context;
        std::shared_ptr<InputContextStackState> state;
        const int priority;
        InputActivation* previous{};
        InputActivation* next{};
    };
} // namespace lux::input::detail

namespace lux::input
{
    InputContext::InputContext(std::string name, bool consumes_keyboard, bool consumes_mouse) noexcept
        : name_(std::move(name)), consumes_keyboard_(consumes_keyboard), consumes_mouse_(consumes_mouse)
    {
    }

    InputContext::~InputContext() noexcept
    {
        while (activation_head_)
        {
            activation_head_->revoke();
        }
    }

    InputContextActivation::InputContextActivation() noexcept = default;

    InputContextActivation::InputContextActivation(std::unique_ptr<detail::InputActivation> activation) noexcept
        : activation_(std::move(activation))
    {
    }

    InputContextActivation::~InputContextActivation() noexcept = default;

    InputContextActivation::InputContextActivation(InputContextActivation&&) noexcept = default;

    InputContextActivation& InputContextActivation::operator=(InputContextActivation&& other) noexcept
    {
        if (this != &other)
        {
            activation_ = std::move(other.activation_);
        }
        return *this;
    }

    InputContextActivation::operator bool() const noexcept
    {
        return activation_ && activation_->context;
    }

    InputContextStack::InputContextStack() noexcept : state_(std::make_shared<detail::InputContextStackState>()) {}

    InputContextStack::~InputContextStack() noexcept
    {
        while (!state_->entries.empty())
        {
            state_->entries.back()->revoke();
        }
    }

    ActivationResult InputContextStack::activate(InputContext& context, int priority) noexcept
    {
        if (contains(&context))
        {
            return lux::cxx::unexpected(EInputActivationError::DUPLICATE_CONTEXT);
        }
        return InputContextActivation(std::make_unique<detail::InputActivation>(context, state_, priority));
    }

    bool InputContextStack::contains(const InputContext* context) const noexcept
    {
        return std::ranges::any_of(
            state_->entries,
            [context](const detail::InputActivation* entry) noexcept { return entry->context == context; }
        );
    }

    InputContext* InputContextStack::top() noexcept
    {
        return state_->entries.empty() ? nullptr : state_->entries.back()->context;
    }

    const InputContext* InputContextStack::top() const noexcept
    {
        return state_->entries.empty() ? nullptr : state_->entries.back()->context;
    }

    bool InputContextStack::empty() const noexcept
    {
        return state_->entries.empty();
    }

    std::size_t InputContextStack::size() const noexcept
    {
        return state_->entries.size();
    }

    InputContext& InputContextStack::operator[](std::size_t index) noexcept
    {
        return const_cast<InputContext&>(std::as_const(*this)[index]);
    }

    const InputContext& InputContextStack::operator[](std::size_t index) const noexcept
    {
        if (index >= state_->entries.size())
        {
            std::terminate();
        }
        return *state_->entries[index]->context;
    }
} // namespace lux::input
