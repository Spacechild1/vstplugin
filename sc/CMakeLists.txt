set(PROJECT "VSTPlugin")
message(STATUS "---\n*** ${PROJECT} ***")

set(SRC "src/${PROJECT}.h" "src/${PROJECT}.cpp")

include_directories(src)

# SuperCollider API:
set(SC_INCLUDEDIR "" CACHE PATH "Path to SuperCollider respository")
message(STATUS "SC_INCLUDEDIR: ${SC_INCLUDEDIR}")
include_directories(${SC_INCLUDEDIR}/include/plugin_interface)
include_directories(${SC_INCLUDEDIR}/include/common)
include_directories(${SC_INCLUDEDIR}/plugin_interface)
include_directories(${SC_INCLUDEDIR}/common)

# Installation path
# install directory:
if (WIN32)
    set(SC_INSTALLDIR "${LOCALAPPDATA}/SuperCollider/Extensions/" CACHE PATH "Installation directoy")
elseif(APPLE)
    set(SC_INSTALLDIR "~/Library/Application Support/SuperCollider/Extensions/" CACHE PATH "Installation directoy")
else()
    set(SC_INSTALLDIR "~/.local/share/SuperCollider/Extensions/" CACHE PATH "Installation directoy")
endif()
if (NOT SC_INSTALLDIR)
    # HACK: force relative path!
    set(SC_INSTALLDIR ".")
endif()
message(STATUS "SC_INSTALLDIR: ${SC_INSTALLDIR}")

if (BUILD_WINE)
    # install wine hosts
    install(PROGRAMS "${HOST_PATH}.exe.so" RENAME "${HOST}"
        DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")

    if (BUILD_HOST32)
        install(PROGRAMS "${HOST32_PATH}.exe.so" RENAME "${HOST32}"
            DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
    endif()

    # don't actually build UGens
    return()
endif()

# plugin extension
set(CMAKE_SHARED_MODULE_PREFIX "") # no prefix!
if(APPLE OR WIN32)
set(CMAKE_SHARED_MODULE_SUFFIX ".scx")
endif()

option(SUPERNOVA "Build plugins for supernova" OFF)
if (SUPERNOVA)
    # for <nova-tt/spin_lock.h>
    include_directories(${SC_INCLUDEDIR}/external_libraries/nova-tt)
    # for <boost/atomic.hpp>
    include_directories(${SC_INCLUDEDIR}/external_libraries/boost)
endif()
message(STATUS "SUPERNOVA: ${SUPERNOVA}")

# the UGen
add_library(${PROJECT} MODULE ${SRC})
if (WIN32 AND HAVE_WINDRES)
    # resource file for sclang icon
    target_sources(${PROJECT} PRIVATE "src/${PROJECT}.rc")
endif()
target_link_libraries(${PROJECT} ${LIBS})
set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS_RELEASE -s)

if(SUPERNOVA)
    add_library(${PROJECT}_supernova MODULE ${SRC})
    if (WIN32 AND HAVE_WINDRES)
        # resource file for sclang icon
        target_sources(${PROJECT}_supernova PRIVATE "src/${PROJECT}.rc")
    endif()
    target_compile_definitions(${PROJECT}_supernova PUBLIC -DSUPERNOVA=1)
    target_link_libraries(${PROJECT}_supernova ${LIBS})
    set_target_properties(${PROJECT}_supernova PROPERTIES LINK_FLAGS_RELEASE -s)
endif()

# installation
if (HOST)
    add_dependencies(${PROJECT} ${HOST})
endif()
if (HOST32)
    add_dependencies(${PROJECT} ${HOST32})
endif()
if (HOST_AMD64)
    add_dependencies(${PROJECT} ${HOST_AMD64})
endif()

install(TARGETS ${PROJECT} DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
if (MSVC)
    install(FILES $<TARGET_PDB_FILE:${PROJECT}> OPTIONAL DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
endif()
if (SUPERNOVA)
    install(TARGETS ${PROJECT}_supernova DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
    if (MSVC)
        install(FILES $<TARGET_PDB_FILE:${PROJECT}_supernova> OPTIONAL DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
    endif()
endif()
if (HOST)
    install(PROGRAMS ${HOST_PATH} DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
    if (MSVC)
        install(FILES ${HOST_PDB_PATH} OPTIONAL DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
    endif()
endif()
if (HOST32)
    install(PROGRAMS ${HOST32_PATH} DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
endif()
if (HOST_AMD64)
    install(PROGRAMS ${HOST_AMD64_PATH} DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
endif()
if (WIN32)
    if (HOST32_PATH)
        install(PROGRAMS "${HOST32_PATH}" RENAME "host_i386.exe"
            DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
    endif()
    if (HOST_AMD64_PATH)
        install(PROGRAMS "${HOST_AMD64_PATH}" RENAME "host_amd64.exe"
            DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
    endif()
endif()
install(DIRECTORY "classes" DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
install(DIRECTORY "doc" DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
install(DIRECTORY "HelpSource" DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
install(FILES "../README.md" "../LICENSE.txt" DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
