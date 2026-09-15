#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/editor/DocumentIdentity.hpp>
#include <lux/engine/editor/documents/visibility.h>
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <string>

namespace lux::editor::scene
{
enum class EObjectSpace : std::uint8_t
{
    NONE,
    SPACE_2D,
    SPACE_3D
};

enum class ESceneStructureError : std::uint8_t
{
    INVALID_OBJECT,
    INVALID_PARTITION,
    MISSING_PROVIDER,
    REFERENCE_IN_USE,
    CODEC_FAILURE,
    HIERARCHY_UNSUPPORTED,
    HIERARCHY_CYCLE
};

enum class EModelPlacementError : std::uint8_t
{
    UNSUPPORTED_SCENE,
    INVALID_PARTITION,
    INVALID_TRANSFORM,
    UNSUPPORTED_DEFORMATION,
    NON_TRS_TRANSFORM,
    MISSING_DEPENDENCY,
    IDENTITY_CONFLICT
};

struct SceneWriteTarget final
{
    DocumentHandle document;
    lux::world::WorldObjectId object;
    editing::StateId state;
    editing::Revision revision;
};

struct PreviewToken final
{
    DocumentHandle document;
    std::uint64_t sequence{};
    std::string origin;

    friend bool operator==(const PreviewToken &, const PreviewToken &) = default;
};

struct ComponentNotice final
{
    lux::world::WorldObjectId object;
    lux::cxx::TypeToken component;
    editing::Revision revision;
    bool preview{};
    std::uint64_t sequence{};
};

class SceneEditor;

namespace detail
{
// One retained operation also owns the active gesture's original and latest
// legal value. It moves directly into EditHistory on commit; it never wraps
// another history.
class LUX_EDITOR_DOCUMENTS_PUBLIC SceneFieldEdit : public editing::EditOperation
{
  public:
    ~SceneFieldEdit() override;
    [[nodiscard]] virtual editing::EditResult<void> update(lux::cxx::TypeToken, const void *) = 0;
    [[nodiscard]] virtual editing::EditResult<void> cancel() noexcept = 0;
};

template <class Component, class Value, class Access> class FieldEdit;
} // namespace detail
} // namespace lux::editor::scene
