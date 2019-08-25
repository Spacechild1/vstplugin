cmake_minimum_required (VERSION 2.8)

set(PROJECT "VSTPlugin")
message(STATUS "---\n*** ${PROJECT} ***")

set(SRC "src/${PROJECT}.h" "src/${PROJECT}.cpp")

include_directories(src)

# SuperCollider API:
set(SC_INCLUDEDIR "" CACHE PATH "SuperCollider source files")
message(STATUS "SC_INCLUDEDIR: ${SC_INCLUDEDIR}")
include_directories(${SC_INCLUDEDIR}/include/plugin_interface)
include_directories(${SC_INCLUDEDIR}/include/common)
include_directories(${SC_INCLUDEDIR}/plugin_interface)
include_directories(${SC_INCLUDEDIR}/common)

# Installation path (LATER use platform specific defaults)
set(SC_INSTALLDIR "" CACHE PATH "Installation path")
message(STATUS "SC_INSTALLDIR: ${SC_INSTALLDIR}")

SET(CMAKE_INSTALL_PREFIX "${SC_INSTALLDIR}" CACHE INTERNAL
	"Prefix prepended to install directories" FORCE)

# plugin extension
set(CMAKE_SHARED_MODULE_PREFIX "") # no prefix!
if(APPLE OR MINGW OR WIN32)
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
add_library(${PROJECT} MODULE ${SRC} ${VST_HEADERS} ${VST_SRC})
add_dependencies(${PROJECT} ${PROBE})
target_compile_definitions(${PROJECT} PUBLIC
    -DLOGFUNCTION=SCLog -DHAVE_NRT_THREAD=1 -DHAVE_UI_THREAD=1)
target_link_libraries(${PROJECT} ${VST_LIBS})

if(SUPERNOVA)
    add_library(${PROJECT}_supernova MODULE ${SRC} ${VST_SRC})
    target_compile_definitions(${PROJECT}_supernova PUBLIC
        -DSUPERNOVA=1 -DHAVE_NRT_THREAD=1 -DHAVE_UI_THREAD=1)
    target_link_libraries(${PROJECT}_supernova ${VST_LIBS})
endif()

# installation
install(TARGETS ${PROJECT} ${PROBE} DESTINATION ${PROJECT}/plugins)
if (SUPERNOVA)
    install(TARGETS ${PROJECT}_supernova DESTINATION ${PROJECT}/plugins)
endif()
install(DIRECTORY classes DESTINATION ${PROJECT})
install(DIRECTORY HelpSource DESTINATION ${PROJECT})
install(FILES "../README.md" "../LICENSE.txt" DESTINATION ${PROJECT})
