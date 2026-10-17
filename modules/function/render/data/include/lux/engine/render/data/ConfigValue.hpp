#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::render
{
    inline constexpr auto kInvalidConfig = error::errorId("lux.render.data.invalid_config");

    // Dynamic configurations specialize this with full retained backing/capacity, not visible ranges.
    template <class T>
    struct ConfigValueTraits
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const T&) noexcept
            requires std::is_trivially_copyable_v<T>
        {
            return sizeof(T);
        }
    };

    // One immutable shared configuration. No object dispatch, clone, mutable cast or wire ABI.
    class ConfigValue final
    {
    public:
        struct Type final
        {
            cxx::TypeToken token;
            std::size_t size = 0;
            std::size_t alignment = 0;
            bool operator==(const Type&) const noexcept = default;
        };

        template <class T> [[nodiscard]] static constexpr Type typeOf() noexcept
        {
            return {cxx::typeToken<T>(), sizeof(T), alignof(T)};
        }

        [[nodiscard]] Type typeInfo() const noexcept { return type_ ? type_->identity : Type{}; }

        // Cold semantic agreement only. No object representation/padding comparison and no hash shortcut.
        // Non-comparable configurations can still be shared by copying their existing immutable owner.
        [[nodiscard]] bool operator==(const ConfigValue& other) const noexcept
        {
            if (typeInfo() != other.typeInfo()) return false;
            if (value_.get() == other.value_.get()) return true;
            return type_ && type_->equal && type_->equal(value_.get(), other.value_.get());
        }

        ConfigValue() noexcept = default;
        ConfigValue(const ConfigValue&) noexcept = default;

        ConfigValue(ConfigValue&& other) noexcept
            : code_pin_(std::move(other.code_pin_)), value_(std::move(other.value_)),
              type_(std::exchange(other.type_, {})), retained_bytes_(std::exchange(other.retained_bytes_, 0)) {}

        ConfigValue& operator=(ConfigValue other) noexcept
        {
            // Both pins remain live until the corresponding old shared control block is destroyed.
            using std::swap;
            swap(code_pin_, other.code_pin_);
            swap(value_, other.value_);
            swap(type_, other.type_);
            swap(retained_bytes_, other.retained_bytes_);
            return *this;
        }

        template <class T>
            requires std::is_nothrow_move_constructible_v<std::remove_cvref_t<T>> &&
                std::is_nothrow_destructible_v<std::remove_cvref_t<T>> &&
                requires(const std::remove_cvref_t<T>& value) {
                    { ConfigValueTraits<std::remove_cvref_t<T>>::retainedBytes(value) } noexcept
                        -> std::same_as<RenderResult<std::size_t>>;
                }
        [[nodiscard]] static RenderResult<ConfigValue>
        make(T&& value, const std::shared_ptr<const void>& source_code_pin = {}) noexcept
        {
            using Value = std::remove_cvref_t<T>;
            auto bytes = ConfigValueTraits<Value>::retainedBytes(value);
            if (!bytes) return cxx::unexpected(bytes.error());
            if (*bytes < sizeof(Value)) return cxx::unexpected(RenderError{kInvalidConfig});
            ConfigValue result;
            result.code_pin_ = source_code_pin;
            result.value_ = std::make_shared<const Value>(std::forward<T>(value));
            static constexpr Metadata type{typeOf<Value>(), equality<Value>()};
            result.type_ = &type;
            result.retained_bytes_ = *bytes;
            return result;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return bool(value_); }
        [[nodiscard]] cxx::TypeToken type() const noexcept { return typeInfo().token; }
        [[nodiscard]] std::size_t objectSize() const noexcept { return typeInfo().size; }
        [[nodiscard]] std::size_t objectAlignment() const noexcept { return typeInfo().alignment; }
        [[nodiscard]] std::size_t retainedBytes() const noexcept { return retained_bytes_; }

        template <class T>
        [[nodiscard]] const T* borrow() const noexcept
        {
            const bool matches = type() == cxx::typeToken<T>() && objectSize() == sizeof(T) &&
                objectAlignment() == alignof(T);
            return matches ? static_cast<const T*>(value_.get()) : nullptr;
        }

        // Trusted typed Factory/Graph thunk uses this after its single cold TypeToken check.
        [[nodiscard]] const void* data() const noexcept { return value_.get(); }

    private:
        using Equal = bool (*)(const void*, const void*) noexcept;
        struct Metadata final
        {
            Type identity;
            Equal equal;
        };

        template <class T> static constexpr Equal equality() noexcept
        {
            if constexpr (requires(const T& a, const T& b) { { a == b } noexcept -> std::convertible_to<bool>; })
                return [](const void* a, const void* b) noexcept {
                    return *static_cast<const T*>(a) == *static_cast<const T*>(b);
                };
            else
                return nullptr;
        }

        std::shared_ptr<const void> code_pin_;
        std::shared_ptr<const void> value_;
        const Metadata* type_ = nullptr;
        std::size_t retained_bytes_ = 0;
    };
}
