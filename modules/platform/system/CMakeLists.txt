add_module(
    MODULE_NAME         system
    NAMESPACE           platform
    SOURCE_FILES        src/memory_pool.cpp
                        src/thread_pool.cpp
                        src/Introspection.cpp
    EXPORT_INCLUDE_DIRS include
)
