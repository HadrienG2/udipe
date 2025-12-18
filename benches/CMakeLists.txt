add_executable(micro)
target_sources(micro PRIVATE micro.c)
target_link_libraries(micro PRIVATE udipe)
