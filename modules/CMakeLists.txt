foreach(layer IN ITEMS platform core resource function)
    add_subdirectory(${layer})
endforeach()
