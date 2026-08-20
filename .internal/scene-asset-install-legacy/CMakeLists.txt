cmake_minimum_required(VERSION 3.25)
project(scene_asset_install_legacy LANGUAGES CXX)

if(NOT DEFINED LEGACY_COMPONENT)
    message(FATAL_ERROR "LEGACY_COMPONENT is required")
endif()

find_package(
    lux-engine-scene
    REQUIRED
    COMPONENTS ${LEGACY_COMPONENT}
)
