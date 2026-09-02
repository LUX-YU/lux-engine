#pragma once

#include <lux/engine/editor/context/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace lux::editor
{
    namespace detail
    {
        struct ToolsetTestAccess;

        template<class Tool>
        concept HasRequestStop = requires(Tool& tool) { tool.requestStop(); };

        template<class Tool>
        concept NothrowRequestStop = requires(Tool& tool) {
            { tool.requestStop() } noexcept -> std::same_as<void>;
        };
    } // namespace detail

    enum class EToolsetError : std::uint8_t
    {
        INVALID_TYPE,
        DUPLICATE_TOOL,
        TYPE_COLLISION,
        MISSING_TOOL,
        FROZEN,
        STOPPING,
        CONSTRUCTION_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct ToolsetFailure final
    {
        EToolsetError code{EToolsetError::INVALID_TYPE};
        lux::cxx::TypeToken type{};
        lux::cxx::TypeToken conflicting_type{};
    };

    /**
     * Owns concrete long-lived L4 Tool facilities for one EditorContext.
     *
     * Installation is confined to the composition phase. freeze() publishes
     * the stable set to Editor windows; requestStop() is idempotent. Contributed
     * code and its static TypeToken names must remain loaded until this Toolset
     * is destroyed.
     */
    class LUX_EDITOR_CONTEXT_PUBLIC Toolset final
    {
    public:
        Toolset();
        ~Toolset() noexcept;

        Toolset(const Toolset&) = delete;
        Toolset& operator=(const Toolset&) = delete;
        Toolset(Toolset&&) = delete;
        Toolset& operator=(Toolset&&) = delete;

        template<class Tool, class... Args>
        [[nodiscard]] lux::cxx::expected<std::reference_wrapper<Tool>, ToolsetFailure>
        install(Args&&... args) noexcept
        {
            static_assert(std::is_object_v<Tool> && !std::is_array_v<Tool>);
            static_assert(std::is_nothrow_destructible_v<Tool>, "Tool destructors must be noexcept");
            static_assert(
                !detail::HasRequestStop<Tool> || detail::NothrowRequestStop<Tool>,
                "Tool requestStop() must return void and be noexcept"
            );

            const auto type = lux::cxx::typeToken<Tool>();
            if (auto ready = prepareInstall(type); !ready)
            {
                return lux::cxx::unexpected(ready.error());
            }

            Tool* tool{};
            try
            {
                tool = new Tool(std::forward<Args>(args)...);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(failure(EToolsetError::ALLOCATION_FAILURE, type));
            }
            catch (...)
            {
                // Tool construction is an explicit plugin/factory containment boundary.
                return lux::cxx::unexpected(failure(EToolsetError::CONSTRUCTION_FAILURE, type));
            }

            const auto destroy = [](void* value) noexcept { delete static_cast<Tool*>(value); };
            RequestStopFn request_stop{};
            if constexpr (detail::NothrowRequestStop<Tool>)
            {
                request_stop = [](void* value) noexcept { static_cast<Tool*>(value)->requestStop(); };
            }

            if (auto inserted = installErased(type, tool, destroy, request_stop); !inserted)
            {
                destroy(tool);
                return lux::cxx::unexpected(inserted.error());
            }
            return std::ref(*tool);
        }

        template<class Tool>
        [[nodiscard]] lux::cxx::expected<std::reference_wrapper<Tool>, ToolsetFailure> get() noexcept
        {
            const auto type = lux::cxx::typeToken<Tool>();
            auto* value = static_cast<Tool*>(findErased(type));
            if (value == nullptr)
            {
                return lux::cxx::unexpected(failure(EToolsetError::MISSING_TOOL, type));
            }
            return std::ref(*value);
        }

        template<class Tool>
        [[nodiscard]] lux::cxx::expected<std::reference_wrapper<const Tool>, ToolsetFailure> get() const noexcept
        {
            const auto type = lux::cxx::typeToken<Tool>();
            const auto* value = static_cast<const Tool*>(findErased(type));
            if (value == nullptr)
            {
                return lux::cxx::unexpected(failure(EToolsetError::MISSING_TOOL, type));
            }
            return std::cref(*value);
        }

        template<class Tool>
        [[nodiscard]] Tool* find() noexcept
        {
            return static_cast<Tool*>(findErased(lux::cxx::typeToken<Tool>()));
        }

        template<class Tool>
        [[nodiscard]] const Tool* find() const noexcept
        {
            return static_cast<const Tool*>(findErased(lux::cxx::typeToken<Tool>()));
        }

        void freeze() noexcept;
        void requestStop() noexcept;
        [[nodiscard]] bool frozen() const noexcept;
        [[nodiscard]] bool stopping() const noexcept;

    private:
        friend struct detail::ToolsetTestAccess;

        using DestroyFn = void (*)(void*) noexcept;
        using RequestStopFn = void (*)(void*) noexcept;

        [[nodiscard]] static ToolsetFailure failure(
            EToolsetError code,
            lux::cxx::TypeToken type = {},
            lux::cxx::TypeToken conflicting_type = {}
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, ToolsetFailure> prepareInstall(lux::cxx::TypeToken type) const noexcept;
        [[nodiscard]] lux::cxx::expected<void, ToolsetFailure> installErased(
            lux::cxx::TypeToken type,
            void* value,
            DestroyFn destroy,
            RequestStopFn request_stop
        ) noexcept;
        [[nodiscard]] void* findErased(lux::cxx::TypeToken type) noexcept;
        [[nodiscard]] const void* findErased(lux::cxx::TypeToken type) const noexcept;

        struct Impl;
        Impl* impl_{};
    };
} // namespace lux::editor
