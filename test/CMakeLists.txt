message(STATUS "---\n*** tests ***")

if (BRIDGE)
    add_executable(shm_test "shm_test.cpp")
    target_link_libraries(shm_test vst)
    target_include_directories(shm_test PUBLIC "../deps")
endif()

add_executable(normalize_path "normalize_path.cpp")
target_link_libraries(normalize_path vst)

add_executable(hashtable_test "hashtable_test.cpp")
target_link_libraries(hashtable_test vst_public)
