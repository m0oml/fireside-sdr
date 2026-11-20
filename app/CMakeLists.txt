cmake_minimum_required(VERSION 3.20)
project(fireside VERSION 0.1 LANGUAGES CXX C)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)   # the DSP tests are unusably slow at -O0
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(core)

add_executable(firesidecli tools/firesidecli.cpp)
target_link_libraries(firesidecli PRIVATE fireside_core)
target_compile_options(firesidecli PRIVATE -Wall -Wextra)

add_executable(firesidecapture tools/firesidecapture.cpp)
target_link_libraries(firesidecapture PRIVATE fireside_core)
target_compile_options(firesidecapture PRIVATE -Wall -Wextra)

add_executable(firesidekey tools/firesidekey.cpp)      # stage 3 supervised transmit tests (analysis/transmit-stage2.md)
target_link_libraries(firesidekey PRIVATE fireside_core)
target_compile_options(firesidekey PRIVATE -Wall -Wextra)
add_executable(firesidetx tools/firesidetx.cpp)
target_link_libraries(firesidetx PRIVATE fireside_core)
target_compile_options(firesidetx PRIVATE -Wall -Wextra)

add_executable(firesideplay tools/firesideplay.cpp)
target_link_libraries(firesideplay PRIVATE fireside_core)
target_compile_options(firesideplay PRIVATE -Wall -Wextra)

add_executable(firesideiq tools/firesideiq.cpp)
target_link_libraries(firesideiq PRIVATE fireside_core)
target_compile_options(firesideiq PRIVATE -Wall -Wextra)

enable_testing()
add_subdirectory(tests)

option(FIRESIDE_BUILD_GUI "Build the Qt 6 GUI" ON)
if(FIRESIDE_BUILD_GUI)
  find_package(Qt6 COMPONENTS Widgets Network)
  if(Qt6_FOUND)
    add_subdirectory(gui)
  else()
    message(STATUS "Qt6 Widgets not found: GUI skipped")
  endif()
endif()

add_executable(ft8bench tools/ft8bench.cpp)   # decoder tuning against jt9 (tools/ft8bench.py)
target_link_libraries(ft8bench PRIVATE fireside_core)
