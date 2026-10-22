#pragma once

#include <concepts>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::render
{
    inline constexpr auto kInvalidConfig = error::errorId("lux.render.data.invalid_config");

    // Dynamic configurations count their own allocations/capacity, excluding externally borrowed data.
    template <class T> struct ConfigValueTraits
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const T&) noexcept
            requires std::is_trivially_copyable_v<T>
        {
            return sizeof(T);
        }
    };

    // One immutable, exclusively owned configuration. Borrowed views last only as long as this value.
    // TypeToken compares semantic C++ identity, not a portable serialization/DLL ABI. The defining module
    // supplies equality/destruction code and must remain loaded until this value is destroyed.
    class ConfigValue final
    {
    public:
        [[nodiscard]] cxx::TypeToken type() const noexcept
        {
            return type_ ? type_->identity : cxx::TypeToken{};
        }

        // Cold semantic agreement only. No object representation/padding comparison and no hash shortcut.
        [[nodiscard]] bool operator==(const ConfigValue& other) const noexcept
        {
            if (type() != other.type())
            {
                return false;
            }
            if (value_.get() == other.value_.get())
            {
                return true;
            }
            return type_ && type_->equal && type_->equal(value_.get(), other.value_.get());
        }

        ConfigValue() noexcept = default;
        ConfigValue(const ConfigValue&) = delete;
        ConfigValue& operator=(const ConfigValue&) = delete;

        ConfigValue(ConfigValue&& other) noexcept
            : value_(std::move(other.value_)), type_(std::exchange(other.type_, {})),
              retained_bytes_(std::exchange(other.retained_bytes_, 0))
        {
        }

        ConfigValue& operator=(ConfigValue&& other) noexcept
        {
            if (this != &other)
            {
                value_ = std::move(other.value_);
                type_ = std::exchange(other.type_, nullptr);
                retained_bytes_ = std::exchange(other.retained_bytes_, 0);
            }
            return *this;
        }

        template <class T>
            requires std::is_nothrow_move_constructible_v<std::remove_cvref_t<T>> &&
                     std::is_nothrow_destructible_v<std::remove_cvref_t<T>> &&
                     requires(const std::remove_cvref_t<T>& value) {
                         {
                             ConfigValueTraits<std::remove_cvref_t<T>>::retainedBytes(value)
                         } noexcept -> std::same_as<RenderResult<std::size_t>>;
                     }
        [[nodiscard]] static RenderResult<ConfigValue> make(T&& value) noexcept
        {
            using Value = std::remove_cvref_t<T>;
            auto bytes = ConfigValueTraits<Value>::retainedBytes(value);
            if (!bytes)
            {
                return cxx::unexpected(bytes.error());
            }
            if (*bytes < sizeof(Value))
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            ConfigValue result;
            result.value_ = ConfigObject{
                new const Value(std::forward<T>(value)),
                [](const void* object) noexcept { delete static_cast<const Value*>(object); }
            };
            static constexpr Metadata type{cxx::typeToken<Value>(), equality<Value>()};
            result.type_ = &type;
            result.retained_bytes_ = *bytes;
            return result;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return bool(value_);
        }

        [[nodiscard]] std::size_t retainedBytes() const noexcept
        {
            return retained_bytes_;
        }

        template <class T> [[nodiscard]] const T* borrow() const noexcept
        {
            const bool matches = type() == cxx::typeToken<T>();
            return matches ? static_cast<const T*>(value_.get()) : nullptr;
        }

        // Trusted typed Factory/Graph thunk uses this after its single cold TypeToken check.
        [[nodiscard]] const void* data() const noexcept
        {
            return value_.get();
        }

    private:
        using Equal = bool (*)(const void*, const void*) noexcept;

        struct Metadata final
        {
            cxx::TypeToken identity;
            Equal equal;
        };

        template <class T> static constexpr Equal equality() noexcept
        {
            if constexpr (requires(const T& a, const T& b) {
                              { a == b } noexcept -> std::convertible_to<bool>;
                          })
            {
                return [](const void* a, const void* b) noexcept
                { return *static_cast<const T*>(a) == *static_cast<const T*>(b); };
            }
            else
            {
                return nullptr;
            }
        }

        using ConfigObject = std::unique_ptr<const void, void (*)(const void*) noexcept>;
        ConfigObject value_{nullptr, nullptr};
        const Metadata* type_ = nullptr;
        std::size_t retained_bytes_ = 0;
    };
} // namespace lux::render
