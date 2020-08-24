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

# Installation path
# install directory:
if (WIN32 OR MINGW)
    set(SC_INSTALLDIR "${LOCALAPPDATA}/SuperCollider/Extensions/" CACHE PATH "Installation directoy")
elseif(APPLE)
    set(SC_INSTALLDIR "~/Library/Application Support/SuperCollider/Extensions/" CACHE PATH "Installation directoy")
else()
    set(SC_INSTALLDIR "~/.local/share/SuperCollider/Extensions/" CACHE PATH "Installation directoy")
endif()
message(STATUS "SC_INSTALLDIR: ${SC_INSTALLDIR}")

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
add_library(${PROJECT} MODULE ${SRC})
target_compile_definitions(${PROJECT} PUBLIC -DLOGFUNCTION=SCLog)
target_link_libraries(${PROJECT} ${LIBS})
set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS_RELEASE -s)

if(SUPERNOVA)
    add_library(${PROJECT}_supernova MODULE ${SRC})
    target_compile_definitions(${PROJECT}_supernova PUBLIC -DSUPERNOVA=1)
    target_link_libraries(${PROJECT}_supernova ${LIBS})
    set_target_properties(${PROJECT}_supernova PROPERTIES LINK_FLAGS_RELEASE -s)
endif()

# installation
add_dependencies(${PROJECT} ${HOST})
if (HOST32)
    add_dependencies(${PROJECT} ${HOST32})
endif()

install(TARGETS ${PROJECT} DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
if (SUPERNOVA)
    install(TARGETS ${PROJECT}_supernova DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
endif()
install(PROGRAMS "${HOST_PATH}" DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
if (HOST32)
    install(PROGRAMS "${HOST32_PATH}" DESTINATION "${SC_INSTALLDIR}/${PROJECT}/plugins")
endif()
install(DIRECTORY classes DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
install(DIRECTORY Help DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
install(DIRECTORY HelpSource DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
install(FILES "../README.md" "../LICENSE.txt" DESTINATION "${SC_INSTALLDIR}/${PROJECT}")
