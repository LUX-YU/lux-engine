#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>

namespace lux::engine::render
{
    // Pure data upload. Definition fixes the implementation scope; no asset identity, cache or IO.
    // Repository registration borrows this object's address until repository retirement. Move only before
    // registration. Destruction is inert: it releases neither resources nor a borrowed Client.
    template <class Definition> class RenderUpload final
    {
    public:
        using Input = typename Definition::Input;
        using Output = typename Definition::Output;
        using CreateResult = lux::render::RenderResult<RenderUpload>;

        // IDs are resolved together during asynchronous professional assembly, never during upload.
        [[nodiscard]] static CreateResult create(
            lux::render::RenderClient& client,
            lux::render::CapabilityImplementationId implementation,
            lux::render::CommandId upload,
            lux::render::CommandId release
        ) noexcept
        {
            const bool invalid_commands = upload.isNull() || release.isNull();
            if (invalid_commands)
            {
                return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
            }
            return RenderUpload{client, implementation, upload, release};
        }

        [[nodiscard]] auto upload(const Input& data) noexcept
        {
            return client_.template request<typename Definition::UploadCommand>(
                upload_id_, Definition::uploadRequest(implementation_, data)
            ) | stdexec::then([](auto response) noexcept -> lux::render::RenderResult<Output>
            {
                if (response.error.type != 0) return cxx::unexpected(response.error);
                return Definition::result(response);
            });
        }

        [[nodiscard]] auto release(Output resource) noexcept
        {
            return client_.template request<typename Definition::ReleaseCommand>(
                release_id_, Definition::releaseRequest(resource)
            ) | stdexec::then([](auto response) noexcept -> lux::render::RenderResult<void>
            {
                if (response.error.type != 0) return cxx::unexpected(response.error);
                return {};
            });
        }

    private:
        RenderUpload(
            lux::render::RenderClient& client,
            lux::render::CapabilityImplementationId implementation,
            lux::render::CommandId upload, lux::render::CommandId release
        ) noexcept
            : client_(client), implementation_(implementation), upload_id_(upload), release_id_(release)
        {
        }

        lux::render::RenderClient& client_;
        lux::render::CapabilityImplementationId implementation_;
        lux::render::CommandId upload_id_;
        lux::render::CommandId release_id_;
    };
}
