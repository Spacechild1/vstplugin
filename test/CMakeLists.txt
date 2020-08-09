cmake_minimum_required (VERSION 2.8)

if (BRIDGE)
    set(SHM_TEST "shm_test")
    add_executable(${SHM_TEST} "${SHM_TEST}.cpp")
    target_link_libraries(${SHM_TEST} ${LIBS})
endif()
