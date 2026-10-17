# N6 Feature dependency installation checkpoint

Parent f0888f0705e82ace81145ea4a2070042cb778b52. Provider::Storages optionally declares a tuple of
actual typed Storage dependencies. The existing RenderServer installation stages their Factory,
configuration and Handler calls before publishing anything. ResourceRegistry batch qualification uses
its sole type table; no Native Storage instance is constructed during directory installation.

All recoverable Feature, Command, canonical TypeOps, Factory/configuration and device capability
checks precede publication. Failed staging destroys only unpublished candidates. Successful publication
uses the existing Handler owner table and Command routes. A Handler shared by several dependencies or
the Feature is constructed once, retains a stable address and is destroyed once. Commands published by
an earlier dependency in this same transaction are reused; an existing active selection is not changed.
Default Feature requirements honour a compatible explicit Storage configuration instead of replacing it.
Direct explicit registerStorage still rejects a conflicting configuration. Cold comparisons are not
added to request dispatch or frame recording.

StorageInstall and StorageFactoryInstall are transient installation values, not another catalog or
runtime Owner. No IPC, Queue, GPU completion, allocation retirement or draw algorithm changes.

New real Server/Client test covers late device capability failure, invalid Feature after dependency
staging, conflicting Command types, repeated dependency and Feature installation, explicit configuration
and active Handler preservation, and sharing the Feature Handler with a dependency. Failed cases expose
no partial Storage/Feature/Command directory. Exact constructor/destructor counters prove unique ownership.
The test's initial compile error omitted the explicit Ack argument; it was corrected before runtime
validation and the failed build log is retained.

Windows implementation all -j4 -- -k0 and second no-work pass. Full serial Release CTest141/141148.51s;
prior140 names retained. Protection checker passes719 frozen blobs and3 user files. Independent exact-I
ASan/installation verification follows separately. BasicMesh/DepthOnly production contributions and
three-dimensional integration remain in progress. N7 final R/D/ASan and performance work are not waived.
Historical performance/IPC PARTIAL, Root product BLOCKED and upper/platform NOT_RUN remain unchanged.
