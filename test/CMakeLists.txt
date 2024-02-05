if (BRIDGE)
    add_executable(shm_test "shm_test.cpp")
    target_link_libraries(shm_test ${LIBS})

    add_executable(normalize_path "normalize_path.cpp")
    target_link_libraries(normalize_path ${LIBS})

    add_executable(hashtable_test "hashtable_test.cpp")
endif()
