cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR OR NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "SOURCE_DIR, BUILD_DIR and PACKAGE_DIR are required")
endif()

file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}/platforms")

file(COPY "${BUILD_DIR}/PUBGAssistantCpp.exe" DESTINATION "${PACKAGE_DIR}")
file(COPY "${SOURCE_DIR}/assets" DESTINATION "${PACKAGE_DIR}")
file(COPY "${SOURCE_DIR}/config" DESTINATION "${PACKAGE_DIR}")
file(COPY "${SOURCE_DIR}/icon.ico" DESTINATION "${PACKAGE_DIR}")

set(VCPKG_RELEASE_DIR "${BUILD_DIR}/vcpkg_installed/x64-windows")
set(VCPKG_RELEASE_BIN_DIR "${VCPKG_RELEASE_DIR}/bin")
set(VCPKG_RELEASE_QT_PLATFORM_DIR "${VCPKG_RELEASE_DIR}/Qt6/plugins/platforms")

set(required_dlls
    double-conversion.dll
    jpeg62.dll
    libpng16.dll
    md4c.dll
    opencv_core4.dll
    opencv_imgcodecs4.dll
    opencv_imgproc4.dll
    pcre2-16.dll
    Qt6Core.dll
    Qt6Gui.dll
    Qt6Widgets.dll
    z.dll
)

foreach(dll_name IN LISTS required_dlls)
    if(EXISTS "${VCPKG_RELEASE_BIN_DIR}/${dll_name}")
        file(COPY "${VCPKG_RELEASE_BIN_DIR}/${dll_name}" DESTINATION "${PACKAGE_DIR}")
    elseif(EXISTS "${BUILD_DIR}/${dll_name}")
        file(COPY "${BUILD_DIR}/${dll_name}" DESTINATION "${PACKAGE_DIR}")
    else()
        message(WARNING "Runtime DLL not found: ${VCPKG_RELEASE_BIN_DIR}/${dll_name}")
    endif()
endforeach()

if(EXISTS "${VCPKG_RELEASE_QT_PLATFORM_DIR}/qwindows.dll")
    file(COPY "${VCPKG_RELEASE_QT_PLATFORM_DIR}/qwindows.dll" DESTINATION "${PACKAGE_DIR}/platforms")
elseif(EXISTS "${BUILD_DIR}/platforms/qwindows.dll")
    file(COPY "${BUILD_DIR}/platforms/qwindows.dll" DESTINATION "${PACKAGE_DIR}/platforms")
else()
    message(WARNING "Qt platform plugin not found: ${VCPKG_RELEASE_QT_PLATFORM_DIR}/qwindows.dll")
endif()

file(GLOB vc_redist_dirs
    "C:/Program Files/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"
)
if(vc_redist_dirs)
    list(SORT vc_redist_dirs COMPARE NATURAL ORDER DESCENDING)
    list(GET vc_redist_dirs 0 vc_redist_dir)
    file(GLOB vc_runtime_dlls
        "${vc_redist_dir}/msvcp140.dll"
        "${vc_redist_dir}/vcruntime140.dll"
        "${vc_redist_dir}/vcruntime140_1.dll"
        "${vc_redist_dir}/concrt140.dll"
    )
    foreach(runtime_dll IN LISTS vc_runtime_dlls)
        file(COPY "${runtime_dll}" DESTINATION "${PACKAGE_DIR}")
    endforeach()
else()
    message(WARNING "MSVC redist directory was not found; install the Microsoft Visual C++ x64 Runtime on target machines.")
endif()

file(GLOB ucrt_dirs
    "C:/Program Files (x86)/Windows Kits/10/Redist/*/ucrt/DLLs/x64"
)
if(ucrt_dirs)
    list(SORT ucrt_dirs COMPARE NATURAL ORDER DESCENDING)
    list(GET ucrt_dirs 0 ucrt_dir)
    file(GLOB ucrt_runtime_dlls
        "${ucrt_dir}/ucrtbase.dll"
        "${ucrt_dir}/api-ms-win-crt-*.dll"
    )
    foreach(runtime_dll IN LISTS ucrt_runtime_dlls)
        file(COPY "${runtime_dll}" DESTINATION "${PACKAGE_DIR}")
    endforeach()
else()
    message(WARNING "UCRT redist directory was not found; target machines may need the Universal CRT update or VC runtime installer.")
endif()

file(GLOB_RECURSE debug_runtime_files
    "${PACKAGE_DIR}/*d.dll"
    "${PACKAGE_DIR}/VCRUNTIME*D.dll"
    "${PACKAGE_DIR}/ucrtbased.dll"
    "${PACKAGE_DIR}/*.pdb"
)
if(debug_runtime_files)
    string(REPLACE ";" "\n  " debug_runtime_list "${debug_runtime_files}")
    message(FATAL_ERROR "Debug-only files were copied into the release package:\n  ${debug_runtime_list}")
endif()

file(WRITE "${PACKAGE_DIR}/README_RELEASE.txt"
"PUBGAssistant-cpp release package

Run PUBGAssistantCpp.exe from this folder.

Copy this whole folder to another computer:
  .build/package/release

Do not copy .build/release as the final package. That directory is a CMake/vcpkg build workspace and may contain debug DLLs such as Qt6Cored.dll or opencv_core4d.dll, which require VCRUNTIME140D.dll and ucrtbased.dll.
")
