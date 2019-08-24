cmake_minimum_required (VERSION 2.8)

set(PROJECT "vstplugin~")
message(STATUS "---\n*** ${PROJECT} ***")

set(FILENAME "${PROJECT}.cpp")

include_directories(src)

# Pd paths:
if (WIN32 OR MINGW)
    set(PD_INCLUDEDIR "${PROGRAMFILES}/Pd/src" CACHE PATH "Pd header files")
elseif(APPLE)
    # isn't there a better way?
    file(GLOB _globresults "/Applications/Pd*.app/Contents/Resources/src")
    list(GET _globresults 0 _globresult)
    set(PD_INCLUDEDIR ${_globresult} CACHE PATH "Pd header files")
else()
    set(PD_INCLUDEDIR "/usr/include/pd" CACHE PATH "Pd header files")
endif()
include_directories("${PD_INCLUDEDIR}")
message(STATUS "PD_INCLUDEDIR: ${PD_INCLUDEDIR}")

if (WIN32 OR MINGW)
    set(PD_BINDIR "${PROGRAMFILES}/Pd/bin" CACHE PATH "Pd binaries")
    message(STATUS "PD_BINDIR: ${PD_BINDIR}")
endif()

if (WIN32 OR MINGW)
    set(PD_INSTALLDIR "${APPDATA}/Pd" CACHE PATH "Installation path")
elseif(APPLE)
    set(PD_INSTALLDIR "~/Library/Pd" CACHE PATH "Installation path")
else()
    set(PD_INSTALLDIR "/usr/local/lib/pd-externals" CACHE PATH "Installation path")
endif()
message(STATUS "PD_INSTALLDIR: ${PD_INSTALLDIR}")

SET(CMAKE_INSTALL_PREFIX "${PD_INSTALLDIR}" CACHE INTERNAL
    "Prefix prepended to install directories" FORCE)

# plugin extension
set(CMAKE_SHARED_MODULE_PREFIX "") # no prefix!
if (NOT PD_EXTENSION)
    if (WIN32 OR MINGW)
        set(CMAKE_SHARED_MODULE_SUFFIX ".dll")
    elseif(APPLE)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_darwin")
    elseif(LINUX)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_linux")
    endif()
else()
    set(CMAKE_SHARED_MODULE_SUFFIX ${PD_EXTENSION})
endif()
message(STATUS "PD_EXTENSION: ${CMAKE_SHARED_MODULE_SUFFIX}")

if (MINGW)
    set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

# the Pd external
add_library(${PROJECT} MODULE src/${FILENAME} ${VST_SRC})
target_link_libraries(${PROJECT} ${VST_LIBS})

# platform specific definitions and options
if (WIN32 OR MINGW)
    target_compile_definitions(${PROJECT} PUBLIC -DHAVE_UI_THREAD=1)
    target_link_libraries(${PROJECT} "${PD_BINDIR}/pd.lib")
elseif(APPLE)
    target_compile_definitions(${PROJECT} PUBLIC -DHAVE_UI_THREAD=0)
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-undefined suppress -flat_namespace -bundle")
else()
    target_compile_definitions(${PROJECT} PUBLIC -DHAVE_UI_THREAD=1)
    set_target_properties(${PROJECT} PROPERTIES COMPILE_FLAGS "-fPIC -fcheck-new")
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-rdynamic -fPIC")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()

# installation
install(TARGETS ${PROJECT} ${PROBE} DESTINATION ${PROJECT})
install(FILES "${PROJECT}-help.pd" "vstpluginbrowser.pd" "vstpluginbrowser-help.pd"
    "../README.md" "../LICENSE.txt" DESTINATION ${PROJECT})
