foreach(source IN LISTS SOURCES)
    file(READ "${source}" content)
    if(content MATCHES
       "(label|hint|format|preview|option|spec[.]id[.]name[(][)]|item[.]label[(][)])[.]data[(][)]")
        message(FATAL_ERROR "UI backend passes a non-owning string_view as a C string: ${source}")
    endif()
endforeach()
