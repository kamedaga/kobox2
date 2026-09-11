# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

project(kobox2 LANGUAGES C)

include(cmake/platform.cmake)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(
    kobox2_protocol_check ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_protocol.py --check
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking generated protocol layout"
)

add_library(kobox2 STATIC src/closure.c src/controller/controller.c)
add_library(kobox2::kobox2 ALIAS kobox2)
add_library(
    kobox2_protocol STATIC
    protocol/src/closure_manifest.c
    protocol/src/gpu.c
    protocol/src/protocol.c
    protocol/src/resource_grant.c
    protocol/src/sha256.c
)
add_library(kobox2::protocol ALIAS kobox2_protocol)
add_dependencies(kobox2_protocol kobox2_protocol_check)

target_compile_features(kobox2 PUBLIC c_std_11)
target_compile_features(kobox2_protocol PUBLIC c_std_11)
set_target_properties(kobox2 kobox2_protocol PROPERTIES C_EXTENSIONS OFF)
target_include_directories(
    kobox2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_include_directories(
    kobox2_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/protocol/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/protocol/generated/include>
        $<INSTALL_INTERFACE:include>
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kobox2 PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kobox2_protocol PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(kobox2 PRIVATE /W4)
    target_compile_options(kobox2_protocol PRIVATE /W4)
endif()

option(KB2_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
if(KB2_ENABLE_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "KB2_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
    target_compile_options(kobox2 PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(kobox2 PRIVATE -fsanitize=address,undefined)
    target_compile_options(
        kobox2_protocol
        PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(kobox2_protocol PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)
if(KB2_BUILD_SANDBOX AND NOT BUILD_TESTING)
    add_subdirectory(linux-sandbox/kobox)
endif()
if(BUILD_TESTING)
    add_library(kobox2_test_closure STATIC tests/test_closure.c)
    target_link_libraries(kobox2_test_closure PUBLIC kobox2::kobox2 kobox2::protocol)
    target_include_directories(kobox2_test_closure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_executable(kobox2_controller_test tests/controller_test.c)
    target_link_libraries(kobox2_controller_test PRIVATE kobox2_test_closure)
    add_executable(kobox2_closure_test tests/closure_test.c)
    target_link_libraries(kobox2_closure_test PRIVATE kobox2::kobox2 kobox2::protocol)
    add_executable(kobox2_protocol_test tests/protocol_test.c)
    target_link_libraries(kobox2_protocol_test PRIVATE kobox2::protocol)
    add_executable(kobox2_resource_grant_test tests/resource_grant_test.c)
    target_link_libraries(kobox2_resource_grant_test PRIVATE kobox2::protocol)
    add_executable(kobox2_gpu_protocol_test tests/gpu_protocol_test.c)
    target_link_libraries(kobox2_gpu_protocol_test PRIVATE kobox2::protocol)
    if(KB2_BUILD_SANDBOX AND KB2_OS_BACKEND STREQUAL "linux")
        add_library(
            kobox2_test_transport STATIC
            protocol/test/src/bootstrap.c
            protocol/test/src/management.c
            protocol/test/src/split_virtqueue.c
        )
        target_include_directories(
            kobox2_test_transport
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/protocol/test/include
        )
        target_link_libraries(kobox2_test_transport PUBLIC kobox2::protocol)
        add_executable(kobox2_native_host_integration_test tests/native_host_integration_test.c)
        target_link_libraries(kobox2_native_host_integration_test
            PRIVATE kobox2::kobox2 kobox2_test_transport)
        target_compile_options(kobox2_native_host_integration_test
            PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_executable(kobox2_bootstrap_test tests/bootstrap_test.c)
        target_link_libraries(kobox2_bootstrap_test PRIVATE kobox2_test_transport)
        add_test(NAME kobox2.bootstrap_fd_ownership COMMAND kobox2_bootstrap_test)
        add_executable(kobox2_virtqueue_test tests/virtqueue_test.c)
        target_link_libraries(kobox2_virtqueue_test PRIVATE kobox2_test_transport)
        add_executable(
            kobox2_core_runtime_inline_test tests/core_runtime_inline_test.c
        )
        target_link_libraries(
            kobox2_core_runtime_inline_test PRIVATE kobox2::protocol
        )
        add_executable(
            kobox2_test_sandbox
            tests/sandbox_child.c
        )
        target_link_libraries(kobox2_test_sandbox PRIVATE kobox2_test_transport)
        add_executable(
            kobox2_host_integration_test
            tests/host_integration_test.c
            tests/host/linux_host_adapter.c
            tests/host/linux_host_transport.c
        )
        target_link_libraries(
            kobox2_host_integration_test
            PRIVATE kobox2_test_closure kobox2_test_transport
        )
        add_subdirectory(linux-sandbox/kobox)
        add_executable(
            kobox2_fixture_integration_test
            tests/fixture_integration_test.c
            tests/host/linux_host_adapter.c
            tests/host/linux_host_transport.c
        )
        target_link_libraries(
            kobox2_fixture_integration_test
            PRIVATE kobox2_test_closure kobox2_test_transport
        )
        add_dependencies(
            kobox2_host_integration_test kobox_fixture_core
            kobox_fixture_provider kobox_fixture_consumer
        )
        add_dependencies(
            kobox2_fixture_integration_test kobox_fixture_core
            kobox_fixture_provider kobox_fixture_consumer
        )
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(kobox2_controller_test PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kobox2_closure_test PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kobox2_test_closure PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kobox2_protocol_test PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kobox2_resource_grant_test PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(kobox2_gpu_protocol_test PRIVATE -Wall -Wextra -Wpedantic)
        if(KB2_BUILD_SANDBOX AND KB2_OS_BACKEND STREQUAL "linux")
            target_compile_options(kobox2_test_transport PRIVATE -Wall -Wextra -Wpedantic)
            target_compile_options(kobox2_virtqueue_test PRIVATE -Wall -Wextra -Wpedantic)
            target_compile_options(
                kobox2_core_runtime_inline_test PRIVATE -Wall -Wextra -Wpedantic
            )
            target_compile_options(kobox2_test_sandbox PRIVATE -Wall -Wextra -Wpedantic)
            target_compile_options(kobox2_host_integration_test PRIVATE -Wall -Wextra -Wpedantic)
            target_compile_options(
                kobox2_fixture_integration_test PRIVATE -Wall -Wextra -Wpedantic
            )
        endif()
    elseif(MSVC)
        target_compile_options(kobox2_controller_test PRIVATE /W4)
        target_compile_options(kobox2_closure_test PRIVATE /W4)
        target_compile_options(kobox2_test_closure PRIVATE /W4)
        target_compile_options(kobox2_protocol_test PRIVATE /W4)
        target_compile_options(kobox2_resource_grant_test PRIVATE /W4)
        target_compile_options(kobox2_gpu_protocol_test PRIVATE /W4)
    endif()

    if(KB2_ENABLE_SANITIZERS)
        target_compile_options(
            kobox2_controller_test
            PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(kobox2_controller_test PRIVATE -fsanitize=address,undefined)
        target_compile_options(
            kobox2_closure_test
            PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(kobox2_closure_test PRIVATE -fsanitize=address,undefined)
        target_compile_options(
            kobox2_test_closure
            PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_compile_options(
            kobox2_protocol_test
            PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(kobox2_protocol_test PRIVATE -fsanitize=address,undefined)
        target_compile_options(
            kobox2_resource_grant_test
            PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(
            kobox2_resource_grant_test PRIVATE -fsanitize=address,undefined
        )
        target_compile_options(
            kobox2_gpu_protocol_test
            PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(kobox2_gpu_protocol_test PRIVATE -fsanitize=address,undefined)
        if(KB2_BUILD_SANDBOX AND KB2_OS_BACKEND STREQUAL "linux")
            target_compile_options(kobox2_native_host_integration_test
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
            target_link_options(kobox2_native_host_integration_test PRIVATE -fsanitize=address,undefined)
            target_compile_options(kobox2_bootstrap_test
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
            target_link_options(kobox2_bootstrap_test PRIVATE -fsanitize=address,undefined)
            target_compile_options(
                kobox2_test_transport
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
            )
            target_compile_options(
                kobox2_virtqueue_test
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
            )
            target_link_options(kobox2_virtqueue_test PRIVATE -fsanitize=address,undefined)
            target_compile_options(
                kobox2_core_runtime_inline_test
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
            )
            target_link_options(
                kobox2_core_runtime_inline_test
                PRIVATE -fsanitize=address,undefined
            )
            target_compile_options(
                kobox2_test_sandbox
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
            )
            target_link_options(kobox2_test_sandbox PRIVATE -fsanitize=address,undefined)
            target_compile_options(
                kobox2_host_integration_test
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
            )
            target_link_options(kobox2_host_integration_test PRIVATE -fsanitize=address,undefined)
            target_compile_options(
                kobox2_fixture_integration_test
                PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined
            )
            target_link_options(
                kobox2_fixture_integration_test PRIVATE -fsanitize=address,undefined
            )
        endif()
    endif()

    add_test(NAME kobox2.controller COMMAND kobox2_controller_test)
    add_test(NAME kobox2.closure COMMAND kobox2_closure_test)
    add_test(NAME kobox2.protocol COMMAND kobox2_protocol_test)
    add_test(NAME kobox2.resource_grant COMMAND kobox2_resource_grant_test)
    add_test(NAME kobox2.gpu_protocol COMMAND kobox2_gpu_protocol_test)
    if(KB2_BUILD_SANDBOX AND KB2_OS_BACKEND STREQUAL "linux")
        add_test(NAME kobox2.virtqueue COMMAND kobox2_virtqueue_test)
        add_test(
            NAME kobox2.core_runtime_inline
            COMMAND kobox2_core_runtime_inline_test
        )
        add_test(
            NAME kobox2.host_process
            COMMAND
                kobox2_host_integration_test
                $<TARGET_FILE:kobox2_test_sandbox>
                $<TARGET_FILE:kobox_fixture_core>
                "${KOBOX_FIXTURE_PROVIDER_MODULE}"
                "${KOBOX_FIXTURE_CONSUMER_MODULE}"
        )
        set_tests_properties(kobox2.host_process PROPERTIES TIMEOUT 15)
        add_test(
            NAME kobox2.fixture_process
            COMMAND
                kobox2_fixture_integration_test
                $<TARGET_FILE:kobox_fixture_sandbox>
                $<TARGET_FILE:kobox_fixture_core>
                "${KOBOX_FIXTURE_PROVIDER_MODULE}"
                "${KOBOX_FIXTURE_CONSUMER_MODULE}"
        )
        set_tests_properties(kobox2.fixture_process PROPERTIES TIMEOUT 15)
    endif()
endif()

install(
    TARGETS kobox2 kobox2_protocol
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES include/kobox2/controller.h DESTINATION include/kobox2)
install(FILES include/kobox2/closure.h DESTINATION include/kobox2)
install(FILES protocol/include/kobox2/protocol.h DESTINATION include/kobox2)
install(FILES protocol/include/kobox2/closure_manifest.h DESTINATION include/kobox2)
install(FILES protocol/include/kobox2/resource_grant.h DESTINATION include/kobox2)
install(FILES protocol/include/kobox2/gpu.h DESTINATION include/kobox2)
install(FILES protocol/include/kobox2/sha256.h DESTINATION include/kobox2)
install(
    FILES
        protocol/generated/include/kobox2/closure_layout.h
        protocol/generated/include/kobox2/gpu_layout.h
        protocol/generated/include/kobox2/gpu_drm_core_layout.h
        protocol/generated/include/kobox2/gpu_drm_mode_layout.h
        protocol/generated/include/kobox2/gpu_drm_virtgpu_layout.h
        protocol/generated/include/kobox2/gpu_drm_amdgpu_layout.h
        protocol/generated/include/kobox2/core_runtime.h
        protocol/generated/include/kobox2/core_runtime_layout.h
        protocol/generated/include/kobox2/memory_arena_layout.h
        protocol/generated/include/kobox2/iommu_domain_layout.h
        protocol/generated/include/kobox2/protocol_layout.h
        protocol/generated/include/kobox2/resource_grant_layout.h
    DESTINATION include/kobox2
)
