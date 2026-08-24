## Runtime extension infrastructure is split by responsibility:
## - loader owns binary loading, ABI validation and code lifetime;
## - orchestration owns the atomic metadata transaction and module activation.
## Concrete extension modules depend only on the public ABI seam.

add_subdirectory(loader)
add_subdirectory(orchestration)
