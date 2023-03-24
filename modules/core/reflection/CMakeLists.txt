find_package(Clang REQUIRED)

set(LuxParserSrcs
    src/ClangParser.cpp
    src/LuxCxxParser.cpp
    src/LuxCxxParserImpl.cpp
)

add_module(
    MODULE_NAME             reflection
    NAMESPACE               core
    SOURCE_FILES            ${LuxParserSrcs}
    EXPORT_INCLUDE_DIRS     include
    PRIVATE_INCLUDE_DIRS    ${CLANG_INCLUDE_DIRS}
                            pinclude
    PUBLIC_LIBRARIES        libclang
                            lux::engine::platform::system
                            lux::engine::core::meta
)
