cmake_minimum_required(VERSION 3.19)
project(kicli VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Output directory ────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Platform-specific settings ──────────────────────────────────────────────
if(WIN32)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN)
    set(PLATFORM_LIBS ws2_32)   # Winsock for curl on Windows
else()
    set(PLATFORM_LIBS pthread m)
endif()

# ── Find system libcurl ──────────────────────────────────────────────────────
find_package(CURL REQUIRED)

# ── Vendor library: cJSON ───────────────────────────────────────────────────
add_library(cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(cjson PUBLIC vendor/cjson)

# ── Vendor library: zip (wraps miniz) ───────────────────────────────────────
add_library(ziplib STATIC vendor/miniz/zip.c)
target_include_directories(ziplib PUBLIC vendor/miniz)
target_compile_definitions(ziplib PUBLIC ZIP_STATIC)

# ── Vendor library: tomlc99 ─────────────────────────────────────────────────
add_library(tomlc99 STATIC vendor/tomlc99/toml.c)
target_include_directories(tomlc99 PUBLIC vendor/tomlc99)

# ── Vendor library: tinycthread ─────────────────────────────────────────────
add_library(tinycthread STATIC vendor/tinycthread/tinycthread.c)
target_include_directories(tinycthread PUBLIC vendor/tinycthread)
if(WIN32)
    target_link_libraries(tinycthread PUBLIC winmm synchronization)
else()
    target_link_libraries(tinycthread PUBLIC pthread)
endif()

# ── kicli sources ───────────────────────────────────────────────────────────
file(GLOB_RECURSE KICLI_SOURCES
    src/*.c
    src/fetch/*.c
    src/fetch/sources/*.c
    src/stock/*.c
    src/stock/suppliers/*.c
    src/sch/*.c
)

add_executable(kicli ${KICLI_SOURCES})

target_include_directories(kicli PRIVATE
    include
    vendor/optparse
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(kicli PRIVATE
    cjson
    ziplib
    tomlc99
    tinycthread
    CURL::libcurl
    ${PLATFORM_LIBS}
)

# Warnings
if(MSVC)
    target_compile_options(kicli PRIVATE /W3)
else()
    target_compile_options(kicli PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────
enable_testing()

foreach(test_src IN ITEMS tests/test_sch.c tests/test_fetch.c tests/test_stock.c)
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_include_directories(${test_name} PRIVATE include vendor/optparse ${CURL_INCLUDE_DIRS})
    target_link_libraries(${test_name} PRIVATE cjson ziplib tomlc99 tinycthread CURL::libcurl ${PLATFORM_LIBS})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ── Install ─────────────────────────────────────────────────────────────────
install(TARGETS kicli RUNTIME DESTINATION bin)

# ── CPack (packaging) ───────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "kicli")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "KiCad CLI toolkit")
set(CPACK_GENERATOR "TGZ;ZIP")
include(CPack)
