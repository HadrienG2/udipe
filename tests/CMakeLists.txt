add_executable(unit_tests)
target_sources(unit_tests PRIVATE unit_tests.c)
target_link_libraries(unit_tests PRIVATE udipe)
add_test(NAME unit_tests COMMAND unit_tests)
