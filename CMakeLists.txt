cmake_minimum_required(VERSION 3.24)

project(PUBGAssistant-cpp LANGUAGES CXX)

include(cmake/msvc_ninja_dependencies.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)

set(CMAKE_AUTOMOC ON)

# Application builds should not pull test executables into the default build.
# Dedicated test presets enable this option explicitly in an isolated build tree.
option(BUILD_TESTING "Build PUBGAssistant-cpp tests" OFF)
include(CTest)

add_subdirectory(modules/pubg_ai_aim)

add_library(ai_aim_settings_core STATIC
    src/AiAimSettings.cpp
)
add_library(pubg::ai_aim_settings_core ALIAS ai_aim_settings_core)
target_compile_features(ai_aim_settings_core PUBLIC cxx_std_20)
target_include_directories(ai_aim_settings_core PUBLIC include)
target_link_libraries(ai_aim_settings_core PUBLIC
    pubg_ai_aim::pubg_ai_aim
    nlohmann_json::nlohmann_json
)
set_target_properties(ai_aim_settings_core PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_options(ai_aim_settings_core PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(ai_aim_settings_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(config_core STATIC
    src/Config.cpp
    src/ResourcePaths.cpp
)
add_library(pubg::config_core ALIAS config_core)
target_compile_features(config_core PUBLIC cxx_std_20)
target_include_directories(config_core PUBLIC
    include
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(config_core PUBLIC
    pubg::ai_aim_settings_core
    nlohmann_json::nlohmann_json
    ${OpenCV_LIBS}
)
set_target_properties(config_core PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_definitions(config_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(config_core PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(config_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(ai_aim_runtime_preflight_core STATIC
    src/AiAimRuntimePreflight.cpp
)
add_library(pubg::ai_aim_runtime_preflight_core ALIAS ai_aim_runtime_preflight_core)
target_compile_features(ai_aim_runtime_preflight_core PUBLIC cxx_std_20)
target_include_directories(ai_aim_runtime_preflight_core PUBLIC include)
set_target_properties(ai_aim_runtime_preflight_core PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_definitions(ai_aim_runtime_preflight_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ai_aim_runtime_preflight_core PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(ai_aim_runtime_preflight_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(ai_aim_host_context_core STATIC
    src/AiAimHostContext.cpp
)
add_library(pubg::ai_aim_host_context_core ALIAS ai_aim_host_context_core)
target_compile_features(ai_aim_host_context_core PUBLIC cxx_std_20)
target_include_directories(ai_aim_host_context_core PUBLIC include)
target_link_libraries(ai_aim_host_context_core PUBLIC
    pubg_ai_aim::pubg_ai_aim)
set_target_properties(ai_aim_host_context_core PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_options(ai_aim_host_context_core PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(ai_aim_host_context_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    target_link_libraries(ai_aim_runtime_preflight_core PUBLIC bcrypt)
endif()

add_library(mouse_motion_dispatcher_core STATIC
    src/MouseMotionDispatcher.cpp
)
add_library(pubg::mouse_motion_dispatcher_core ALIAS mouse_motion_dispatcher_core)
target_compile_features(mouse_motion_dispatcher_core PUBLIC cxx_std_20)
target_include_directories(mouse_motion_dispatcher_core PUBLIC include)
target_link_libraries(mouse_motion_dispatcher_core PUBLIC Threads::Threads)
set_target_properties(mouse_motion_dispatcher_core PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_options(mouse_motion_dispatcher_core PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(mouse_motion_dispatcher_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(ai_aim_coordinator_core STATIC
    src/AiAimCoordinator.cpp
)
add_library(pubg::ai_aim_coordinator_core ALIAS ai_aim_coordinator_core)
target_compile_features(ai_aim_coordinator_core PUBLIC cxx_std_20)
target_include_directories(ai_aim_coordinator_core PUBLIC include)
target_link_libraries(ai_aim_coordinator_core PUBLIC
    pubg_ai_aim::pubg_ai_aim
    pubg::mouse_motion_dispatcher_core
    Threads::Threads
)
set_target_properties(ai_aim_coordinator_core PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_options(ai_aim_coordinator_core PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(ai_aim_coordinator_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(PUBGAssistant-cpp
    resources/PUBGAssistant-cpp.manifest
    src/main.cpp
    src/App.cpp
    src/AiAimCoordinatorProduction.cpp
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
    src/InputControllerMouseMotionBackend.cpp
    src/HotkeyManager.cpp
    src/LargeMapRadar.cpp
    src/MapPointAssistant.cpp
    src/MortarAutoAim.cpp
    src/MortarMountDetector.cpp
    src/ThrowablesAssistant.cpp
    src/C4Assistant.cpp
    src/ScopeMotionTracker.cpp
    src/ui/MainWindow.cpp
    src/ui/RoundedButton.cpp
    src/ui/Theme.cpp
    src/ui/CurveEditor.cpp
    src/ui/RegionCalibrationOverlay.cpp
    src/ui/ScaleCalibrationWindow.cpp
    src/ui/RecoilDebuggerWindow.cpp
    src/ui/SpecialWeaponDebuggerWindow.cpp
    src/ui/DisplaySettingsWindow.cpp
    src/ui/AiAimSettingsWindow.cpp
    src/ui/AiAimParameterCatalog.cpp
    include/ui/MainWindow.hpp
    include/ui/RoundedButton.hpp
    include/ui/Theme.hpp
    include/ui/CurveEditor.hpp
    include/ui/RegionCalibrationOverlay.hpp
    include/ui/ScaleCalibrationWindow.hpp
    include/ui/RecoilDebuggerWindow.hpp
    include/ui/SpecialWeaponDebuggerWindow.hpp
    include/ui/DisplaySettingsWindow.hpp
    include/ui/AiAimSettingsWindow.hpp
    include/ui/AiAimParameterCatalog.hpp
    include/MortarAutoAim.hpp
    include/MortarMountDetector.hpp
    include/AiAimCoordinator.hpp
    include/AiAimCoordinatorProduction.hpp
    include/MouseMotionDispatcher.hpp
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
    pubg::ai_aim_settings_core
    pubg::ai_aim_coordinator_core
    pubg::ai_aim_runtime_preflight_core
    pubg::ai_aim_host_context_core
    pubg::config_core
    pubg::mouse_motion_dispatcher_core
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

# The application depends on this always-out-of-date target, so both a relink and
# an assets/config-only build use exactly one synchronization implementation.
add_custom_target(sync_runtime_config
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/assets
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/config
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/icon.ico
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/icon.ico
    COMMAND ${CMAKE_COMMAND}
        -DOPTIONAL_SOURCE=${CMAKE_SOURCE_DIR}/README.pdf
        -DOPTIONAL_DESTINATION=$<TARGET_FILE_DIR:PUBGAssistant-cpp>/README.pdf
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_optional_file.cmake
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/ai_aim_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/modules/pubg_ai_aim/runtime
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/ai_aim_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/modules/pubg_ai_aim/LICENSE.md
        ${CMAKE_SOURCE_DIR}/modules/pubg_ai_aim/THIRD_PARTY_NOTICES.md
        $<TARGET_FILE_DIR:PUBGAssistant-cpp>/ai_aim_runtime/
    COMMENT "Synchronizing PUBGAssistant-cpp runtime assets and configuration"
)
add_dependencies(PUBGAssistant-cpp sync_runtime_config)

if(BUILD_TESTING)
    add_executable(display_settings_window_tests
        tests/DisplaySettingsWindowTests.cpp
        include/ui/DisplaySettingsWindow.hpp
        src/ui/DisplaySettingsWindow.cpp
        src/ui/Theme.cpp
    )
    target_link_libraries(display_settings_window_tests PRIVATE pubg::config_core Qt6::Widgets)
    set_target_properties(display_settings_window_tests PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_definitions(display_settings_window_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(display_settings_window_tests PRIVATE /utf-8 /W4 /permissive-)
        target_link_libraries(display_settings_window_tests PRIVATE user32 gdi32 dwmapi)
    endif()
    add_test(NAME display_settings_window_tests COMMAND display_settings_window_tests -platform minimal)
    set_tests_properties(display_settings_window_tests PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(display_settings_window_tests PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_VCPKG_PREFIX}/$<IF:$<CONFIG:Debug>,debug/Qt6,Qt6>/plugins/platforms")
    endif()

    add_executable(status_hud_layout_tests tests/StatusHudLayoutTests.cpp)
    target_include_directories(status_hud_layout_tests PRIVATE include)
    set_target_properties(status_hud_layout_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(status_hud_layout_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME status_hud_layout_tests COMMAND status_hud_layout_tests)
    set_tests_properties(status_hud_layout_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_hud_indicator_tests tests/AiAimHudIndicatorTests.cpp)
    target_include_directories(ai_aim_hud_indicator_tests PRIVATE include)
    set_target_properties(ai_aim_hud_indicator_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(ai_aim_hud_indicator_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME ai_aim_hud_indicator_tests COMMAND ai_aim_hud_indicator_tests)
    set_tests_properties(ai_aim_hud_indicator_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_settings_window_tests
        tests/AiAimSettingsWindowTests.cpp
        src/ui/AiAimSettingsWindow.cpp
        src/ui/AiAimParameterCatalog.cpp
        src/ui/Theme.cpp
    )
    target_link_libraries(ai_aim_settings_window_tests PRIVATE pubg::config_core Qt6::Widgets)
    set_target_properties(ai_aim_settings_window_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_definitions(ai_aim_settings_window_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(ai_aim_settings_window_tests PRIVATE /utf-8 /W4 /permissive-)
        target_link_libraries(ai_aim_settings_window_tests PRIVATE user32 gdi32 dwmapi)
    endif()
    add_test(NAME ai_aim_settings_window_tests COMMAND ai_aim_settings_window_tests -platform minimal)
    set_tests_properties(ai_aim_settings_window_tests PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(ai_aim_settings_window_tests PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_VCPKG_PREFIX}/$<IF:$<CONFIG:Debug>,debug/Qt6,Qt6>/plugins/platforms")
    endif()

    add_executable(overlay_window_message_tests
        tests/OverlayWindowMessageTests.cpp
        src/OverlayWindow.cpp
    )
    target_include_directories(overlay_window_message_tests PRIVATE
        include
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(overlay_window_message_tests PRIVATE
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
    )
    set_target_properties(overlay_window_message_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_definitions(overlay_window_message_tests PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        target_compile_options(overlay_window_message_tests PRIVATE /utf-8 /W4 /permissive-)
        target_link_libraries(overlay_window_message_tests PRIVATE user32 gdi32 gdiplus)
    endif()
    add_test(NAME overlay_window_message_tests COMMAND overlay_window_message_tests)
    set_tests_properties(overlay_window_message_tests PROPERTIES TIMEOUT 20)

    add_executable(equipment_selection_tests
        tests/EquipmentSelectionTests.cpp
    )
    target_include_directories(equipment_selection_tests PRIVATE include)
    set_target_properties(equipment_selection_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(equipment_selection_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME equipment_selection_tests COMMAND equipment_selection_tests)
    set_tests_properties(equipment_selection_tests PROPERTIES TIMEOUT 20)

    add_executable(primary_hotkeys_tests
        tests/PrimaryHotkeysTests.cpp
    )
    target_include_directories(primary_hotkeys_tests PRIVATE include)
    set_target_properties(primary_hotkeys_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(primary_hotkeys_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME primary_hotkeys_tests COMMAND primary_hotkeys_tests)
    set_tests_properties(primary_hotkeys_tests PROPERTIES TIMEOUT 20)

    add_executable(shutdown_sequence_tests
        tests/ShutdownSequenceTests.cpp
    )
    target_include_directories(shutdown_sequence_tests PRIVATE include)
    set_target_properties(shutdown_sequence_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(shutdown_sequence_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME shutdown_sequence_tests COMMAND shutdown_sequence_tests)
    set_tests_properties(shutdown_sequence_tests PROPERTIES TIMEOUT 20)

    add_executable(scope_exit_tests
        tests/ScopeExitTests.cpp
    )
    target_include_directories(scope_exit_tests PRIVATE include)
    set_target_properties(scope_exit_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(scope_exit_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME scope_exit_tests COMMAND scope_exit_tests)
    set_tests_properties(scope_exit_tests PROPERTIES TIMEOUT 20)

    add_executable(latest_render_publication_tests
        tests/LatestRenderPublicationTests.cpp
    )
    target_include_directories(latest_render_publication_tests PRIVATE include)
    set_target_properties(latest_render_publication_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(latest_render_publication_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME latest_render_publication_tests COMMAND latest_render_publication_tests)
    set_tests_properties(latest_render_publication_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_runtime_error_tracker_tests
        tests/AiAimRuntimeErrorTrackerTests.cpp
    )
    target_include_directories(ai_aim_runtime_error_tracker_tests PRIVATE include)
    set_target_properties(ai_aim_runtime_error_tracker_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(ai_aim_runtime_error_tracker_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME ai_aim_runtime_error_tracker_tests COMMAND ai_aim_runtime_error_tracker_tests)
    set_tests_properties(ai_aim_runtime_error_tracker_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_host_context_tests
        tests/AiAimHostContextTests.cpp
    )
    target_link_libraries(ai_aim_host_context_tests PRIVATE
        pubg::ai_aim_host_context_core)
    set_target_properties(ai_aim_host_context_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(ai_aim_host_context_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME ai_aim_host_context_tests COMMAND ai_aim_host_context_tests)
    set_tests_properties(ai_aim_host_context_tests PROPERTIES TIMEOUT 20)

    add_executable(game_menu_state_tracker_tests
        tests/GameMenuStateTrackerTests.cpp
    )
    target_include_directories(game_menu_state_tracker_tests PRIVATE include)
    set_target_properties(game_menu_state_tracker_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(game_menu_state_tracker_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME game_menu_state_tracker_tests COMMAND game_menu_state_tracker_tests)
    set_tests_properties(game_menu_state_tracker_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_runtime_preflight_tests
        tests/AiAimRuntimePreflightTests.cpp
    )
    target_link_libraries(ai_aim_runtime_preflight_tests PRIVATE
        pubg::ai_aim_runtime_preflight_core)
    target_compile_definitions(ai_aim_runtime_preflight_tests PRIVATE
        PUBG_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_target_properties(ai_aim_runtime_preflight_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(ai_aim_runtime_preflight_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME ai_aim_runtime_preflight_tests COMMAND ai_aim_runtime_preflight_tests)
    set_tests_properties(ai_aim_runtime_preflight_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_config_persistence_tests
        tests/AiAimConfigPersistenceTests.cpp
    )
    target_link_libraries(ai_aim_config_persistence_tests PRIVATE
        pubg::config_core)
    set_target_properties(ai_aim_config_persistence_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(ai_aim_config_persistence_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME ai_aim_config_persistence_tests COMMAND ai_aim_config_persistence_tests)
    set_tests_properties(ai_aim_config_persistence_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_settings_tests
        tests/AiAimSettingsTests.cpp
    )
    target_link_libraries(ai_aim_settings_tests PRIVATE
        pubg::ai_aim_settings_core)
    target_compile_definitions(ai_aim_settings_tests PRIVATE
        PUBG_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    set_target_properties(ai_aim_settings_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(ai_aim_settings_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME ai_aim_settings_tests COMMAND ai_aim_settings_tests)
    set_tests_properties(ai_aim_settings_tests PROPERTIES TIMEOUT 20)

    add_executable(mouse_motion_dispatcher_tests
        tests/MouseMotionDispatcherTests.cpp
    )
    target_link_libraries(mouse_motion_dispatcher_tests PRIVATE
        pubg::mouse_motion_dispatcher_core)
    set_target_properties(mouse_motion_dispatcher_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(mouse_motion_dispatcher_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME mouse_motion_dispatcher_tests COMMAND mouse_motion_dispatcher_tests)
    set_tests_properties(mouse_motion_dispatcher_tests PROPERTIES TIMEOUT 20)

    add_executable(ai_aim_coordinator_tests
        tests/AiAimCoordinatorTests.cpp
    )
    target_link_libraries(ai_aim_coordinator_tests PRIVATE
        pubg::ai_aim_coordinator_core)
    set_target_properties(ai_aim_coordinator_tests PROPERTIES AUTOMOC OFF)
    if(MSVC)
        target_compile_options(ai_aim_coordinator_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME ai_aim_coordinator_tests COMMAND ai_aim_coordinator_tests)
    set_tests_properties(ai_aim_coordinator_tests PROPERTIES TIMEOUT 20)
endif()

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
