add_executable(basic_lifecycle)
target_sources(basic_lifecycle PRIVATE basic_lifecycle.c)
target_link_libraries(basic_lifecycle PRIVATE udipe)
add_test(NAME basic_lifecycle COMMAND basic_lifecycle)
