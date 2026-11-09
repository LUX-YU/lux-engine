#pragma once
#include <lux/engine/editor/AssetImporter.hpp>
#include <lux/engine/editor/ValueEditing.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor
{
    struct AssetImportEditor final
    {
        std::shared_ptr<const void> code_lifetime;
        std::string importer;
        std::uint32_t version{};
        cxx::type_info type;
        AssetImportResult<std::unique_ptr<ui::Element>> (*create)(std::span<const std::byte>) noexcept = nullptr;
        AssetImportResult<std::vector<std::byte>> (*encode)(const ui::Element&) noexcept = nullptr;
    };

    // One decoded draft per open form. Only an explicit import/encode creates bytes.
    template <AssetImporter T> class AssetImportFields final : public ui::Element
    {
    public:
        explicit AssetImportFields(typename T::Configuration value) noexcept : value_(std::move(value)) {}

        [[nodiscard]] AssetImportResult<std::vector<std::byte>> encode() const noexcept
        {
            return T::encode(value_);
        }

    private:
        ui::SizeHint sizeHintContent() noexcept override
        {
            ui::SizeHint hint;
            hint.minimum = {180, 32};
            hint.preferred = {300, 150};
            return hint;
        }

        void draw() noexcept override
        {
            static_cast<void>(editValue(ValueEditOptions{}, value_));
        }

        typename T::Configuration value_;
    };

    template <AssetImporter T> AssetImportEditor declareAssetImportEditor(std::shared_ptr<const void> code = {})
    {
        return {
            std::move(code),
            std::string{T::name},
            T::version,
            cxx::make_basic_type_info<typename T::Configuration>(),
            +[](std::span<const std::byte> bytes) noexcept -> AssetImportResult<std::unique_ptr<ui::Element>>
            {
                auto value = T::decode(bytes);
                if (!value)
                {
                    return cxx::unexpected(value.error());
                }
                return std::unique_ptr<ui::Element>{std::make_unique<AssetImportFields<T>>(std::move(*value))};
            },
            +[](const ui::Element& element) noexcept
            { return static_cast<const AssetImportFields<T>&>(element).encode(); }
        };
    }
} // namespace lux::editor
