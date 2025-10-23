add_executable(context_lifecycle)
target_sources(context_lifecycle PRIVATE context_lifecycle.c)
target_link_libraries(context_lifecycle PRIVATE udipe)

add_executable(custom_allocator)
target_sources(custom_allocator PRIVATE custom_allocator.c)
target_link_libraries(custom_allocator PRIVATE udipe)

add_executable(log_to_tempfile)
target_sources(log_to_tempfile PRIVATE log_to_tempfile.c)
target_link_libraries(log_to_tempfile PRIVATE udipe)

if(UDIPE_BUILD_TESTS)
    add_test(NAME context_lifecycle COMMAND context_lifecycle)
    add_test(NAME custom_allocator COMMAND custom_allocator)
    add_test(NAME log_to_tempfile COMMAND log_to_tempfile)
endif()
