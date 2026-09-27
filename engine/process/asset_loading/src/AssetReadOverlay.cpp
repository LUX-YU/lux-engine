#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <algorithm>

namespace lux::process::asset_loading
{
    namespace
    {
        class MemoryOverlay final : public AssetReadPort::Endpoint
        {
        public:
            MemoryOverlay(std::vector<MemoryAssetImage> images, AssetReadPort fallback)
                : images_(std::move(images)), fallback_(std::move(fallback))
            {}
            lux::async::SubmitResult submit(
                ReadAssetImage request,
                void* state,
                void (*complete)(void*, Outcome&&) noexcept,
                lux::async::SubmitOptions options
            ) noexcept override
            {
                const auto found = std::ranges::find(images_, request.id, &MemoryAssetImage::id);
                if (found == images_.end())
                    return fallback_.submit(request, state, complete, options);
                // This port transports bytes only. loadAsset schedules decoding on CPU.
                auto image = found->image;
                complete(state, Outcome{std::move(image)});
                return {};
            }

        private:
            const std::vector<MemoryAssetImage> images_;
            AssetReadPort fallback_;
        };
    }

    lux::cxx::expected<AssetReadPort, lux::async::ESubmitError> makeAssetReadOverlay(
        std::vector<MemoryAssetImage> images,
        AssetReadPort fallback
    ) noexcept
    {
        for (std::size_t index{}; index < images.size(); ++index)
        {
            const auto& image = images[index];
            const bool invalid = image.id.isNull() || !image.image;
            const bool duplicate = std::ranges::find(std::span(images).first(index), image.id, &MemoryAssetImage::id) !=
                                   std::span(images).first(index).end();
            if (invalid || duplicate)
                return lux::cxx::unexpected(lux::async::ESubmitError::PAYLOAD_INVALID);
        }
        return AssetReadPort{std::make_shared<MemoryOverlay>(std::move(images), std::move(fallback))};
    }
}
