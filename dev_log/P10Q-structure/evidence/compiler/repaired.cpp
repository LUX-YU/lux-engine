#include <lux/engine/editor/persistence/ArtifactStore.hpp>
template<class T> struct Policy { static constexpr auto bytes = sizeof(T); };
auto instantiated = Policy<lux::editor::persistence::CommitReceipt>::bytes;
