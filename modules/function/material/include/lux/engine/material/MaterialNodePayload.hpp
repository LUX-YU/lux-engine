#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/material/MaterialCompileFailure.hpp>
#include <lux/engine/material/graph/visibility.h>
#include <lux/engine/object/CodeLease.hpp>

#include <memory>
#include <type_traits>
#include <utility>

namespace lux::material
{
    template <class T> using MaterialNodeResult = cxx::expected<T, MaterialCompileFailure>;

    // Owns only semantic data and its code. No node/pin identity or graph membership.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialNodePayload final
    {
    public:
        MaterialNodePayload() noexcept = default;
        ~MaterialNodePayload();
        MaterialNodePayload(const MaterialNodePayload&) = delete;
        MaterialNodePayload& operator=(const MaterialNodePayload&) = delete;
        MaterialNodePayload(MaterialNodePayload&&) noexcept;
        MaterialNodePayload& operator=(MaterialNodePayload&&) noexcept;

        template <class T, auto Clone, class... Args>
            requires std::is_nothrow_constructible_v<T, Args...> && std::is_nothrow_destructible_v<T> &&
                     std::is_same_v<decltype(Clone), MaterialNodeResult<std::unique_ptr<T>> (*)(const T&) noexcept>
        [[nodiscard]] static MaterialNodeResult<MaterialNodePayload>
        make(object::CodeLease code, Args&&... args) noexcept
        {
            static_assert(Clone != nullptr);
            if (!code.valid())
            {
                return cxx::unexpected(MaterialCompileFailure{
                    EMaterialCompileError::INVALID_RESULT,
                    "node payload requires a valid code lease"
                });
            }
            auto value = std::make_unique<T>(std::forward<Args>(args)...);
            return MaterialNodePayload{
                std::move(code),
                cxx::typeToken<T>(),
                value.release(),
                [](void* pointer) noexcept { delete static_cast<T*>(pointer); },
                [](const void* pointer) noexcept -> MaterialNodeResult<void*>
                {
                    auto copy = Clone(*static_cast<const T*>(pointer));
                    if (!copy)
                    {
                        return cxx::unexpected(std::move(copy.error()));
                    }
                    return copy->release();
                }
            };
        }

        [[nodiscard]] MaterialNodeResult<MaterialNodePayload> clone() const noexcept;

        template <class T> [[nodiscard]] T* get() noexcept
        {
            return type_ == cxx::typeToken<T>() ? static_cast<T*>(value_) : nullptr;
        }

        template <class T> [[nodiscard]] const T* get() const noexcept
        {
            return type_ == cxx::typeToken<T>() ? static_cast<const T*>(value_) : nullptr;
        }

    private:
        friend class MaterialNodeType;
        using Destroy = void (*)(void*) noexcept;
        using Clone = MaterialNodeResult<void*> (*)(const void*) noexcept;

        MaterialNodePayload(object::CodeLease, cxx::TypeToken, void*, Destroy, Clone) noexcept;
        void reset() noexcept;

        object::CodeLease code_{object::CodeLease::builtin()};
        cxx::TypeToken type_;
        void* value_{};
        Destroy destroy_{};
        Clone clone_{};
    };
} // namespace lux::material
