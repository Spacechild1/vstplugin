set(PROJECT "vstplugin_tilde")
set(FILENAME "vstplugin~")
message(STATUS "---\n*** ${FILENAME} ***")

set(SRC "src/${FILENAME}.h" "src/${FILENAME}.cpp")

include_directories(src)

# Pd directories:
if (NOT PD_DIR)
    if (WIN32)
        set(PD_INCLUDEDIR "${PROGRAMFILES}/Pd/src" CACHE PATH "Pd header files")
    elseif(APPLE)
        # isn't there a better way?
        file(GLOB _globresults "/Applications/Pd*.app/Contents/Resources/src")
        list(GET _globresults 0 _globresult)
        set(PD_INCLUDEDIR ${_globresult} CACHE PATH "Pd header files")
    else()
        set(PD_INCLUDEDIR "/usr/include/pd" CACHE PATH "Pd header files")
    endif()
    if (WIN32)
        set(PD_BINDIR "${PROGRAMFILES}/Pd/bin" CACHE PATH "Pd binaries")
    endif()
else()
    set(PD_INCLUDEDIR "${PD_DIR}/src" CACHE PATH "Pd header files" FORCE)
    if (WIN32)
        set(PD_BINDIR "${PD_DIR}/bin" CACHE PATH "Pd binaries" FORCE)
    endif()
endif()
include_directories(${PD_INCLUDEDIR})
message(STATUS "PD_INCLUDEDIR: ${PD_INCLUDEDIR}")
if (WIN32)
    message(STATUS "PD_BINDIR: ${PD_BINDIR}")
endif()

# install directory:
if (WIN32)
    set(PD_INSTALLDIR "${APPDATA}/Pd" CACHE PATH "Installation directoy")
elseif(APPLE)
    set(PD_INSTALLDIR "~/Library/Pd" CACHE PATH "Installation directory")
else()
    set(PD_INSTALLDIR "/usr/local/lib/pd-externals" CACHE PATH "Installation directory")
endif()
if (NOT PD_INSTALLDIR)
    # HACK: force relative path!
    set(PD_INSTALLDIR ".")
endif()
message(STATUS "PD_INSTALLDIR: ${PD_INSTALLDIR}")

if (BUILD_WINE)
    # install wine hosts
    install(PROGRAMS "${HOST_PATH}.exe.so" RENAME "${HOST}"
        DESTINATION "${PD_INSTALLDIR}/${FILENAME}")

    if (BUILD_HOST32)
        install(PROGRAMS "${HOST32_PATH}.exe.so" RENAME "${HOST32}"
            DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
    endif()

    # don't actually build Pd external
    return()
endif()

# multi-instance support
option(PD_MULTI_INSTANCE "Pd multi-instance support (for libpd)" OFF)
message(STATUS "Pd multi-instance support: ${PD_MULTI_INSTANCE}")

# plugin extension
set(CMAKE_SHARED_MODULE_PREFIX "") # no prefix!
set(PD_EXTENSION "" CACHE STRING "Pd plugin extension")
if (NOT PD_EXTENSION)
    if (WIN32)
        set(CMAKE_SHARED_MODULE_SUFFIX ".dll")
    elseif(APPLE)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_darwin")
    elseif(LINUX)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_linux")
    endif()
else()
    set(CMAKE_SHARED_MODULE_SUFFIX ".${PD_EXTENSION}")
endif()
message(STATUS "Pd extension: ${CMAKE_SHARED_MODULE_SUFFIX}")

if (MINGW)
    set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

# the Pd external
add_library(${PROJECT} MODULE ${SRC})
set_target_properties(${PROJECT} PROPERTIES OUTPUT_NAME ${FILENAME})
target_link_libraries(${PROJECT} ${LIBS})
set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS_RELEASE -s)
if (PD_MULTI_INSTANCE)
    target_compile_definitions(${PROJECT} PUBLIC -DPDINSTANCE)
endif()

# platform specific definitions and options
if (WIN32)
    if (MINGW)
        set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "--enable-auto-import")
        target_link_libraries(${PROJECT} "${PD_BINDIR}/pd.dll")
    else()
        target_link_libraries(${PROJECT} "${PD_BINDIR}/pd.lib")
    endif()
elseif(APPLE)
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-undefined suppress -flat_namespace -bundle")
else()
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-rdynamic -fPIC")
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

install(TARGETS ${PROJECT} DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
if (MSVC)
    install(FILES $<TARGET_PDB_FILE:${PROJECT}> OPTIONAL DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
endif()
if (HOST)
    install(PROGRAMS ${HOST_PATH} DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
    if (MSVC)
        install(FILES ${HOST_PDB_PATH} OPTIONAL DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
    endif()
endif()
if (HOST32)
    install(PROGRAMS ${HOST32_PATH} DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
endif()
if (HOST_AMD64)
    install(PROGRAMS ${HOST_AMD64_PATH} DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
endif()
file(GLOB PD_PATCHES "*.pd")
install(FILES ${PD_PATCHES} "../README.md" "../LICENSE.txt" DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
