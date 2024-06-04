message(STATUS "---\n*** VSTPlugin ***")

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
    install(TARGETS host
        RENAME $<TARGET_FILE_BASE_NAME:host>
        DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")

    if (BUILD_HOST32)
        install(TARGETS host32
            RENAME $<TARGET_FILE_BASE_NAME:host32>
            DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
    endif()

    # don't actually build UGens
    return()
endif()

# SuperCollider API:
set(SC_INCLUDEDIR "" CACHE PATH "Path to SuperCollider respository")
message(STATUS "SC_INCLUDEDIR: ${SC_INCLUDEDIR}")

option(SUPERNOVA "Build plugins for supernova" OFF)
message(STATUS "SUPERNOVA: ${SUPERNOVA}")

# common properties
add_library(sc_common INTERFACE)

target_sources(sc_common INTERFACE "src/VSTPlugin.h" "src/VSTPlugin.cpp")

target_include_directories(sc_common INTERFACE
    ${SC_INCLUDEDIR}/include/plugin_interface
    ${SC_INCLUDEDIR}/include/common
    ${SC_INCLUDEDIR}/plugin_interface
    ${SC_INCLUDEDIR}/common)

# plugin extension
if(APPLE OR WIN32)
    set(SUFFIX ".scx")
else()
    set(SUFFIX ${CMAKE_SHARED_MODULE_SUFFIX})
endif()

if (WIN32 AND HAVE_WINDRES)
    # resource file for sclang icon
    target_sources(sc_common INTERFACE "src/VSTPlugin.rc")
endif()

target_link_libraries(sc_common INTERFACE vst)

# the UGen
add_library(sc MODULE)
target_link_libraries(sc PRIVATE sc_common)
set_target_properties(sc PROPERTIES
    PREFIX "" # no prefix!
    OUTPUT_NAME "VSTPlugin"
    SUFFIX ${SUFFIX})

# the Supernova UGen
if (SUPERNOVA)
    add_library(sc_supernova MODULE)
    target_compile_definitions(sc_supernova PRIVATE SUPERNOVA=1)
    # for <nova-tt/spin_lock.h>
    target_include_directories(sc_supernova PRIVATE
        ${SC_INCLUDEDIR}/external_libraries/nova-tt)
    # for <boost/atomic.hpp>
    target_include_directories(sc_supernova PRIVATE
        ${SC_INCLUDEDIR}/external_libraries/boost)
    target_link_libraries(sc_supernova PRIVATE sc_common)
    set_target_properties(sc_supernova PROPERTIES
        PREFIX "" # no prefix!
        OUTPUT_NAME "VSTPlugin_supernova"
        SUFFIX ${SUFFIX})
endif()

# installation

install(TARGETS sc DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
if (MSVC)
    install(FILES $<TARGET_PDB_FILE:sc> OPTIONAL DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
endif()

if (SUPERNOVA)
    install(TARGETS sc_supernova DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
    if (MSVC)
        install(FILES $<TARGET_PDB_FILE:sc_supernova> OPTIONAL DESTINATION "${SC_INSTALLDIR}/scvst/plugins")
    endif()
endif()

# installation
if (TARGET host)
    add_dependencies(sc_common host)
    install(TARGETS host DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
    if (MSVC)
        install(FILES $<TARGET_PDB_FILE:host> OPTIONAL DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
    endif()
endif()

if (TARGET host32)
    add_dependencies(sc_common host32)
    install(TARGETS host32 DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
endif()

if (TARGET host_amd64)
    add_dependencies(sc_common host_amd64)
    install(TARGETS host_amd64 DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
endif()

if (WIN32)
    if (HOST32_PATH)
        install(PROGRAMS "${HOST32_PATH}" RENAME "host_i386.exe"
            DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
    endif()

    if (HOST_AMD64_PATH)
        install(PROGRAMS "${HOST_AMD64_PATH}" RENAME "host_amd64.exe"
            DESTINATION "${SC_INSTALLDIR}/VSTPlugin/plugins")
    endif()
endif()

install(DIRECTORY "classes" DESTINATION "${SC_INSTALLDIR}/VSTPlugin")
install(DIRECTORY "doc" DESTINATION "${SC_INSTALLDIR}/VSTPlugin")
install(DIRECTORY "HelpSource" DESTINATION "${SC_INSTALLDIR}/VSTPlugin")
install(FILES "../README.md" "../LICENSE.txt" DESTINATION "${SC_INSTALLDIR}/VSTPlugin")
