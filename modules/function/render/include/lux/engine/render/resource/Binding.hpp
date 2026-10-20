#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/resource/StorageDescriptor.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    // Cold placement lists. No scope identity, parent tree or runtime authority is allocated here.
    template <class T> struct StorageGroups final
    {
        std::vector<T> global;
        std::vector<T> scene;
        std::vector<T> view;
        std::vector<T> attachment;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return global.size() + scene.size() + view.size() + attachment.size();
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return size() == 0;
        }
    };

    struct GraphStorageBinding final
    {
        GraphResourceId resource;
        StorageRequirement storage;
    };

    using GraphStorageBindings = StorageGroups<GraphStorageBinding>;

    // Effect-owned concrete arguments, resolved only at Attach; never part of immutable Native code.
    class StorageArgument final
    {
    public:
        GraphResourceId resource;

        StorageArgument() noexcept = default;
        StorageArgument(const StorageArgument&) = delete;
        StorageArgument& operator=(const StorageArgument&) = delete;

        StorageArgument(StorageArgument&& other) noexcept
            : resource(other.resource), value_(std::move(other.value_)), type_(std::exchange(other.type_, {})),
              equal_(std::exchange(other.equal_, nullptr))
        {
        }

        StorageArgument& operator=(StorageArgument&& other) noexcept
        {
            if (this != &other)
            {
                resource = other.resource;
                value_ = std::move(other.value_);
                type_ = std::exchange(other.type_, {});
                equal_ = std::exchange(other.equal_, nullptr);
            }
            return *this;
        }

        template <class T> [[nodiscard]] static StorageArgument make(GraphResourceId resource, T value) noexcept
        {
            static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>);
            StorageArgument result;
            result.resource = resource;
            result.value_ = {
                new const T(std::move(value)),
                [](const void* object) noexcept { delete static_cast<const T*>(object); }
            };
            result.type_ = cppType<T>();
            if constexpr (requires(const T& a, const T& b) {
                              { a == b } noexcept -> std::convertible_to<bool>;
                          })
            {
                result.equal_ = [](const void* a, const void* b) noexcept
                { return *static_cast<const T*>(a) == *static_cast<const T*>(b); };
            }
            return result;
        }

        [[nodiscard]] CppType typeInfo() const noexcept
        {
            return type_;
        }

        [[nodiscard]] const void* data() const noexcept
        {
            return value_.get();
        }

        [[nodiscard]] bool operator==(const StorageArgument& other) const noexcept
        {
            return type_ == other.type_ && resource == other.resource &&
                   (data() == other.data() || (equal_ && equal_(data(), other.data())));
        }

    private:
        using Value = std::unique_ptr<const void, void (*)(const void*) noexcept>;
        Value value_{nullptr, nullptr};
        CppType type_{};
        bool (*equal_)(const void*, const void*) noexcept = nullptr;
    };

} // namespace lux::render
