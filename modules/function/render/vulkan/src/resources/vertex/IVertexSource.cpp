/**
 * @file IVertexSource.cpp
 * @brief vtable anchor for IVertexSource — keeps the vtable from being
 *        emitted in every TU that includes the header.
 *
 * Stage R1.1 of render-refactor.
 */

#include <lux/engine/render/resources/vertex/IVertexSource.hpp>
#include <lux/engine/render/resources/vertex/VertexRegistration.hpp>

namespace lux::render
{
    IVertexSource::~IVertexSource()
    {
        if (registration_)
        {
            registration_->revoke(true);
        }
    }
} // namespace lux::render
