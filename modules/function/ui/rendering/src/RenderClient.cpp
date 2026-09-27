#include <lux/engine/function/render/client/FeatureOpSend.hpp>
#include <lux/engine/ui/rendering/detail/RenderFeature.hpp>

#include <lux/engine/ui/rendering/detail/FontConfiguration.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>

#include <algorithm>
#include <cstring>

namespace lux::ui
{
    lux::cxx::expected<std::vector<std::byte>, EInitError> makeRenderConfiguration(const Root& root)
    {
        auto font = root.fontAtlas();
        if (!font)
        {
            return lux::cxx::unexpected(font.error());
        }
        std::vector<std::byte> bytes;
        const auto encoded = render::UiRenderConfigurationCodec().portable.encode(&*font, bytes);
        if (!encoded)
            return lux::cxx::unexpected(EInitError::INVALID_FONT_DATA);
        return bytes;
    }

    render::Expected<void> appendFrame(
        render::RenderProgramSession::Builder& builder,
        const render::UiRenderOperationIds& operations,
        render::RenderSceneId scene,
        render::FeatureHandle feature,
        const std::shared_ptr<const RenderFrame>& input
    )
    {
        const bool is_invalid_destination = !operations.valid() || !scene.isValid() || !feature.isValid();
        const bool is_invalid_input = !input || !input->draw_data.valid() || input->sequence == 0;
        if (is_invalid_destination || is_invalid_input)
        {
            return render::renderFailure<render::err::comm::RequestInvalid>();
        }
        const bool has_missing_resources = !input->draw_data.textures().empty() && input->resources.empty();
        const bool has_invalid_resources =
            std::ranges::any_of(input->resources, [](const auto& state) { return !state; });
        if (has_missing_resources || has_invalid_resources)
            return render::renderFailure<render::err::comm::RequestInvalid>();
        auto submission = input->submission.acquire();
        if (!submission)
        {
            auto created = render::RenderSubmissionState::create();
            if (!created)
                return lux::cxx::unexpected(created.error());
            submission = std::move(*created);
            input->submission = submission.observe();
        }
        const auto index =
            builder.emplaceAttachment<detail::FrameInput>(detail::kFrameAttachment, std::move(submission), input);
        builder.push(
            render::opcode_of_v<render::UiRenderFrameOp>,
            operations.id<render::UiRenderFrameOp>(),
            render::UiRenderFramePayload{scene, feature, index}
        );
        return {};
    }
    render::Expected<void> appendClear(
        render::RenderProgramSession::Builder& builder,
        const render::UiRenderOperationIds& operations,
        render::RenderSceneId scene,
        render::FeatureHandle feature
    )
    {
        if (!operations.valid() || !scene.isValid() || !feature.isValid())
        {
            return render::renderFailure<render::err::comm::RequestInvalid>();
        }
        builder.push(
            render::opcode_of_v<render::UiRenderClearOp>,
            operations.id<render::UiRenderClearOp>(),
            render::UiRenderClearPayload{scene, feature}
        );
        return {};
    }

} // namespace lux::ui

namespace lux::render
{
    namespace
    {
        using namespace serialization;

        struct FontInput final
        {
            std::uint32_t width{}, height{};
            std::span<const std::byte> pixels;
        };

        lux::cxx::expected<FontInput, SerializationFailure> readFont(std::span<const std::byte> input) noexcept
        {
            BinaryReader reader(input);
            auto width = reader.readUnsigned<std::uint32_t>();
            if (!width)
                return lux::cxx::unexpected(width.error());
            auto height = reader.readUnsigned<std::uint32_t>();
            if (!height)
                return lux::cxx::unexpected(height.error());
            auto length = reader.readUnsigned<std::uint64_t>();
            if (!length)
                return lux::cxx::unexpected(length.error());
            const bool is_valid_size = *length == reader.remaining();
            const bool is_valid_font = ui::detail::validFontConfiguration(*width, *height, *length);
            if (!is_valid_size || !is_valid_font)
                return lux::cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()});
            return FontInput{*width, *height, input.subspan(reader.offset())};
        }

        SerializationResult encodeFont(const void* object, std::vector<std::byte>& output) noexcept
        {
            if (!object)
                return lux::cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE, 0});
            const auto& font = *static_cast<const ui::FontAtlas*>(object);
            const auto width = static_cast<std::uint32_t>(font.width);
            const auto height = static_cast<std::uint32_t>(font.height);
            if (!ui::detail::validFontConfiguration(width, height, font.pixels.size()))
                return lux::cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE, 0});
            std::vector<std::byte> prepared;
            prepared.reserve(16U + font.pixels.size());
            BinaryWriter writer(prepared);
            static_cast<void>(writer.writeUnsigned(width));
            static_cast<void>(writer.writeUnsigned(height));
            static_cast<void>(writer.writeUnsigned<std::uint64_t>(font.pixels.size()));
            static_cast<void>(writer.writeBytes(std::as_bytes(std::span(font.pixels))));
            output = std::move(prepared);
            return {};
        }

        SerializationResult decodeFont(std::span<const std::byte> input, void* object) noexcept
        {
            if (!object)
                return lux::cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE, 0});
            const auto source = readFont(input);
            if (!source)
                return lux::cxx::unexpected(source.error());
            ui::FontAtlas font;
            font.width = static_cast<int>(source->width);
            font.height = static_cast<int>(source->height);
            font.pixels.resize(source->pixels.size());
            std::memcpy(font.pixels.data(), source->pixels.data(), source->pixels.size());
            *static_cast<ui::FontAtlas*>(object) = std::move(font);
            return {};
        }

        SerializationResult materializeFont(std::span<const std::byte> input, std::vector<std::byte>& output) noexcept
        {
            const auto source = readFont(input);
            if (!source)
                return lux::cxx::unexpected(source.error());
            const UiRenderCommConfig header{source->width, source->height};
            std::vector<std::byte> prepared(sizeof(header) + source->pixels.size());
            std::memcpy(prepared.data(), &header, sizeof(header));
            std::memcpy(prepared.data() + sizeof(header), source->pixels.data(), source->pixels.size());
            output = std::move(prepared);
            return {};
        }
    }

    RenderFeatureConfigCodec UiRenderConfigurationCodec() noexcept
    {
        return {
            "lux.render.ui.v1.configuration",
            1,
            {lux::cxx::typeToken<ui::FontAtlas>(),
             +[](std::vector<std::byte>&) noexcept -> SerializationResult {
                 return lux::cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE, 0});
             },
             &encodeFont,
             &decodeFont},
            &materializeFont
        };
    }
}
