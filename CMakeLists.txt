cmake_minimum_required(VERSION 3.24)

project(PUBGAssistantCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(PUBGAssistantCpp
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
)

target_include_directories(PUBGAssistantCpp PRIVATE
    include
    include/ui
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(PUBGAssistantCpp PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    Qt6::Widgets
)

if(WIN32)
    get_filename_component(QT_VCPKG_PREFIX "${Qt6_DIR}/../.." ABSOLUTE)
    target_compile_definitions(PUBGAssistantCpp PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_link_libraries(PUBGAssistantCpp PRIVATE
        user32
        gdi32
        gdiplus
        dwmapi
        shcore
        shell32
    )

    add_custom_command(TARGET PUBGAssistantCpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:PUBGAssistantCpp>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_VCPKG_PREFIX}/$<IF:$<CONFIG:Debug>,debug/Qt6,Qt6>/plugins/platforms/$<IF:$<CONFIG:Debug>,qwindowsd.dll,qwindows.dll>"
            $<TARGET_FILE_DIR:PUBGAssistantCpp>/platforms/
    )
endif()

add_custom_command(TARGET PUBGAssistantCpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:PUBGAssistantCpp>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:PUBGAssistantCpp>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/icon.ico
        $<TARGET_FILE_DIR:PUBGAssistantCpp>/icon.ico
)
