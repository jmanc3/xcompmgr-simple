cmake_minimum_required(VERSION 3.14)

set(project_name xcompmgr-simple)
project(${project_name})

set(CMAKE_CXX_STANDARD 11)

file(GLOB HEADERS *.h)
file(GLOB SOURCES *.cpp)

add_executable(${project_name} ${HEADERS} ${SOURCES})

find_package(PkgConfig)

if (NOT PkgConfig_FOUND)
    message(FATAL_ERROR "You need to have pkg-config installed. On voidlinux, sudo xbps-install pkg-config")
endif ()

function(try_to_add_dependency lib_name nice_name possible_fix)
    if (${lib_name}_FOUND)
        target_link_libraries(${project_name} PUBLIC ${${lib_name}_LIBRARIES})
        target_include_directories(${project_name} PUBLIC ${${lib_name}_INCLUDE_DIRS})
        target_compile_options(${project_name} PUBLIC ${${lib_name}_CFLAGS_OTHER})
    else ()
        message(FATAL_ERROR "Could not find: ${nice_name}.\
                             Make sure you're system has it installed.\
                             To install it on voidlinux, sudo xbps-install ${possible_fix}")
    endif ()
endfunction(try_to_add_dependency)

# The first variable is the name we want to give it, and the second is the thing that pkg-config actually searches for
pkg_check_modules(D_X11 x11)
pkg_check_modules(D_XCOMPOSITE xcomposite)
pkg_check_modules(D_X11RENDER xrender)
pkg_check_modules(D_XSHAPE xext)
pkg_check_modules(D_XDAMAGE xdamage)

try_to_add_dependency(D_X11 x11 "xorg-devel")
try_to_add_dependency(D_XCOMPOSITE Xcomposite "xorg-devel")
try_to_add_dependency(D_X11RENDER Xrender "xorg-devel")
try_to_add_dependency(D_XSHAPE Xshape "xorg-devel")
try_to_add_dependency(D_XDAMAGE Xdamage "xorg-devel")
