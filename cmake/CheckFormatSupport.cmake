#[[
    @file CheckFormatSupport.cmake
    @brief Detects compiler support for std::format
    If not supported, use libfmt as fallback
]]

include(CheckCXXSourceCompiles)

#[[
    @brief Function to check std::format support
]]
function(check_format_support)
    # Save current CMAKE_REQUIRED_FLAGS
    set(CMAKE_REQUIRED_FLAGS_BACKUP ${CMAKE_REQUIRED_FLAGS})
    
    # Set C++20 standard
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
    
    # Test if std::format is available
    check_cxx_source_compiles("
        #include <format>
        #include <string>
        int main() {
            std::string result = std::format(\"Hello, {}!\", \"World\");
            return 0;
        }
    " HAVE_STD_FORMAT)
    
    # Restore CMAKE_REQUIRED_FLAGS
    set(CMAKE_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS_BACKUP})
    
    if(HAVE_STD_FORMAT)
        message(STATUS "std::format is supported by the compiler")
        set(USE_STD_FORMAT ON PARENT_SCOPE)
        set(USE_LIBFMT OFF PARENT_SCOPE)
    else()
        message(STATUS "std::format is not supported, looking for libfmt...")
        
        # Try to find libfmt
        find_package(fmt QUIET)
        
        if(fmt_FOUND)
            message(STATUS "Found libfmt, using it as std::format replacement")
            set(USE_STD_FORMAT OFF PARENT_SCOPE)
            set(USE_LIBFMT ON PARENT_SCOPE)
            set(FMT_TARGET fmt::fmt PARENT_SCOPE)
        else()
            message(STATUS "libfmt not found, trying to use system package...")

            # Try pkg-config approach
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(FMT QUIET fmt)
                if(FMT_FOUND)
                    message(STATUS "Found libfmt via pkg-config")
                    set(USE_STD_FORMAT OFF PARENT_SCOPE)
                    set(USE_LIBFMT ON PARENT_SCOPE)
                    set(FMT_LIBRARIES ${FMT_LIBRARIES} PARENT_SCOPE)
                    set(FMT_INCLUDE_DIRS ${FMT_INCLUDE_DIRS} PARENT_SCOPE)
                else()
                    message(WARNING "Neither std::format nor libfmt found. You may need to install libfmt-dev")
                    set(USE_STD_FORMAT OFF PARENT_SCOPE)
                    set(USE_LIBFMT OFF PARENT_SCOPE)
                endif()
            else()
                message(WARNING "Neither std::format nor libfmt found. You may need to install libfmt-dev")
                set(USE_STD_FORMAT OFF PARENT_SCOPE)
                set(USE_LIBFMT OFF PARENT_SCOPE)
            endif()
        endif()
    endif()
endfunction()
