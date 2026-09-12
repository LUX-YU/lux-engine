#pragma once

#include <lux/engine/editor/application/UiVulkanPresentation.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <functional>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace lux::editor::application::detail
{
    template<class Factory>
    [[nodiscard]] lux::cxx::expected<std::jthread, EUiVulkanPresentationError>
    startPresentationThread(Factory&& factory) noexcept
    {
        try
        {
            return std::invoke(std::forward<Factory>(factory));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EUiVulkanPresentationError::ALLOCATION_FAILURE);
        }
        catch (const std::system_error&)
        {
            return lux::cxx::unexpected(EUiVulkanPresentationError::THREAD_CREATION_FAILURE);
        }
    }
} // namespace lux::editor::application::detail
