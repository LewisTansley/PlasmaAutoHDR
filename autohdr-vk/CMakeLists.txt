cmake_minimum_required(VERSION 3.16)

project(autohdr-vk VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../autohdr-core autohdr-core-build)

find_program(GLSLC glslc REQUIRED)

set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

add_custom_command(
    OUTPUT ${SHADER_OUTPUT_DIR}/fullscreen.vert.spv ${SHADER_OUTPUT_DIR}/autohdr.frag.spv
    COMMAND ${GLSLC} -o ${SHADER_OUTPUT_DIR}/fullscreen.vert.spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert
    COMMAND ${GLSLC} -o ${SHADER_OUTPUT_DIR}/autohdr.frag.spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/autohdr.frag
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/autohdr.frag
    COMMENT "Compiling AutoHDR Vulkan shaders"
)

add_library(autohdr_vk SHARED
    src/layer.cpp
    src/config_state.cpp
    ${SHADER_OUTPUT_DIR}/fullscreen.vert.spv
    ${SHADER_OUTPUT_DIR}/autohdr.frag.spv
)

target_include_directories(autohdr_vk
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../autohdr-core/include
)

target_link_libraries(autohdr_vk PRIVATE Vulkan::Vulkan autohdr_core)
set_target_properties(autohdr_vk PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME "autohdr-vk")

target_compile_definitions(autohdr_vk PRIVATE
    AUTOHDR_VK_VERT_SPIRV_PATH="${SHADER_OUTPUT_DIR}/fullscreen.vert.spv"
    AUTOHDR_VK_FRAG_SPIRV_PATH="${SHADER_OUTPUT_DIR}/autohdr.frag.spv"
)

install(TARGETS autohdr_vk LIBRARY DESTINATION lib)
install(FILES layer/VkLayer_AUTOHDR.json DESTINATION share/vulkan/implicit_layer.d)

add_custom_target(autohdr_vk_shaders DEPENDS ${SHADER_OUTPUT_DIR}/fullscreen.vert.spv ${SHADER_OUTPUT_DIR}/autohdr.frag.spv)
add_dependencies(autohdr_vk autohdr_vk_shaders)

install(PROGRAMS scripts/install-layer.sh DESTINATION bin)
