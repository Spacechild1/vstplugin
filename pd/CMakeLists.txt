message(STATUS "---\n*** vstplugin~ ***")

# Pd directories:
if (NOT PD_DIR)
    if (WIN32)
        set(_pddir "${PROGRAMFILES}/Pd")
        if (NOT PD_INCLUDEDIR AND NOT (EXISTS "${_pddir}"))
            message(FATAL_ERROR "Could not find Pd installation - please set PD_INCLUDEDIR or PD_DIR")
        endif()
        set(PD_INCLUDEDIR "${_pddir}/src" CACHE PATH "Pd header files")
        set(PD_BINDIR "${_pddir}/bin" CACHE PATH "Pd executables")
    elseif(APPLE)
        file(GLOB _globresult "/Applications/Pd*.app")
        if (_globresult)
            list(GET _globresult 0 _pddir)
        endif()
        if (NOT PD_INCLUDEDIR AND NOT (EXISTS "${_pddir}"))
            message(FATAL_ERROR "Could not find Pd installation - please set PD_INCLUDEDIR")
        endif()
        set(PD_INCLUDEDIR "${_pddir}/Contents/Resources/src" CACHE PATH "Pd header files")
    else()
        set(PD_INCLUDEDIR "/usr/include/pd" CACHE PATH "Pd header files")
    endif()
else()
    set(PD_INCLUDEDIR "${PD_DIR}/src" CACHE PATH "Pd header files" FORCE)
    if (WIN32)
        set(PD_BINDIR "${PD_DIR}/bin" CACHE PATH "Pd binaries" FORCE)
    endif()
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

# wine host installation
if (BUILD_WINE)
    # install wine hosts
    # NB: we cannot install it as a target because CMake would use the wrong
    # output name, so we need to use PROGRAMS with the full absolute file path.
    # NB: generator expressions are only supported for RENAME with CMake 3.20+,
    # so we use the OUTPUT_NAME property instead.
    get_target_property(hostname host OUTPUT_NAME)
    install(PROGRAMS "$<TARGET_FILE_DIR:host>/${hostname}.exe.so"
        RENAME "${hostname}"
        DESTINATION "${PD_INSTALLDIR}/vstplugin~")

    if (BUILD_HOST32)
        get_target_property(host32name host32 OUTPUT_NAME)
        install(PROGRAMS "$<TARGET_FILE_DIR:host32>/${host32name}.exe.so"
            RENAME ${host32name}
            DESTINATION "${PD_INSTALLDIR}/vstplugin~")
    endif()

    # don't actually build Pd external
    return()
endif()

message(STATUS "PD_INCLUDEDIR: ${PD_INCLUDEDIR}")
if (WIN32)
    message(STATUS "PD_BINDIR: ${PD_BINDIR}")
endif()

# the Pd external
add_library(pd MODULE "src/vstplugin~.h" "src/vstplugin~.cpp")

if (WIN32 AND HAVE_WINDRES)
    # resource file for sclang icon
    target_sources(pd PRIVATE "src/vstplugin~.rc")
endif()

target_include_directories(pd PRIVATE ${PD_INCLUDEDIR})

target_link_libraries(pd PRIVATE vst)

# multi-instance support
option(PD_MULTI_INSTANCE "Pd multi-instance support (for libpd)" OFF)
message(STATUS "Pd multi-instance support: ${PD_MULTI_INSTANCE}")
if (PD_MULTI_INSTANCE)
    target_compile_definitions(pd PUBLIC PDINSTANCE)
endif()

# plugin extension
set(PD_EXTENSION "" CACHE STRING "Pd plugin extension")
if (NOT PD_EXTENSION)
    # default extensions
    if (WIN32)
        set(SUFFIX ".dll")
    elseif(APPLE)
        set(SUFFIX ".pd_darwin")
    elseif(LINUX)
        set(SUFFIX ".pd_linux")
    else()
        set(SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}")
    endif()
else()
    set(SUFFIX ".${PD_EXTENSION}")
endif()
message(STATUS "Pd extension: ${SUFFIX}")

# output name
set_target_properties(pd PROPERTIES
    PREFIX "" # no prefix!
    OUTPUT_NAME "vstplugin~"
    SUFFIX ${SUFFIX})

# platform specific definitions and options
if (WIN32)
    if (MINGW)
        target_link_options(pd PRIVATE --enable-auto-import)
        target_link_libraries(pd PRIVATE "${PD_BINDIR}/pd.dll")
    else()
        target_link_libraries(pd PRIVATE "${PD_BINDIR}/pd.lib")
    endif()
elseif(APPLE)
    target_link_options(pd PRIVATE -undefined suppress -flat_namespace -bundle)
else()
    target_link_options(pd PRIVATE -rdynamic -fPIC)
endif()

# installation

install(TARGETS pd DESTINATION "${PD_INSTALLDIR}/vstplugin~")
if (MSVC)
    install(FILES $<TARGET_PDB_FILE:pd> OPTIONAL DESTINATION "${PD_INSTALLDIR}/vstplugin~")
endif()

if (TARGET host)
    add_dependencies(pd host)
    install(TARGETS host DESTINATION "${PD_INSTALLDIR}/vstplugin~")
    if (MSVC)
        install(FILES $<TARGET_PDB_FILE:host> OPTIONAL DESTINATION "${PD_INSTALLDIR}/vstplugin~")
    endif()
endif()

if (TARGET host32)
    add_dependencies(pd host32)
    install(TARGETS host32 DESTINATION "${PD_INSTALLDIR}/vstplugin~")
endif()

if (TARGET host_amd64)
    add_dependencies(pd host_amd64)
    install(TARGETS host_amd64 DESTINATION "${PD_INSTALLDIR}/vstplugin~")
endif()

if (WIN32)
    if (HOST32_PATH)
        install(PROGRAMS "${HOST32_PATH}" RENAME "host_i386.exe"
            DESTINATION "${PD_INSTALLDIR}/vstplugin~")
    endif()
    if (HOST_AMD64_PATH)
        install(PROGRAMS "${HOST_AMD64_PATH}" RENAME "host_amd64.exe"
            DESTINATION "${PD_INSTALLDIR}/vstplugin~")
    endif()
endif()

file(GLOB PD_PATCHES "*.pd")
install(FILES ${PD_PATCHES} "../README.md" "../LICENSE.txt"
    DESTINATION "${PD_INSTALLDIR}/vstplugin~")
