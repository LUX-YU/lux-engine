#include <lux/engine/editor/assets/ModelImportRecipe.hpp>
#include <cassert>
#include <limits>

using namespace lux::editor::assets;

int main()
{
    ModelImportRecipe original{"Sources/fixed", "mesh.obj", {}, {{"mesh.obj", std::string(64, 'a')}}};
    original.configuration.uniform_scale = 2.0F;
    original.configuration.make_left_handed = true;
    original.configuration.import_animations = false;
    original.configuration.pre_rotation = Eigen::AngleAxisf(0.5F, Eigen::Vector3f::UnitY());
    const auto encoded = encodeModelImportRecipe(original);
    assert(encoded);
    const auto decoded = decodeModelImportRecipe(*encoded);
    assert(decoded);
    assert(decoded->root == original.root && decoded->entry == original.entry);
    assert(decoded->files.size() == 1 && decoded->files[0].path == "mesh.obj");
    assert(decoded->files[0].digest == original.files[0].digest);
    assert(decoded->configuration.uniform_scale == 2.0F && decoded->configuration.make_left_handed);
    assert(!decoded->configuration.import_animations);
    assert(decoded->configuration.pre_rotation.coeffs().isApprox(original.configuration.pre_rotation.coeffs()));
    assert(*encodeModelImportRecipe(*decoded) == *encoded);
    auto invalid = original;
    invalid.files.push_back({"Mesh.obj", std::string(64, 'b')});
    assert(encodeModelImportRecipe(invalid).error().domain == "model.recipe.duplicate-path");
    invalid = original;
    invalid.root = "../outside";
    assert(!encodeModelImportRecipe(invalid));
    invalid = original;
    invalid.entry = "missing.obj";
    assert(encodeModelImportRecipe(invalid).error().domain == "model.recipe.entry");
    invalid = original;
    invalid.files[0].digest[0] = 'g';
    assert(encodeModelImportRecipe(invalid).error().domain == "model.recipe.files");
    invalid = original;
    invalid.configuration.uniform_scale = std::numeric_limits<float>::infinity();
    assert(encodeModelImportRecipe(invalid).error().domain == "model.recipe.scale");
    invalid = original;
    invalid.configuration.pre_rotation.coeffs().setZero();
    assert(encodeModelImportRecipe(invalid).error().domain == "model.recipe.rotation");

    // Existing persisted schema remains readable, with strict unknown-field/shape rejection.
    const std::string prefix =
        "format='lux.editor.model-source'\nversion=1\nroot='Sources/fixed'\nentry='mesh.obj'\n"
        "scale=1.0\nleft_handed=false\nanimations=true\nrotation=[0.0,0.0,0.0,1.0]\n";
    const std::string files = "[[files]]\npath='mesh.obj'\ndigest='" + std::string(64, 'a') + "'\n";
    auto read = [](const std::string& value) { return decodeModelImportRecipe(std::as_bytes(std::span(value))); };
    assert(read(prefix + files));
    assert(read(prefix + "future=1\n" + files).error().domain == "model.recipe.unknown-field");
    assert(read(prefix + files + "extra=1\n").error().domain == "model.recipe.files");
    auto overflow = prefix;
    overflow.replace(overflow.find("scale=1.0"), 9, "scale=1e100");
    assert(read(overflow + files).error().domain == "model.recipe.scale");
    auto wrong_version = prefix;
    wrong_version.replace(wrong_version.find("version=1"), 9, "version=2");
    assert(read(wrong_version + files).error().domain == "model.recipe.schema");
    assert(!read("invalid toml"));
}
