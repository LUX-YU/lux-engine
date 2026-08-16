if(NOT TARGET toolchain_asset_cook)
    add_subdirectory(cook)
endif()
if(NOT TARGET toolchain_texture_importer)
    add_subdirectory(texture)
endif()
add_subdirectory(material)
add_subdirectory(builtin)
add_subdirectory(model)

# In a normal host build the repository root configures this early because
# Function/Render consumes its generated files. Keep the Toolchain subtree
# independently composable without creating a second target.
if(NOT TARGET lux_asset_packer)
    add_subdirectory(packer)
endif()

if(ENABLE_ASSET_TEST)
    add_subdirectory(test)
endif()
