# Legacy quarantine

`legacy/ecs`, `legacy/engine`, and `legacy/modules/resource/asset-runtime` are
retained only as implementation references during the vNext L1 rebuild.
Nothing in this tree is configured, compiled, installed, linked, packaged,
scanned by code generation, or included by new production code.

Do not add compatibility headers or targets that point back into this tree.
After the new L1 and the first L3 headless Scene have passed the freeze audit,
the quarantine may be deleted as a separate, explicit change.
