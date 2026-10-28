# HarmonyOS native module for the embedded SIRIL engine.
#
# libsiril-ohos.so is produced by the SIRIL meson build
# (meson setup -Dgtk=false -Dohos_bridge=true).  All of its runtime
# dependencies are copied next to the HAP libraries by
# scripts/build-ohos-app.sh.
cmake_minimum_required(VERSION 3.5.0)
project(sirilohos)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefix of the cross-compiled SIRIL installation.  The build script exports
# SIRIL_OHOS_PREFIX; without it we fall back to the sibling build directory so
# a developer can build the HAP straight after the native build.
if(DEFINED ENV{SIRIL_OHOS_PREFIX})
    set(SIRIL_OHOS_PREFIX "$ENV{SIRIL_OHOS_PREFIX}")
else()
    set(SIRIL_OHOS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../siril-ohos-build/sysroot")
endif()

message(STATUS "SIRIL OHOS prefix: ${SIRIL_OHOS_PREFIX}")

add_library(sirilohos SHARED napi_init.cpp)

target_include_directories(sirilohos PRIVATE
    ${SIRIL_OHOS_PREFIX}/include
    ${SIRIL_OHOS_PREFIX}/include/glib-2.0
    ${SIRIL_OHOS_PREFIX}/lib/glib-2.0/include)

target_link_directories(sirilohos PRIVATE ${SIRIL_OHOS_PREFIX}/lib)

target_link_libraries(sirilohos PUBLIC
    libace_napi.z.so
    siril-ohos
    glib-2.0)

set_target_properties(sirilohos PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
