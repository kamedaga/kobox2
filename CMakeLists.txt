# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

project(kobox2 LANGUAGES C)

add_library(kobox2 STATIC src/controller/controller.c)
add_library(kobox2::kobox2 ALIAS kobox2)

target_compile_features(kobox2 PUBLIC c_std_11)
set_target_properties(kobox2 PROPERTIES C_EXTENSIONS OFF)
target_include_directories(
    kobox2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kobox2 PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(kobox2 PRIVATE /W4)
endif()

option(KB2_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
if(KB2_ENABLE_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "KB2_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
    target_compile_options(kobox2 PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(kobox2 PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(kobox2_controller_test tests/controller_test.c)
    target_link_libraries(kobox2_controller_test PRIVATE kobox2::kobox2)

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(kobox2_controller_test PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(kobox2_controller_test PRIVATE /W4)
    endif()

    if(KB2_ENABLE_SANITIZERS)
        target_compile_options(
            kobox2_controller_test
            PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(kobox2_controller_test PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME kobox2.controller COMMAND kobox2_controller_test)
endif()

install(
    TARGETS kobox2
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES include/kobox2/controller.h DESTINATION include/kobox2)
