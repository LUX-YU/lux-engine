## Stable Engine Scene contracts shared by runtime composition, editor tooling,
## and offline scene-package producers. This layer owns scene semantics but no
## World, Schedule, renderer, or product lifecycle state.

add_subdirectory(api)

install_components(
    PROJECT_NAME    lux-engine-scene
    VERSION         0.0.1
    NAMESPACE       lux::engine::scene
    COMPONENTS      scene_api
)
