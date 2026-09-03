if(PUBG_AI_AIM_BUILD_TESTS)
    add_executable(pubg_ai_aim_core_tests core_tests.cpp)
    target_link_libraries(pubg_ai_aim_core_tests PRIVATE pubg_ai_aim::pubg_ai_aim)
    if(MSVC)
        target_compile_options(pubg_ai_aim_core_tests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME pubg_ai_aim_core_tests COMMAND pubg_ai_aim_core_tests)
    set_tests_properties(pubg_ai_aim_core_tests PROPERTIES TIMEOUT 15)
endif()

if(PUBG_AI_AIM_BUILD_ORT_SMOKE_TEST
        AND WIN32
        AND PUBG_AI_AIM_ENABLE_WINDOWS_BACKENDS)
    add_executable(pubg_ai_aim_ort_smoke ort_smoke.cpp)
    target_link_libraries(pubg_ai_aim_ort_smoke PRIVATE pubg_ai_aim::pubg_ai_aim)
    if(MSVC)
        target_compile_options(pubg_ai_aim_ort_smoke PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(
        NAME pubg_ai_aim_ort_cpu_smoke
        COMMAND pubg_ai_aim_ort_smoke
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/onnxruntime.dll"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/PUBGV8.onnx"
            --cpu)
    set_tests_properties(pubg_ai_aim_ort_cpu_smoke PROPERTIES TIMEOUT 30)
endif()
