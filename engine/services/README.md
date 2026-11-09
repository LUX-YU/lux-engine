# Engine services

`assets` owns asset identity, CPU data and registered uses. It depends on Execution and resource contracts, not Render.
`context` declares the process facilities available to Systems. The owning implementation is in runtime/initialization.
`world_storage` contains partition codecs and IO; `serialization` contains World/Simulation/Scene asset encoding.

Concrete systems consume these services. No service may depend on Editor or a concrete Render/Script System.
