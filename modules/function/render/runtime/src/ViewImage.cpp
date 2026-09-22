#include <lux/engine/render/detail/ViewImageLifetime.hpp>

namespace lux::render
{
ViewImageLease::ViewImageLease() noexcept = default;
ViewImageLease::~ViewImageLease() noexcept = default;
ViewImageLease::ViewImageLease(const ViewImageLease &) noexcept = default;
ViewImageLease &ViewImageLease::operator=(const ViewImageLease &) noexcept = default;
ViewImageLease::ViewImageLease(ViewImageLease &&) noexcept = default;
ViewImageLease &ViewImageLease::operator=(ViewImageLease &&) noexcept = default;
bool ViewImageLease::valid() const noexcept
{
    return record_ && record_->version;
}
} // namespace lux::render
