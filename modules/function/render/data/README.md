# Render values

`RenderPayload` retains immutable bytes and accounts for the complete source vector capacity,
including when only a subrange is exposed. `fromVector` does not deep-copy the asset. The host keeps
foreign owner/deleter code loaded until the final byte owner is released. `copyOf` is an explicit copy.

`ConfigValue` holds immutable typed configuration and any code pin needed by its destructor. Cold
comparison uses the complete C++ type and value contract. Dynamic configurations supply retained-byte
accounting for all backing capacity; they are not serialized through a Render-specific wire schema.

Shader contracts own Stage, Entry, SPIR-V and field/layout descriptions. Native checks the actual
binary against this declaration. These values contain no Vulkan handles and create no Shader compiler.
`description_types` installs canonical Shader/Image metadata; Render does not copy another module's headers.

Commands transfer C++ values through Platform IPC. Persistent asset encoding belongs to the existing
resource/asset codecs. The former unused Render Schema/Encoding implementation is not an installed API.
