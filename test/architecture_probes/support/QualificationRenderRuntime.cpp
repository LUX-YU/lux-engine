#include "QualificationRenderRuntime.hpp"

#include "DeviceRenderFixture.hpp"

#include <new>

namespace lux::scene::qualification
{
    QualificationRenderRuntime::QualificationRenderRuntime(CreateInfo info) noexcept
        : fixture_(&info.fixture), meta_(&info.meta), factories_(info.factories)
    {
    }

    lux::cxx::expected<RenderRuntimeLease, RenderRuntimeFailure>
    QualificationRenderRuntime::acquire() noexcept
    {
        if (fixture_ == nullptr || meta_ == nullptr || !fixture_->ok())
        {
            return lux::cxx::unexpected(RenderRuntimeFailure{ERenderRuntimeError::ACTIVATION_FAILURE});
        }
        if (!catalog_ready_)
        {
            try
            {
                for (const auto* factory : factories_)
                {
                    const auto* static_meta = factory != nullptr
                        ? meta_->getRenderFeatureMeta(factory->descriptor.type)
                        : nullptr;
                    const bool invalid_factory = factory == nullptr || factory->name == nullptr ||
                        factory->name[0] == '\0' || static_meta == nullptr ||
                        static_meta->display_name != factory->descriptor.name;
                    if (invalid_factory)
                    {
                        return lux::cxx::unexpected(RenderRuntimeFailure{
                            ERenderRuntimeError::FEATURE_REGISTRATION_FAILURE
                        });
                    }
                    auto registered = fixture_->control().syncCall(
                        fixture_->control().registerFeatureType(*factory)
                    );
                    const bool registration_failed = !registered || registered->feature_type_id == 0U ||
                        !registered->error.ok();
                    if (registration_failed)
                    {
                        return lux::cxx::unexpected(RenderRuntimeFailure{
                            ERenderRuntimeError::FEATURE_REGISTRATION_FAILURE,
                            registered ? registered->error : render::RenderError{}
                        });
                    }
                    catalog_.add(
                        *factory,
                        registered->feature_type_id,
                        std::span(registered->ops, registered->op_count)
                    );
                }
                catalog_ready_ = true;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(RenderRuntimeFailure{ERenderRuntimeError::ACTIVATION_FAILURE});
            }
            catch (...)
            {
                return lux::cxx::unexpected(RenderRuntimeFailure{ERenderRuntimeError::ACTIVATION_FAILURE});
            }
        }
        ++lease_count_;
        return makeLease();
    }

    std::size_t QualificationRenderRuntime::leaseCount() const noexcept
    {
        return lease_count_;
    }

    bool QualificationRenderRuntime::catalogReady() const noexcept
    {
        return catalog_ready_;
    }

    void QualificationRenderRuntime::release() noexcept
    {
        (void)fixture_->control().flushDeferredReleases();
        if (lease_count_ != 0U)
        {
            --lease_count_;
        }
    }

    render::RenderControlSession& QualificationRenderRuntime::control() noexcept
    {
        return fixture_->control();
    }

    render::RenderProgramSession& QualificationRenderRuntime::programs() noexcept
    {
        return fixture_->session();
    }

    render::RenderUploadClient QualificationRenderRuntime::upload() noexcept
    {
        return fixture_->uploadClientForTest();
    }

    const render::FeatureCatalog& QualificationRenderRuntime::features() const noexcept
    {
        return catalog_;
    }
} // namespace lux::scene::qualification
