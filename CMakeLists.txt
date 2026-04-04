cmake_minimum_required(VERSION 3.19)
project(kicli VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Platform ────────────────────────────────────────────────────────────────
if(WIN32)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN)
    set(PLATFORM_LIBS ws2_32)
else()
    set(PLATFORM_LIBS m)
endif()

# ── System libcurl (needed for fetch/stock when implemented) ─────────────────
find_package(CURL REQUIRED)

# ── Sources ─────────────────────────────────────────────────────────────────
file(GLOB_RECURSE KICLI_SOURCES
    src/*.c
    src/kicad/*.c
    src/project/*.c
    src/fetch/*.c
    src/stock/*.c
    src/sch/*.c
    src/sch/parser/*.c
    src/sch/ops/*.c
    src/jlcpcb/*.c
)

add_executable(kicli ${KICLI_SOURCES})

target_include_directories(kicli PRIVATE
    include
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(kicli PRIVATE
    CURL::libcurl
    ${PLATFORM_LIBS}
)

if(MSVC)
    target_compile_options(kicli PRIVATE /W3)
else()
    target_compile_options(kicli PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Install ─────────────────────────────────────────────────────────────────
install(TARGETS kicli RUNTIME DESTINATION bin)

# ── Package ─────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "kicli")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "KiCad CLI toolkit")
set(CPACK_GENERATOR "TGZ;ZIP")
include(CPack)
