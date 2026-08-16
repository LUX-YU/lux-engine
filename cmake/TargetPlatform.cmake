include_guard(GLOBAL)

function(_lux_normalize_os raw_value output)
    string(TOUPPER "${raw_value}" value)
    if(value STREQUAL "WINDOWS")
        set(result WINDOWS)
    elseif(value STREQUAL "LINUX")
        set(result LINUX)
    elseif(value STREQUAL "DARWIN")
        set(result MACOS)
    elseif(value STREQUAL "ANDROID")
        set(result ANDROID)
    else()
        message(FATAL_ERROR
            "Unsupported operating system '${raw_value}'. "
            "Supported systems are Windows, Linux, Darwin and Android."
        )
    endif()
    set(${output} "${result}" PARENT_SCOPE)
endfunction()

function(_lux_normalize_arch raw_value output)
    string(TOLOWER "${raw_value}" value)
    if(value MATCHES "^(amd64|x86_64)$")
        set(result X64)
    elseif(value MATCHES "^(arm64|arm64-v8a|aarch64)$")
        set(result ARM64)
    elseif(value MATCHES "^(x86|i[3-6]86)$")
        set(result X86)
    elseif(value MATCHES "^(arm|armeabi-v7a|armv7|armv7-a)$")
        set(result ARM32)
    else()
        message(FATAL_ERROR
            "Unsupported architecture '${raw_value}'. "
            "Supported architectures are x64, x86, arm64 and arm32."
        )
    endif()
    set(${output} "${result}" PARENT_SCOPE)
endfunction()

function(lux_detect_build_platform)
    _lux_normalize_os("${CMAKE_HOST_SYSTEM_NAME}" host_os)
    _lux_normalize_os("${CMAKE_SYSTEM_NAME}" target_os)

    set(target_processor "${CMAKE_SYSTEM_PROCESSOR}")
    if(target_os STREQUAL "ANDROID" AND CMAKE_ANDROID_ARCH_ABI)
        set(target_processor "${CMAKE_ANDROID_ARCH_ABI}")
    endif()
    _lux_normalize_arch("${CMAKE_HOST_SYSTEM_PROCESSOR}" host_arch)
    _lux_normalize_arch("${target_processor}" target_arch)

    set(LUX_HOST_OS "${host_os}" PARENT_SCOPE)
    set(LUX_HOST_ARCH "${host_arch}" PARENT_SCOPE)
    set(LUX_TARGET_OS "${target_os}" PARENT_SCOPE)
    set(LUX_TARGET_ARCH "${target_arch}" PARENT_SCOPE)
    set(LUX_IS_CROSS_BUILD "${CMAKE_CROSSCOMPILING}" PARENT_SCOPE)
endfunction()

function(lux_validate_profile_platform profile)
    set(host_only_profiles DEVELOPER EDITOR TOOLCHAIN)
    if(profile IN_LIST host_only_profiles AND LUX_IS_CROSS_BUILD)
        message(FATAL_ERROR
            "LUX_BUILD_PROFILE=${profile} is a host-native product and may not "
            "be cross-compiled (${LUX_HOST_OS}/${LUX_HOST_ARCH} -> "
            "${LUX_TARGET_OS}/${LUX_TARGET_ARCH}). Use PLAYER for target builds."
        )
    endif()

    if(profile IN_LIST host_only_profiles AND
       NOT LUX_TARGET_OS MATCHES "^(WINDOWS|LINUX|MACOS)$")
        message(FATAL_ERROR
            "LUX_BUILD_PROFILE=${profile} requires a desktop target; got "
            "${LUX_TARGET_OS}/${LUX_TARGET_ARCH}."
        )
    endif()
endfunction()

function(lux_write_build_facts profile)
    set(capabilities "")
    foreach(capability IN ITEMS RUNTIME REFERENCE_PLAYER EDITOR TOOLCHAIN)
        if(LUX_PROFILE_HAS_${capability})
            list(APPEND capabilities "${capability}")
        endif()
    endforeach()
    if(capabilities)
        list(JOIN capabilities "," capability_text)
    else()
        set(capability_text "NONE")
    endif()

    # A configure-time audit record, not an input. Keeping all three axes on
    # separate lines makes CI/profile comparisons mechanical and prevents an
    # OS-shaped product profile from hiding in a preset name.
    string(CONCAT facts
        "schema=1\n"
        "profile=${profile}\n"
        "host_os=${LUX_HOST_OS}\n"
        "host_arch=${LUX_HOST_ARCH}\n"
        "target_os=${LUX_TARGET_OS}\n"
        "target_arch=${LUX_TARGET_ARCH}\n"
        "cross_build=${LUX_IS_CROSS_BUILD}\n"
        "capabilities=${capability_text}\n"
    )
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/lux-build-facts.txt"
        CONTENT "${facts}"
    )
endfunction()
