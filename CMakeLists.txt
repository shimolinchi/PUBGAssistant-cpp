cmake_minimum_required(VERSION 3.24)

project(PUBGAssistant-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(PUBGAssistant-cpp
    resources/PUBGAssistant-cpp.manifest
    src/main.cpp
    src/App.cpp
    src/Config.cpp
    src/RegionManager.cpp
    src/ScreenCapture.cpp
    src/TemplateMatcher.cpp
    src/MinimapRadar.cpp
    src/ElevationRadar.cpp
    src/WeaponDetector.cpp
    src/EquipmentDetector.cpp
    src/GestureIdentifier.cpp
    src/RecoilControl.cpp
    src/Ballistics.cpp
    src/SpecialAssistants.cpp
    src/StatusHud.cpp
    src/OverlayWindow.cpp
    src/InputController.cpp
    src/HotkeyManager.cpp
    src/ResourcePaths.cpp
    src/LargeMapRadar.cpp
    src/MapPointAssistant.cpp
    src/MortarAutoAim.cpp
    src/MortarMountDetector.cpp
    src/ThrowablesAssistant.cpp
    src/C4Assistant.cpp
    src/ScopeMotionTracker.cpp
    src/ui/MainWindow.cpp
    src/ui/RoundedButton.cpp
    src/ui/CurveEditor.cpp
    src/ui/RegionCalibrationOverlay.cpp
    src/ui/ScaleCalibrationWindow.cpp
    src/ui/RecoilDebuggerWindow.cpp
    src/ui/SpecialWeaponDebuggerWindow.cpp
    include/ui/MainWindow.hpp
    include/ui/RoundedButton.hpp
    include/ui/CurveEditor.hpp
    include/ui/RegionCalibrationOverlay.hpp
    include/ui/ScaleCalibrationWindow.hpp
    include/ui/RecoilDebuggerWindow.hpp
    include/ui/SpecialWeaponDebuggerWindow.hpp
    include/MortarAutoAim.hpp
    include/MortarMountDetector.hpp
)

target_include_directories(PUBGAssistant-cpp PRIVATE
    include
    include/ui
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(PUBGAssistant-cpp PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    Qt6::Widgets
)

if(WIN32)
    target_sources(PUBGAssistant-cpp PRIVATE
        resources/app_icon.rc
    )
    get_filename_component(QT_VCPKG_PREFIX "${Qt6_DIR}/../.." ABSOLUTE)
    target_compile_definitions(PUBGAssistant-cpp PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_compile_options(PUBGAssistant-cpp PRIVATE /utf-8)
    target_link_options(PUBGAssistant-cpp PRIVATE
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
    target_link_libraries(PUBGAssistant-cpp PRIVATE
        user32
        gdi32
        gdiplus
        dwmapi
        shcore
        shell32
    )

    add_custom_command(TARGET PUBGAssistant-cpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:PUBGAssistant-cpp>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_VCPKG_PREFIX}/$<IF:$<CONFIG:Debug>,debug/Qt6,Qt6>/plugins/platforms/$<IF:$<CONFIG:Debug>,qwindowsd.dll,qwindows.dll>"
            $<TARGET_FILE_DIR:PUBGAssistant-cpp>/platforms/
    )
endif()

add_custom_command(TARGET PUBGAssistant-cpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/icon.ico
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/icon.ico
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_target(package_release_runtime ALL
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DBUILD_DIR=$<TARGET_FILE_DIR:PUBGAssistant-cpp>
            -DPACKAGE_DIR=${CMAKE_SOURCE_DIR}/.build/package/release
            -P ${CMAKE_SOURCE_DIR}/cmake/package_runtime.cmake
        DEPENDS PUBGAssistant-cpp
        COMMENT "Packaging PUBGAssistant-cpp release runtime"
    )
endif()
