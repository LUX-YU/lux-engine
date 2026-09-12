#include <lux/engine/editor/ui/actions/ActiveEditHistory.hpp>

#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace lux::editor::ui
{
    namespace detail
    {
        struct ActiveEditHistoryControl final
        {
            const std::thread::id owner{std::this_thread::get_id()};
            std::atomic<ActiveEditHistory *> router{};
        };
    } // namespace detail
    namespace
    {
        using namespace editing;
        std::atomic<std::uint64_t> router_identity{0U};
        constexpr auto kMaxCounter = (std::numeric_limits<std::uint64_t>::max)();

        [[nodiscard]] auto fail(EEditError code) noexcept
        {
            return lux::cxx::unexpected(makeEditFailure(code));
        }
        struct RouteGuard final
        {
            bool &busy;
            explicit RouteGuard(bool &value) noexcept : busy(value)
            {
                busy = true;
            }
            ~RouteGuard() noexcept
            {
                busy = false;
            }
        };
        [[nodiscard]] EEditError unavailable(EHistoryActionAvailability availability, EHistoryAction action) noexcept
        {
            switch (availability)
            {
            case EHistoryActionAvailability::EMPTY:
                return action == EHistoryAction::UNDO ? EEditError::NO_UNDO : EEditError::NO_REDO;
            case EHistoryActionAvailability::BUSY:
                return EEditError::BUSY;
            case EHistoryActionAvailability::BLOCKED:
                return EEditError::BLOCKED_BY_HOST;
            case EHistoryActionAvailability::CLOSED:
                return EEditError::CLOSED;
            default:
                return EEditError::CONTRACT_VIOLATION;
            }
        }
    } // namespace

    struct ActiveEditHistory::Impl final
    {
        struct Registration final
        {
            EditHistoryTarget *target{};
            HistoryId history;
            HistoryTargetHandle handle;
            std::size_t references{1};
        };
        const std::thread::id owner{std::this_thread::get_id()};
        std::shared_ptr<detail::ActiveEditHistoryControl> control;
        std::vector<Registration> registrations;
        std::uint64_t identity{}, next_registration{};
        HistoryTargetHandle active;
        mutable bool busy{};
        bool closed{};

        [[nodiscard]] EditResult<void> check() const noexcept
        {
            if (owner != std::this_thread::get_id())
            {
                return fail(EEditError::WRONG_THREAD);
            }
            if (closed)
            {
                return fail(EEditError::CLOSED);
            }
            if (busy)
            {
                return fail(EEditError::BUSY);
            }
            return {};
        }
        [[nodiscard]] const Registration *find(HistoryTargetHandle handle) const noexcept
        {
            if (!handle.valid())
            {
                return nullptr;
            }
            for (const auto &item : registrations)
            {
                if (item.target != nullptr && item.handle == handle)
                {
                    return &item;
                }
            }
            return nullptr;
        }
        [[nodiscard]] EditResult<HistoryTargetView> targetView(const Registration &item) const noexcept
        {
            if (item.target->historyId() != item.history)
            {
                return fail(EEditError::CONTRACT_VIOLATION);
            }
            auto view = item.target->historyView();
            if (!view)
            {
                return lux::cxx::unexpected(view.error());
            }
            const bool is_wrong_identity = view->history.history != item.history ||
                                           view->history.current.history != item.history ||
                                           !view->history.current.valid();
            if (is_wrong_identity)
            {
                return fail(EEditError::CONTRACT_VIOLATION);
            }
            return view;
        }
    };

    ActiveEditHistory::ActiveEditHistory(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }
    ActiveEditHistory::CreateResult ActiveEditHistory::create(std::size_t target_capacity) noexcept
    {
        const bool is_invalid_capacity =
            target_capacity == 0U || target_capacity > std::vector<Impl::Registration>{}.max_size();
        if (is_invalid_capacity)
        {
            return fail(EEditError::INVALID_LIMITS);
        }
        {
            auto state = std::make_unique<Impl>();
            state->registrations.resize(target_capacity);
            state->control = std::make_shared<detail::ActiveEditHistoryControl>();
            auto result = std::unique_ptr<ActiveEditHistory>(new ActiveEditHistory(std::move(state)));
            auto issued = router_identity.load(std::memory_order_relaxed);
            do
            {
                if (issued == kMaxCounter)
                {
                    return fail(EEditError::ID_EXHAUSTED);
                }
            } while (!router_identity.compare_exchange_weak(issued, issued + 1U, std::memory_order_relaxed));
            result->impl_->identity = issued + 1U;
            result->impl_->control->router = result.get();
            return result;
        }
    }

    ActiveEditHistory::~ActiveEditHistory() noexcept
    {
        if (!close())
        {
            std::abort();
        }
    }

    EditResult<HistoryTargetRegistration> ActiveEditHistory::registerTarget(EditHistoryTarget &target) noexcept
    {
        auto &state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        RouteGuard guard(state.busy);
        for (const auto &item : state.registrations)
        {
            if (item.target == &target)
            {
                return fail(EEditError::DUPLICATE_TARGET);
            }
        }
        const auto history = target.historyId();
        if (!history.valid())
        {
            return fail(EEditError::INVALID_ARGUMENT);
        }
        Impl::Registration *available{};
        for (auto &item : state.registrations)
        {
            if (item.target && item.history == history)
            {
                return fail(EEditError::DUPLICATE_TARGET);
            }
            if (!item.target && !available)
            {
                available = &item;
            }
        }
        if (!available)
        {
            return fail(EEditError::TARGET_CAPACITY);
        }
        const Impl::Registration candidate{&target, history, {}};
        auto view = state.targetView(candidate);
        if (!view)
        {
            return lux::cxx::unexpected(view.error());
        }
        if (view->history.closed)
        {
            return fail(EEditError::CLOSED);
        }
        if (state.next_registration == kMaxCounter)
        {
            return fail(EEditError::ID_EXHAUSTED);
        }
        const HistoryTargetHandle handle{state.identity, ++state.next_registration};
        *available = Impl::Registration{&target, history, handle};
        return HistoryTargetRegistration{state.control, handle};
    }

    EditResult<HistoryTargetRegistration> ActiveEditHistory::retainTarget(EditHistoryTarget &target) noexcept
    {
        auto &state = *impl_;
        if (auto checked = state.check(); !checked)
            return lux::cxx::unexpected(checked.error());
        {
            RouteGuard guard(state.busy);
            for (auto &item : state.registrations)
            {
                if (item.target != &target)
                    continue;
                auto current = state.targetView(item);
                if (!current)
                    return lux::cxx::unexpected(current.error());
                if (current->history.closed)
                    return fail(EEditError::CLOSED);
                if (item.references == (std::numeric_limits<std::size_t>::max)())
                    return fail(EEditError::TARGET_CAPACITY);
                ++item.references;
                return HistoryTargetRegistration{state.control, item.handle};
            }
        }
        return registerTarget(target);
    }

    EditResult<void> ActiveEditHistory::activate(HistoryTargetHandle handle) noexcept
    {
        auto &state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return checked;
        }
        if (!state.find(handle))
        {
            return fail(EEditError::STALE_TARGET);
        }
        state.active = handle;
        return {};
    }
    EditResult<void> ActiveEditHistory::deactivate() noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return checked;
        }
        impl_->active = {};
        return {};
    }
    EditResult<HistoryTargetHandle> ActiveEditHistory::activeTarget() const noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
        {
            return fail(EEditError::WRONG_THREAD);
        }
        return impl_->active;
    }
    EditResult<ActiveHistoryView> ActiveEditHistory::view() const noexcept
    {
        auto &state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        const auto *item = state.find(state.active);
        if (!item)
        {
            return ActiveHistoryView{};
        }
        RouteGuard guard(state.busy);
        auto target = state.targetView(*item);
        if (!target)
        {
            return lux::cxx::unexpected(target.error());
        }
        return ActiveHistoryView{item->handle, true, *target};
    }
    EditResult<HistoryTargetResult> ActiveEditHistory::undo() noexcept
    {
        return invoke(EHistoryAction::UNDO);
    }
    EditResult<HistoryTargetResult> ActiveEditHistory::redo() noexcept
    {
        return invoke(EHistoryAction::REDO);
    }

    EditResult<HistoryTargetResult> ActiveEditHistory::invoke(EHistoryAction action) noexcept
    {
        if (auto checked = impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        if (!impl_->active.valid())
            return fail(EEditError::NO_ACTIVE_TARGET);
        return invoke(action, impl_->active);
    }
    EditResult<HistoryTargetView> ActiveEditHistory::view(HistoryTargetHandle handle) const noexcept
    {
        if (auto checked = impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        const auto *item = impl_->find(handle);
        if (!item)
            return fail(EEditError::STALE_TARGET);
        RouteGuard guard(impl_->busy);
        return impl_->targetView(*item);
    }
    EditResult<HistoryTargetResult> ActiveEditHistory::undo(HistoryTargetHandle handle) noexcept
    {
        return invoke(EHistoryAction::UNDO, handle);
    }
    EditResult<HistoryTargetResult> ActiveEditHistory::redo(HistoryTargetHandle handle) noexcept
    {
        return invoke(EHistoryAction::REDO, handle);
    }
    EditResult<HistoryTargetResult> ActiveEditHistory::invoke(EHistoryAction action,
                                                              HistoryTargetHandle handle) noexcept
    {
        auto &state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        const auto *item = state.find(handle);
        if (!item)
        {
            return fail(EEditError::STALE_TARGET);
        }
        RouteGuard guard(state.busy);
        auto before = state.targetView(*item);
        if (!before)
        {
            return lux::cxx::unexpected(before.error());
        }
        const auto availability = action == EHistoryAction::UNDO ? before->undo : before->redo;
        if (before->history.closed)
        {
            return fail(EEditError::CLOSED);
        }
        if (availability != EHistoryActionAvailability::READY)
        {
            return fail(unavailable(availability, action));
        }
        auto result = action == EHistoryAction::UNDO ? item->target->undo() : item->target->redo();
        if (!result)
        {
            return result;
        }
        const auto &content = result->content;
        const bool is_wrong_history = content.current.history != item->history || !content.current.valid();
        const bool is_content = result->outcome == EHistoryTargetOutcome::CONTENT_APPLIED &&
                                content.effect == EEditEffect::CHANGE &&
                                content.revision.value > before->history.revision.value;
        const bool is_cancel = result->outcome == EHistoryTargetOutcome::TRANSIENT_CANCELLED &&
                               content.effect == EEditEffect::NO_CHANGE && content.current == before->history.current &&
                               content.revision == before->history.revision &&
                               content.event_sequence == before->history.event_sequence;
        if (is_wrong_history || (!is_content && !is_cancel))
        {
            return fail(EEditError::CONTRACT_VIOLATION);
        }
        return result;
    }

    EditResult<void> ActiveEditHistory::unregisterTarget(HistoryTargetHandle handle) noexcept
    {
        auto &state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return checked;
        }
        for (auto &item : state.registrations)
        {
            if (item.target && item.handle == handle)
            {
                if (--item.references)
                    return {};
                item = {};
                if (state.active == handle)
                {
                    state.active = {};
                }
                return {};
            }
        }
        return fail(EEditError::STALE_TARGET);
    }
    EditResult<void> ActiveEditHistory::close() noexcept
    {
        auto &state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return checked.error().code == EEditError::CLOSED ? EditResult<void>{} : checked;
        }
        state.closed = true;
        state.active = {};
        state.registrations.clear();
        state.control->router = nullptr;
        return {};
    }

    HistoryTargetRegistration::HistoryTargetRegistration(std::weak_ptr<detail::ActiveEditHistoryControl> control,
                                                         HistoryTargetHandle handle) noexcept
        : control_(std::move(control)), handle_(handle)
    {
    }
    HistoryTargetRegistration::~HistoryTargetRegistration() noexcept
    {
        if (!reset())
        {
            std::abort();
        }
    }
    HistoryTargetRegistration::HistoryTargetRegistration(HistoryTargetRegistration &&other) noexcept
    {
        *this = std::move(other);
    }
    HistoryTargetRegistration &HistoryTargetRegistration::operator=(HistoryTargetRegistration &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        if (const auto control = other.control_.lock(); control && other.handle_.valid())
        {
            const auto *router = control->router.load(std::memory_order_acquire);
            if (router && control->owner != std::this_thread::get_id())
            {
                std::abort();
            }
            // Transferring the token does not mutate the source route. In particular,
            // registerTarget returns its new token while its call guard is still live.
            // The destination's reset below enforces admission when replacing a binding.
        }
        if (!reset())
        {
            std::abort();
        }
        control_ = std::move(other.control_);
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }
    HistoryTargetHandle HistoryTargetRegistration::handle() const noexcept
    {
        return handle_;
    }
    EditResult<void> HistoryTargetRegistration::reset() noexcept
    {
        const auto control = control_.lock();
        if (control && handle_.valid())
        {
            auto *router = control->router.load(std::memory_order_acquire);
            if (router && control->owner != std::this_thread::get_id())
            {
                return fail(EEditError::WRONG_THREAD);
            }
            if (router)
            {
                if (auto result = router->unregisterTarget(handle_); !result)
                {
                    return result;
                }
            }
        }
        control_.reset();
        handle_ = {};
        return {};
    }
} // namespace lux::editor::ui
