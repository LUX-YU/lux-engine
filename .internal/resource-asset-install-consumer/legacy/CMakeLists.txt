cmake_minimum_required(VERSION 3.25)
project(resource_asset_legacy_lookup LANGUAGES CXX)

set(LEGACY_COMPONENT asset_core CACHE STRING "Retired Asset component to probe")
find_package(lux-engine-resource REQUIRED COMPONENTS ${LEGACY_COMPONENT})
