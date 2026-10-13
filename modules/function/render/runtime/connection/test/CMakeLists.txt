add_executable(render_connection_test Connection.cpp)

target_link_libraries(render_connection_test PRIVATE render_connection)

foreach(test contracts ownership races progress cost)
    add_test(NAME render.connection.${test} COMMAND render_connection_test ${test})
    set_tests_properties(render.connection.${test} PROPERTIES TIMEOUT 45)
endforeach()

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/connection-dependencies.txt"
    CONTENT "links=$<TARGET_PROPERTY:render_connection,LINK_LIBRARIES>\n")
