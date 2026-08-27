add_subdirectory(platform)

# Script ABI/signature primitives are L0 foundation even though their physical
# package path remains under function/script for this migration.  Configure the
# target before Core's cold meta adapter and Resource descriptions so they can
# depend on the durable vocabulary without depending on a backend.
add_subdirectory(function/script/core)
add_subdirectory(core)

foreach(layer IN ITEMS resource function)
    add_subdirectory(${layer})
endforeach()
