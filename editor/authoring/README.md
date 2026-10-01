# Authoring

Scene, Material, Flow, Project and Layout contain pure author values, validation and codecs.
The three sessions reuse Editing's History and SessionState. Process, runtime instances, UI,
compiler and physical file publication are outside this layer.

ProjectBuilder produces ProjectBuildConfig; it does not create files. ProjectCatalogModel is the
single immutable catalog provider. RecoveryManifest and layout plans never open author content.
