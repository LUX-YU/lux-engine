# N6 material tool dependency boundary

The offline fixture consumes the existing MaterialGraph/lowerMaterial and ShaderEmitter. No second
MaterialGraph/parser/compiler is introduced. Only modules/function/material/CMakeLists.txt is added
to the prior whitelist: replace broad resource::description linkage with its actually consumed neutral
headers, retaining real graph/identity/object dependencies and all original tests. Installed graph gets
its required MaterialEnums header. Bootstrap selects those real source dependencies and tests; no
placeholder target or old SDK source substitutes. The previous checker and reports remain unchanged;
check_material_protection.py preserves its checks and adds exactly that CMake file.

Three shader includes are verbatim extractions from frozen Legacy, with source/destination SHA in
assets/material/provenance.json. No Legacy source is modified or linked. This is the bounded Unlit
build-time fixture dependency, not authorization to refactor the full material tool product.
