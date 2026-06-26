cmake_minimum_required(VERSION 3.16)

project(autohdr-core VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(autohdr_core STATIC
    src/tone_curve.cpp
    src/tone_curve_presets.cpp
    src/tone_curve_user_presets.cpp
    src/config.cpp
    src/display_limits.cpp
)

set_target_properties(autohdr_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(autohdr_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

enable_testing()

add_executable(autohdr_core_tests tests/test_lut.cpp)
target_link_libraries(autohdr_core_tests PRIVATE autohdr_core)
add_test(NAME autohdr_core_tests COMMAND autohdr_core_tests)

install(TARGETS autohdr_core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/autohdr DESTINATION include)
