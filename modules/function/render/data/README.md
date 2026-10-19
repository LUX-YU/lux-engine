# Render values

Upload requests borrow immutable source bytes. Client keeps the source address, extent and contents valid
until the Response, including failure or cancellation. A failed submit takes no borrow. Abandoning a request
or closing its link does not prove that CPU reading has ended: the Host must drain Server and Producers
before releasing inputs that have no Response. Native owns staging and submitted GPU resources.

`ConfigValue` exclusively owns an immutable typed configuration and is move-only. Assembly borrows it
synchronously; installation owns its defaults and concrete Storage copies only the fields it needs.
Dynamic configurations account for their own allocation capacity, excluding externally borrowed data.
The Host keeps implementation/deleter code loaded until all values and callbacks have been destroyed.
Configurations are not serialized through a Render-specific wire schema.

Shader contracts own Stage, Entry, SPIR-V and field/layout descriptions. Native checks the actual
binary against this declaration. These values contain no Vulkan handles and create no Shader compiler.
`description_types` installs canonical Shader/Image metadata; Render does not copy another module's headers.

Commands transfer C++ values through Platform IPC. Persistent asset encoding belongs to the existing
resource/asset codecs. The former unused Render Schema/Encoding implementation is not an installed API.
