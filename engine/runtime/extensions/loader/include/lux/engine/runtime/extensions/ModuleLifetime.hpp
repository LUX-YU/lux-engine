#pragma once
/**
 * @file ModuleLifetime.hpp
 * @brief Shared code-lifetime owner for an extension dynamic library.
 */

#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/core/extension_abi/ModuleAbi.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lux::extensions
{
    enum class EExtensionModuleSource : std::uint8_t
    {
        FILE_PATH,
        MEMORY_IMAGE
    };

    struct ExtensionModuleOrigin final
    {
        EExtensionModuleSource kind{EExtensionModuleSource::FILE_PATH};
        std::filesystem::path path;
        std::string hint;
        std::size_t image_bytes{0u};
    };

    class ExtensionModuleSource final
    {
    public:
        ExtensionModuleSource() noexcept = default;

        [[nodiscard]] static ExtensionModuleSource fromPath(
            std::filesystem::path path)
        {
            ExtensionModuleSource result;
            result.kind_ = EExtensionModuleSource::FILE_PATH;
            result.path_ = std::move(path);
            return result;
        }

        [[nodiscard]] static ExtensionModuleSource fromMemory(
            lux::cxx::SharedBytes<> image,
            std::string hint)
        {
            ExtensionModuleSource result;
            result.kind_ = EExtensionModuleSource::MEMORY_IMAGE;
            result.image_ = std::move(image);
            result.hint_ = std::move(hint);
            return result;
        }

        [[nodiscard]] EExtensionModuleSource kind() const noexcept
        {
            return kind_;
        }
        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }
        [[nodiscard]] const lux::cxx::SharedBytes<>& image() const noexcept
        {
            return image_;
        }
        [[nodiscard]] std::string_view hint() const noexcept
        {
            return hint_;
        }
        [[nodiscard]] bool valid() const noexcept
        {
            if (kind_ == EExtensionModuleSource::FILE_PATH)
                return !path_.empty() && image_.empty() && hint_.empty();
            return path_.empty() && !image_.empty() && !hint_.empty();
        }
        [[nodiscard]] std::size_t accountedBytes() const noexcept
        {
            if (kind_ == EExtensionModuleSource::FILE_PATH)
            {
                return path_.native().size() *
                    sizeof(std::filesystem::path::value_type);
            }
            return image_.size() + hint_.size();
        }
        [[nodiscard]] ExtensionModuleOrigin origin() const
        {
            return ExtensionModuleOrigin{
                kind_,
                path_,
                hint_,
                image_.size()};
        }

    private:
        EExtensionModuleSource kind_{EExtensionModuleSource::FILE_PATH};
        std::filesystem::path path_;
        lux::cxx::SharedBytes<> image_;
        std::string hint_;
    };

    class ModuleLifetime final
    {
    public:
        ModuleLifetime(
            lux::engine::platform::DynamicLibrary library,
            ExtensionId id,
            ExtensionVersion version,
            ExtensionModuleOrigin origin) noexcept
            : library_(std::move(library))
            , id_(std::move(id))
            , version_(version)
            , origin_(std::move(origin))
        {}

        ModuleLifetime(const ModuleLifetime&) = delete;
        ModuleLifetime& operator=(const ModuleLifetime&) = delete;

        [[nodiscard]] const ExtensionId& id() const noexcept { return id_; }
        [[nodiscard]] ExtensionVersion version() const noexcept
        {
            return version_;
        }
        [[nodiscard]] const ExtensionModuleOrigin& origin() const noexcept
        {
            return origin_;
        }
        [[nodiscard]] const lux::engine::platform::DynamicLibrary&
        library() const noexcept
        {
            return library_;
        }

    private:
        lux::engine::platform::DynamicLibrary library_;
        ExtensionId id_;
        ExtensionVersion version_;
        ExtensionModuleOrigin origin_;
    };

    using ModuleLease = std::shared_ptr<const ModuleLifetime>;
}
