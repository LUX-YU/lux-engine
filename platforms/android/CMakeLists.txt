# =============================================================================
#  android/ — APK shell + bring-up harness.
#
#  This is app scaffolding, not an engine component: it produces the native
#  library that android.app.NativeActivity loads, and (via package.ps1) the APK
#  around it. Only processed when ANDROID is on — see the root CMakeLists.
#
#  Zero Java by construction: the activity class comes from the platform and
#  the entry point (ANativeActivity_onCreate) comes from the NDK's
#  native_app_glue, so nothing here needs a JDK, Gradle, or dex.
# =============================================================================

# native_app_glue ships WITH the NDK (sources/android/native_app_glue), unlike
# GameActivity which would have to be pulled as an AndroidX AAR through
# Gradle/Maven. That difference is the whole reason bring-up starts here: it is
# two files on disk versus standing up a second build system.
set(_glue_dir "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
if(NOT EXISTS "${_glue_dir}/android_native_app_glue.c")
    message(FATAL_ERROR
        "native_app_glue not found under '${_glue_dir}'. "
        "Expected it inside the NDK (CMAKE_ANDROID_NDK='${CMAKE_ANDROID_NDK}').")
endif()

add_library(lux_native_app_glue STATIC "${_glue_dir}/android_native_app_glue.c")
target_include_directories(lux_native_app_glue PUBLIC "${_glue_dir}")
# The glue is compiled into a shared library, so its objects need to be PIC.
set_target_properties(lux_native_app_glue PROPERTIES POSITION_INDEPENDENT_CODE ON)
lux_classify_target(
    TARGET  lux_native_app_glue
    LAYER   PLATFORM
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_library(luxsmoke SHARED smoke/smoke_main.cpp)
lux_classify_target(
    TARGET  luxsmoke
    LAYER   HOST
    PRODUCT TEST
    ROLE    COMPOSITION
)

target_link_libraries(luxsmoke
    PRIVATE
        lux_native_app_glue
        lux::engine::ecs::core   # ComponentTypeCatalog (gate [2])
        lux::engine::core::meta  # meta_module_init — drains the registrar list
        lux::engine::runtime::runtime_render_scene
        lux::engine::runtime::runtime_render_backend_host # shared channel/thread assembly
        lux::engine::runtime::runtime_frame # FrameCoordinator lexical-frame ownership
        vulkan                   # NDK-provided libvulkan (gate [3])
        render_features          # GeneralRenderServer + built-in feature factories
        lux::engine::platform::window  # requiredVulkanInstanceExtensions — the
                                       # sanctioned seam for the surface extension
                                       # list, even though the Android LuxWindow
                                       # deliberately owns no surface itself
        android
        "-llog"   # NDK liblog — quoted: a bare `log` now resolves to the
                  # engine's core::log TARGET, not the system library
)

# The reflection sidecars are SHARED libraries whose static registrars populate
# ComponentTypeCatalog. Nothing REFERENCES their symbols, so they are only
# present at runtime if something links them — link them here explicitly, or
# gate [2] would report an empty catalogue and we would be debugging the
# generator instead of the packaging.
foreach(_sidecar
        ecs_meta
        render_components_meta
        render_components_2d_meta
        render_components_3d_meta
        spatial2d_components_meta
        spatial3d_components_meta
        pixel_meta
        animation_2d_meta
        script_meta)
    if(TARGET ${_sidecar})
        target_link_libraries(luxsmoke PRIVATE ${_sidecar})
    endif()
endforeach()

# ANativeActivity_onCreate lives in the glue and is referenced by nobody in this
# library, so --gc-sections/--as-needed would drop it and the activity would
# fail to start with "unable to find ANativeActivity_onCreate". -u forces it to
# be kept as an undefined-then-resolved symbol.
target_link_options(luxsmoke PRIVATE "-u" "ANativeActivity_onCreate")

# ── luxgame — the GAME front end (NativeActivity → SceneRuntime) ─────────────
# The editor→binary loop's Android leg: boots the pak extracted from the APK's
# assets/ through the SAME SceneRuntime + standard feature plan the desktop
# player drives. The smoke harness above stays as the bring-up gauntlet.
add_library(luxgame SHARED game/game_main.cpp)
lux_classify_target(
    TARGET  luxgame
    LAYER   HOST
    PRODUCT PLAYER
    ROLE    COMPOSITION
)

target_link_libraries(luxgame
    PRIVATE
        lux_native_app_glue
        lux::engine::host::game_application # Runtime/scene/render/close owner
        lux::game::deployment                  # packaged launch manifest parser
        lux::engine::platform::window        # Vulkan extension list
        lux::engine::core::log               # Logcat process outlet
        android
        "-llog"
)

# Reflection sidecars: same rationale as luxsmoke — nothing references their
# symbols, so they must be linked to be present at runtime, or every entity
# loads hollow.
foreach(_sidecar
        ecs_meta
        render_components_meta
        render_components_2d_meta
        render_components_3d_meta
        spatial2d_components_meta
        spatial3d_components_meta
        pixel_meta
        animation_2d_meta
        script_meta)
    if(TARGET ${_sidecar})
        target_link_libraries(luxgame PRIVATE ${_sidecar})
    endif()
endforeach()

target_link_options(luxgame PRIVATE "-u" "ANativeActivity_onCreate")

# Build-graph-derived ELF inventories. The Android output directory can contain
# stale or unrelated .so files from earlier profiles; packaging that directory
# wholesale silently turns build history into product contents. Resolve only
# target edges reachable from each NativeActivity entrypoint. Conditional
# generator-expression branches may conservatively over-approximate, but an
# unreferenced build-tree library can no longer enter an APK by accident.
function(_lux_android_runtime_targets root output)
    set(queue "${root}")
    set(visited "")
    set(runtime_targets "")
    while(queue)
        list(POP_FRONT queue current)
        if(NOT TARGET "${current}" OR current IN_LIST visited)
            continue()
        endif()
        get_target_property(aliased "${current}" ALIASED_TARGET)
        if(aliased)
            set(current "${aliased}")
        endif()
        if(current IN_LIST visited)
            continue()
        endif()
        list(APPEND visited "${current}")

        get_target_property(type "${current}" TYPE)
        if(type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY|UNKNOWN_LIBRARY)$" AND
           NOT current STREQUAL root)
            list(APPEND runtime_targets "${current}")
        endif()

        get_target_property(direct "${current}" LINK_LIBRARIES)
        get_target_property(interface "${current}" INTERFACE_LINK_LIBRARIES)
        foreach(expression IN LISTS direct interface)
            _lux_arch_resolve_dependencies("${expression}" dependencies)
            list(APPEND queue ${dependencies})
        endforeach()
    endwhile()
    list(REMOVE_DUPLICATES runtime_targets)
    list(SORT runtime_targets)
    set(${output} "${runtime_targets}" PARENT_SCOPE)
endfunction()

foreach(android_entrypoint IN ITEMS luxsmoke luxgame)
    _lux_android_runtime_targets("${android_entrypoint}" runtime_targets)
    set(inventory
        "# Generated from ${android_entrypoint}'s configured target link closure.\n")
    foreach(runtime_target IN LISTS runtime_targets)
        string(APPEND inventory "$<TARGET_FILE_NAME:${runtime_target}>\n")
    endforeach()
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/bin/${android_entrypoint}.runtime-files"
        CONTENT "${inventory}"
    )
endforeach()
