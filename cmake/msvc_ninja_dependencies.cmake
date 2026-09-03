# Some localized MSVC installations emit UTF-8 while CMake's compiler probe
# decodes using the legacy console codepage. Ninja then records zero headers.
# Preserve the compiler's literal bytes so incremental builds cannot mix ABIs.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set(pubg_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/PubgDependencyProbe")
    file(MAKE_DIRECTORY "${pubg_probe_dir}")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c
            "${CMAKE_CURRENT_LIST_DIR}/msvc_dependency_probe.cpp"
            "/Fo${pubg_probe_dir}/probe.obj"
        WORKING_DIRECTORY "${pubg_probe_dir}"
        OUTPUT_VARIABLE pubg_probe_output
        ERROR_VARIABLE pubg_probe_error
        RESULT_VARIABLE pubg_probe_result
        ENCODING NONE
    )
    if(NOT pubg_probe_result EQUAL 0 OR NOT pubg_probe_output MATCHES
        "(^|\n)([^:\r\n]+:[^:\r\n]+:[ \t]+)[A-Za-z]:[/\\]")
        message(FATAL_ERROR "Cannot determine MSVC include dependency prefix: ${pubg_probe_error}")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
endif()
