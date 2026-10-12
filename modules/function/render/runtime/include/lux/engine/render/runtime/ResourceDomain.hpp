#pragma once

#include <concepts>
#include <functional>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Handles.hpp>
#include <memory>
#include <vector>

namespace lux::render
{
    struct ResourceDomainTag;
    using ResourceDomainId = cxx::SlotKey<ResourceDomainTag>;

    // Configuration-aware factories explicitly validate hits without constructing another resource.
    template <typename F, typename T>
    concept ResourceFactoryFor = requires(F& factory, const T& existing) {
        { factory() } -> std::same_as<RenderResult<std::unique_ptr<T>>>;
        { factory.isCompatible(existing) } -> std::same_as<bool>;
    };

    // One unnamed resource per type; compiler-local type hash AND name are compared. Type information selects a local
    // entry, never an object ID. Parent is a read-only stable borrow; Session rejects deleting a parent with children.
    // Mutation and erase require the caller's GPU-safe point, just like Native RAII destruction.
    class ResourceDomain
    {
    public:
        explicit ResourceDomain(ResourceDomainId parent_id = {}, const ResourceDomain* parent = nullptr) noexcept
            : parent_id_(parent_id), parent_(parent)
        {
        }

        ResourceDomain(const ResourceDomain&) = delete;
        ResourceDomain& operator=(const ResourceDomain&) = delete;

        [[nodiscard]] ResourceDomainId parentId() const noexcept
        {
            return parent_id_;
        }

        template <typename T> [[nodiscard]] T* tryGetLocal() noexcept
        {
            for (auto& entry : entries_)
            {
                if (entry->type.hash() == cxx::type_hash<T>() && entry->type.name() == cxx::type_name<T>())
                {
                    return static_cast<Entry<T>*>(entry.get())->value.get();
                }
            }
            return nullptr;
        }

        template <typename T> [[nodiscard]] const T* tryGetLocal() const noexcept
        {
            return const_cast<ResourceDomain*>(this)->tryGetLocal<T>();
        }

        template <typename T> [[nodiscard]] const T* tryGet() const noexcept
        {
            if (const auto* local = tryGetLocal<T>())
            {
                return local;
            }
            return parent_ ? parent_->tryGet<T>() : nullptr;
        }

        template <typename T> [[nodiscard]] T& getLocal() noexcept
        {
            auto* value = tryGetLocal<T>();
            if (!value)
            {
                std::terminate();
            }
            return *value;
        }

        template <typename T> [[nodiscard]] const T& getLocal() const noexcept
        {
            return const_cast<ResourceDomain*>(this)->getLocal<T>();
        }

        template <typename T, ResourceFactoryFor<T> F>
        [[nodiscard]] RenderResult<std::reference_wrapper<T>> getOrCreate(F&& factory) noexcept
        {
            if (auto* existing = tryGetLocal<T>())
            {
                if (!factory.isCompatible(*existing))
                {
                    return cxx::unexpected(RenderError{kDefinitionMismatch});
                }
                return std::ref(*existing);
            }
            auto candidate = factory();
            if (!candidate)
            {
                return cxx::unexpected(candidate.error());
            }
            return getOrCreate<T>(std::move(*candidate));
        }

        // An already occupied type is an explicit conflict, not a silent replacement.
        template <typename T>
        [[nodiscard]] RenderResult<std::reference_wrapper<T>> getOrCreate(std::unique_ptr<T> candidate) noexcept
        {
            if (!candidate)
            {
                return cxx::unexpected(RenderError{kInvalidIdentity});
            }
            if (tryGetLocal<T>())
            {
                return cxx::unexpected(RenderError{kDefinitionMismatch});
            }
            auto& value = *candidate;
            entries_.push_back(std::make_unique<Entry<T>>(std::move(candidate)));
            return std::ref(value);
        }

        template <typename T> bool erase() noexcept
        {
            for (auto i = entries_.begin(); i != entries_.end(); ++i)
            {
                if ((*i)->type.hash() == cxx::type_hash<T>() && (*i)->type.name() == cxx::type_name<T>())
                {
                    entries_.erase(i);
                    return true;
                }
            }
            return false;
        }

    private:
        struct EntryBase
        {
            explicit EntryBase(cxx::basic_type_info info) noexcept : type(info) {}

            virtual ~EntryBase() = default;
            cxx::basic_type_info type;
        };

        template <typename T> struct Entry final : EntryBase
        {
            explicit Entry(std::unique_ptr<T> owned) noexcept
                : EntryBase(cxx::make_basic_type_info<T>()), value(std::move(owned))
            {
            }

            std::unique_ptr<T> value;
        };

        ResourceDomainId parent_id_;
        const ResourceDomain* parent_;
        std::vector<std::unique_ptr<EntryBase>> entries_;
    };
} // namespace lux::render
