/**
 * @file IVertexSource.cpp
 * @brief vtable anchor for IVertexSource — keeps the vtable from being
 *        emitted in every TU that includes the header.
 *
 * Stage R1.1 of render-refactor.
 */

#include <lux/engine/render/resources/vertex/IVertexSource.hpp>

namespace lux::render
{
    IVertexSource::~IVertexSource() = default;
}
