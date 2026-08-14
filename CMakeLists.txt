cmake_minimum_required(VERSION 3.20)

project(dobby LANGUAGES CXX)

set(DOBBY_PROTOCOL_REFERENCE_DIR ${PROJECT_SOURCE_DIR}/data/bedrock/1.26.40)
set(DOBBY_PROTOCOL_REFERENCE_FILES
    ${DOBBY_PROTOCOL_REFERENCE_DIR}/protocol.json
    ${DOBBY_PROTOCOL_REFERENCE_DIR}/version.json)
set(DOBBY_PROTOCOL_REFERENCE_COMMIT
    8a80816cbfb3fe2b609f2cde4e57796c8033af61)
set(DOBBY_PROTOCOL_REFERENCE_HEADER
    ${CMAKE_CURRENT_BINARY_DIR}/generated/network/protocol_reference.hpp)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${DOBBY_PROTOCOL_REFERENCE_HEADER}
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/generate_protocol_reference.py
        --protocol ${DOBBY_PROTOCOL_REFERENCE_DIR}/protocol.json
        --version ${DOBBY_PROTOCOL_REFERENCE_DIR}/version.json
        --output ${DOBBY_PROTOCOL_REFERENCE_HEADER}
        --commit ${DOBBY_PROTOCOL_REFERENCE_COMMIT}
    DEPENDS
        ${DOBBY_PROTOCOL_REFERENCE_FILES}
        ${PROJECT_SOURCE_DIR}/tools/generate_protocol_reference.py
    VERBATIM)
add_custom_target(dobby_protocol_reference
    DEPENDS ${DOBBY_PROTOCOL_REFERENCE_HEADER})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DOBBY_COMMON_SOURCES
    src/core/config.cpp
    src/core/preferences.cpp
    src/core/runtime_state.cpp
    src/diagnostics/client_schema_trace.cpp
    src/diagnostics/protocol_dump.cpp
    src/diagnostics/report_builder.cpp
    src/diagnostics/stream_probe.cpp
    src/diagnostics/violation_decoder.cpp
    src/metrics/chunk_metrics_layout.cpp
    src/metrics/client_performance.cpp
    src/metrics/network_metrics.cpp
    src/metrics/packet_traffic.cpp
    src/platform/files.cpp
    src/platform/process_memory.cpp
    src/platform/safe_memory.cpp
    src/platform/preferences_store.cpp
    src/ui/chest_esp.cpp
    src/ui/entity_hitbox_overlay.cpp
    src/ui/network_metrics_overlay.cpp
    src/ui/packet_traffic_overlay.cpp
    src/ui/pixel_text.cpp
    src/ui/ore_esp.cpp
    src/ui/window_policy.cpp)

if(ANDROID)
    enable_language(ASM)
    configure_file(
        src/network/protocol_reference_data.S.in
        ${CMAKE_CURRENT_BINARY_DIR}/protocol_reference_data.S
        @ONLY)
    add_library(dobby SHARED
        src/dobby.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/protocol_reference_data.S
        ${DOBBY_COMMON_SOURCES}
        src/hooks/chest_esp_hook.cpp
        src/hooks/chunk_metrics_hook.cpp
        src/hooks/entity_hitbox_hook.cpp
        src/hooks/minecraft_image.cpp
        src/hooks/network_metrics_hook.cpp
        src/hooks/ore_esp_scanner.cpp
        src/hooks/outbound_packet_hook.cpp
        src/hooks/overlay_camera_hook.cpp
        src/hooks/packet_hooks.cpp
        src/hooks/protocol_dump_hook.cpp
        src/hooks/packet_traffic_hook.cpp
        src/hooks/render_camera.cpp
        src/platform/launcher.cpp
        src/platform/log.cpp
        src/ui/developer_ui.cpp)
    target_include_directories(dobby PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        src)
    add_dependencies(dobby dobby_protocol_reference)
    set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/protocol_reference_data.S
        PROPERTY OBJECT_DEPENDS "${DOBBY_PROTOCOL_REFERENCE_FILES}")
    target_compile_options(dobby PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    target_link_libraries(dobby PRIVATE dl)
    target_link_libraries(dobby PRIVATE log)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(dobby PRIVATE
            -Wl,--version-script,${PROJECT_SOURCE_DIR}/exports.map
            -Wl,--no-undefined-version)
        set_target_properties(dobby PROPERTIES
            LINK_DEPENDS ${PROJECT_SOURCE_DIR}/exports.map)
    endif()
else()
    enable_testing()
    add_executable(dobby_tests tests/dobby_tests.cpp ${DOBBY_COMMON_SOURCES})
    target_include_directories(dobby_tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        src)
    add_dependencies(dobby_tests dobby_protocol_reference)
    target_compile_options(dobby_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dobby_tests COMMAND dobby_tests)
endif()
