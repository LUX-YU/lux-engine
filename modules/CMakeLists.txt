foreach(layer IN ITEMS platform core)
    add_subdirectory(${layer})
endforeach()

# Script ABI/signature primitives are L0 foundation even though their physical
# package path remains under function/script for this migration.  Configure the
# core target before Resource descriptions so the latter can depend on the
# durable vocabulary without depending on a backend.
add_subdirectory(function/script/core)

foreach(layer IN ITEMS resource function)
    add_subdirectory(${layer})
endforeach()
