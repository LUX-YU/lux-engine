include_guard(GLOBAL)
include(CMakePackageConfigHelpers)

# Reuse the toolset's component metadata, headers and asset helpers. Package destinations
# must be relative: an absolute install(EXPORT) destination embeds the original prefix.
function(lux_engine_install_components)
    cmake_parse_arguments(PACKAGE "" "PROJECT_NAME;VERSION;NAMESPACE" "COMPONENTS" ${ARGN})
    if(NOT PACKAGE_PROJECT_NAME OR NOT PACKAGE_NAMESPACE OR NOT PACKAGE_COMPONENTS)
        message(FATAL_ERROR "lux_engine_install_components requires PROJECT_NAME, NAMESPACE and COMPONENTS")
    endif()
    if(NOT PACKAGE_VERSION)
        set(PACKAGE_VERSION 0.0.0)
    endif()
    set(package_directory "share/${PACKAGE_PROJECT_NAME}")
    string(REPLACE "-" "_" __PACKAGE_CPACK_COMPONENT__ "${PACKAGE_PROJECT_NAME}")

    foreach(component IN LISTS PACKAGE_COMPONENTS)
        get_target_property(export_name ${component} EXPORT_NAME)
        set(COMPONENT_TARGET "${PACKAGE_NAMESPACE}::${export_name}")
        set(COMPONENT_NAME "${component}")
        set(COMPONENT_DEPENDENCIES)
        set(COMPONENT_TRANSITIVE_COMMANDS)
        set(COMPONENT_EXPORT_PROPERTIES)
        __install_assets(${component} "${package_directory}/${component}")
        __install_export_header_dirs(${component} ".")
        __generate_transitive_commands(${component} COMPONENT_TRANSITIVE_COMMANDS)
        __generate_export_properties_commands(${component} COMPONENT_EXPORT_PROPERTIES)
        get_target_property(COMPONENT_ASSET_TYPES ${component} EXPORT_ASSET_TYPES)
        if(NOT COMPONENT_ASSET_TYPES)
            set(COMPONENT_ASSET_TYPES)
        endif()
        get_target_property(dependencies ${component} INTERNAL_DEPENDENCIES)
        if(dependencies)
            foreach(dependency IN LISTS dependencies)
                get_target_property(original ${dependency} ALIASED_TARGET)
                if(original)
                    list(APPEND COMPONENT_DEPENDENCIES "${original}")
                else()
                    list(APPEND COMPONENT_DEPENDENCIES "${dependency}")
                endif()
            endforeach()
        endif()

        set(import_file "${PACKAGE_PROJECT_NAME}-${component}-import.cmake")
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ComponentImport.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${import_file}" @ONLY)
        install(EXPORT ${export_name}
            DESTINATION "${package_directory}/${component}"
            COMPONENT ${__PACKAGE_CPACK_COMPONENT__}
            NAMESPACE ${PACKAGE_NAMESPACE}::
            FILE "${PACKAGE_PROJECT_NAME}-${component}-config-targets.cmake")
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${import_file}"
            DESTINATION "${package_directory}/${component}" COMPONENT ${__PACKAGE_CPACK_COMPONENT__})
        install(TARGETS ${component} EXPORT ${export_name} COMPONENT ${__PACKAGE_CPACK_COMPONENT__})
    endforeach()

    configure_package_config_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PackageConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${PACKAGE_PROJECT_NAME}-config.cmake"
        INSTALL_DESTINATION "${package_directory}")
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/${PACKAGE_PROJECT_NAME}-config-version.cmake"
        VERSION ${PACKAGE_VERSION} COMPATIBILITY SameMajorVersion)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${PACKAGE_PROJECT_NAME}-available-components.cmake"
        "set(${PACKAGE_PROJECT_NAME}_AVAILABLE_COMPONENTS ${PACKAGE_COMPONENTS})\n")
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${PACKAGE_PROJECT_NAME}-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PACKAGE_PROJECT_NAME}-config-version.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PACKAGE_PROJECT_NAME}-available-components.cmake"
        DESTINATION "${package_directory}" COMPONENT ${__PACKAGE_CPACK_COMPONENT__})
endfunction()
