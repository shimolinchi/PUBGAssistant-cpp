cmake_minimum_required(VERSION 3.24)

set(PUBG_AI_AIM_VERSION 0.1.0)
set(PUBG_AI_AIM_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PUBG_AI_AIM_IS_TOP_LEVEL ON)
    project(pubg_ai_aim VERSION ${PUBG_AI_AIM_VERSION} LANGUAGES CXX)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PUBG_AI_AIM_BUILD_TESTS "Build pubg_ai_aim tests" ${PUBG_AI_AIM_IS_TOP_LEVEL})
option(PUBG_AI_AIM_BUILD_ORT_SMOKE_TEST
    "Build the pubg_ai_aim ONNX Runtime/model smoke test" OFF)
option(PUBG_AI_AIM_BUILD_EXAMPLES "Build pubg_ai_aim examples" ${PUBG_AI_AIM_IS_TOP_LEVEL})
option(PUBG_AI_AIM_ENABLE_WINDOWS_BACKENDS "Build DXGI and dynamically loaded ORT/DirectML adapters" ON)

add_library(pubg_ai_aim STATIC
    src/AimConfig.cpp
    src/Algorithms.cpp
    src/AimEngine.cpp
    src/AimModule.cpp
)
add_library(pubg_ai_aim::pubg_ai_aim ALIAS pubg_ai_aim)

target_compile_features(pubg_ai_aim PUBLIC cxx_std_20)
target_include_directories(pubg_ai_aim
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(WIN32 AND PUBG_AI_AIM_ENABLE_WINDOWS_BACKENDS)
    target_sources(pubg_ai_aim PRIVATE
        src/DxgiFrameSource.cpp
        src/OrtDmlDetector.cpp
    )
    target_include_directories(pubg_ai_aim PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
    )
    target_compile_definitions(pubg_ai_aim PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_link_libraries(pubg_ai_aim PRIVATE d3d11 dxgi)
else()
    target_sources(pubg_ai_aim PRIVATE src/WindowsAdaptersStub.cpp)
endif()

if(MSVC)
    target_compile_options(pubg_ai_aim PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(pubg_ai_aim PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(pubg_ai_aim PROPERTIES
    VERSION ${PUBG_AI_AIM_VERSION}
    SOVERSION 0
    EXPORT_NAME pubg_ai_aim
)

if(PUBG_AI_AIM_BUILD_TESTS OR PUBG_AI_AIM_BUILD_ORT_SMOKE_TEST)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PUBG_AI_AIM_BUILD_EXAMPLES)
    add_executable(pubg_ai_aim_external_example examples/external_detections.cpp)
    target_link_libraries(pubg_ai_aim_external_example PRIVATE pubg_ai_aim::pubg_ai_aim)
endif()

install(TARGETS pubg_ai_aim
    EXPORT pubg_ai_aimTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT pubg_ai_aimTargets
    FILE pubg_ai_aimTargets.cmake
    NAMESPACE pubg_ai_aim::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pubg_ai_aim
)

configure_package_config_file(
    cmake/pubg_ai_aimConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pubg_ai_aimConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pubg_ai_aim
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/pubg_ai_aimConfigVersion.cmake
    VERSION ${PUBG_AI_AIM_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pubg_ai_aimConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/pubg_ai_aimConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pubg_ai_aim
)
