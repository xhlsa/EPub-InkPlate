
cmake_minimum_required(VERSION 3.16.0)

set(CMAKE_CXX_STANDARD 17)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# Always exclude embed/ — binary blobs registered separately below.
list(FILTER app_sources EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/embed/.*")

# BUILD_VARIANT selects which entry point is compiled.
#   full (default) — full multi-book firmware with all controllers
#   single_book    — minimal single-book reader (src/single_book/main.cpp)
if(NOT DEFINED BUILD_VARIANT)
  set(BUILD_VARIANT "full")
endif()

if(BUILD_VARIANT STREQUAL "single_book")
  message(STATUS "BUILD_VARIANT=single_book: using src/single_book/main.cpp")
  # Drop the full-app entry point; single_book/main.cpp provides app_main().
  list(FILTER app_sources EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/main\\.cpp")
  add_compile_definitions(SINGLE_BOOK_BUILD=1)
else()
  message(STATUS "BUILD_VARIANT=full: standard firmware")
  # Drop single-book sources from the full build.
  list(FILTER app_sources EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/src/single_book/.*")
endif()

set(components
    global
    miniz
    pugixml
    sys_functions
    freertos
    ESP-IDF-InkPlate
    inkplate_screen
    memory_pool
    pictures
    esp_http_server
    esp_app_format
    pthread
    freetype
    spi_flash
    esp_littlefs
)

idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "." REQUIRES ${components})

target_add_binary_data(${COMPONENT_TARGET} "embed/favicon.ico" BINARY)
target_add_binary_data(${COMPONENT_TARGET} "embed/upload_script.html" TEXT)

