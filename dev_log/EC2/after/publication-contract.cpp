#include <lux/engine/editor/storage/PreparedProjectOpen.hpp>
#include <type_traits>
using namespace lux::editor;
static_assert(!std::is_copy_constructible_v<PreparedProjectPublication>);
static_assert(!std::is_copy_assignable_v<PreparedProjectPublication>);
static_assert(std::is_nothrow_move_constructible_v<PreparedProjectPublication>);
static_assert(std::is_nothrow_move_assignable_v<PreparedProjectPublication>);
static_assert(!std::is_copy_constructible_v<PreparedProjectOpen>);
static_assert(std::is_nothrow_move_assignable_v<PreparedProjectOpen>);
#if defined(ILLEGAL_MANIFEST)
void modify(PreparedProjectPublication& prepared) { prepared.plan().manifest().name = "changed"; }
#elif defined(ILLEGAL_BYTES)
void modify(PreparedProjectPublication& prepared) { prepared.plan().manifest_bytes_ = {}; }
#elif defined(ILLEGAL_OPEN)
void modify(PreparedProjectOpen& prepared) { prepared.manifest().name = "changed"; }
#endif
