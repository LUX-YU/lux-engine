## Runtime extension infrastructure is split by responsibility:
## - loader owns binary loading, ABI validation and code lifetime;
## - contribution_host owns runtime registration transactions and per-domain
##   activation. Concrete extension modules depend on these public seams;
##   neither target depends on a concrete extension.

add_subdirectory(loader)
add_subdirectory(contribution_host)
