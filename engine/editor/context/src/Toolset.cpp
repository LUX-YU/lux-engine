#include <lux/engine/editor/Toolset.hpp>

#include <algorithm>
#include <new>
#include <vector>

namespace lux::editor
{
    namespace
    {
        enum class EToolsetState : std::uint8_t
        {
            COMPOSING,
            FROZEN,
            STOPPING,
        };
    }

    struct Toolset::Impl final
    {
        struct Entry final
        {
            lux::cxx::TypeToken type;
            void* value{};
            DestroyFn destroy{};
            RequestStopFn request_stop{};
        };

        std::vector<Entry> entries;
        EToolsetState state{EToolsetState::COMPOSING};
    };

    Toolset::Toolset() : impl_(new Impl())
    {
    }

    Toolset::~Toolset() noexcept
    {
        requestStop();
        if (impl_ == nullptr)
            return;
        while (!impl_->entries.empty())
        {
            const auto entry = impl_->entries.back();
            impl_->entries.pop_back();
            entry.destroy(entry.value);
        }
        delete impl_;
        impl_ = nullptr;
    }

    ToolsetFailure Toolset::failure(
        EToolsetError code,
        lux::cxx::TypeToken type,
        lux::cxx::TypeToken conflicting_type
    ) noexcept
    {
        return ToolsetFailure{code, type, conflicting_type};
    }

    lux::cxx::expected<void, ToolsetFailure> Toolset::prepareInstall(lux::cxx::TypeToken type) const noexcept
    {
        if (!type.isValid())
        {
            return lux::cxx::unexpected(failure(EToolsetError::INVALID_TYPE, type));
        }
        if (impl_ == nullptr || impl_->state == EToolsetState::STOPPING)
        {
            return lux::cxx::unexpected(failure(EToolsetError::STOPPING, type));
        }
        if (impl_->state == EToolsetState::FROZEN)
        {
            return lux::cxx::unexpected(failure(EToolsetError::FROZEN, type));
        }
        const auto existing = std::ranges::find_if(impl_->entries, [type](const Impl::Entry& entry) noexcept {
            return entry.type.hash() == type.hash();
        });
        if (existing == impl_->entries.end())
            return {};
        const auto code = existing->type.name() == type.name() ? EToolsetError::DUPLICATE_TOOL :
                                                                 EToolsetError::TYPE_COLLISION;
        return lux::cxx::unexpected(failure(code, type, existing->type));
    }

    lux::cxx::expected<void, ToolsetFailure> Toolset::installErased(
        lux::cxx::TypeToken type,
        void* value,
        DestroyFn destroy,
        RequestStopFn request_stop
    ) noexcept
    {
        if (value == nullptr || destroy == nullptr)
        {
            return lux::cxx::unexpected(failure(EToolsetError::INVALID_TYPE, type));
        }
        if (auto ready = prepareInstall(type); !ready)
        {
            return ready;
        }
        try
        {
            impl_->entries.push_back(Impl::Entry{type, value, destroy, request_stop});
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EToolsetError::ALLOCATION_FAILURE, type));
        }
    }

    void* Toolset::findErased(lux::cxx::TypeToken type) noexcept
    {
        return const_cast<void*>(std::as_const(*this).findErased(type));
    }

    const void* Toolset::findErased(lux::cxx::TypeToken type) const noexcept
    {
        if (impl_ == nullptr || !type.isValid())
            return nullptr;
        const auto found = std::ranges::find_if(impl_->entries, [type](const Impl::Entry& entry) noexcept {
            return entry.type == type;
        });
        return found == impl_->entries.end() ? nullptr : found->value;
    }

    void Toolset::freeze() noexcept
    {
        if (impl_ != nullptr && impl_->state == EToolsetState::COMPOSING)
        {
            impl_->state = EToolsetState::FROZEN;
        }
    }

    void Toolset::requestStop() noexcept
    {
        if (impl_ == nullptr || impl_->state == EToolsetState::STOPPING)
            return;
        impl_->state = EToolsetState::STOPPING;
        for (auto iterator = impl_->entries.rbegin(); iterator != impl_->entries.rend(); ++iterator)
        {
            if (iterator->request_stop != nullptr)
            {
                iterator->request_stop(iterator->value);
            }
        }
    }

    bool Toolset::frozen() const noexcept
    {
        return impl_ != nullptr && impl_->state != EToolsetState::COMPOSING;
    }

    bool Toolset::stopping() const noexcept
    {
        return impl_ == nullptr || impl_->state == EToolsetState::STOPPING;
    }
} // namespace lux::editor
