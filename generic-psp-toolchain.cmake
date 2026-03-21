# Generic toolchain file for cross-compiling for PSP
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_SYSTEM_PROCESSOR mipsel)

# Detect PSPDEV from environment
if(NOT DEFINED ENV{PSPDEV})
    message(FATAL_ERROR "PSPDEV environment variable not set")
endif()
file(TO_CMAKE_PATH "$ENV{PSPDEV}" PSPDEV_PATH)

set(CMAKE_C_COMPILER   "${PSPDEV_PATH}/bin/psp-gcc")
set(CMAKE_CXX_COMPILER "${PSPDEV_PATH}/bin/psp-g++")
set(CMAKE_AR           "${PSPDEV_PATH}/bin/psp-ar")
set(CMAKE_RANLIB       "${PSPDEV_PATH}/bin/psp-ranlib")

set(CMAKE_FIND_ROOT_PATH "${PSPDEV_PATH}/psp")

add_definitions(-D_PSP)
include_directories(
    ${CMAKE_FIND_ROOT_PATH}/sdk/include
    ${CMAKE_FIND_ROOT_PATH}/include
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_INSTALL_PREFIX "${CMAKE_FIND_ROOT_PATH}" CACHE PATH "Install prefix" FORCE)
set(CMAKE_C_FLAGS "-G0" CACHE STRING "PSP C flags" FORCE)
set(CMAKE_CXX_FLAGS "-G0" CACHE STRING "PSP C++ flags" FORCE)
