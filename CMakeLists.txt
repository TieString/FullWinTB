cmake_minimum_required(VERSION 3.25)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
project(FullWinTB VERSION 0.1.1 LANGUAGES CXX RC)
# Use the existing Explorer component for updates confined to the EXE.
set(FULLWINTB_APPEARANCE_VERSION 0.1.6)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(fullwintb_webview2
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3650.58/microsoft.web.webview2.1.0.3650.58.nupkg
    DOWNLOAD_NAME microsoft.web.webview2.zip
    URL_HASH SHA256=911a472128c82ac8baa0c486c23342cc9dd6e7dc50d754e676726642ca065c60
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(fullwintb_webview2)
set(webview2_native "${fullwintb_webview2_SOURCE_DIR}/build/native")
string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" webview2_arch)

set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/version.h.in "${generated_dir}/version.h" @ONLY)

function(fullwintb_native target)
    target_include_directories(${target} PRIVATE src "${generated_dir}")
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/EHsc>
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/Os;/GL>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>)
    endif()
endfunction()

add_library(FullWinTBAppearance SHARED src/taskbar_appearance.cpp src/taskbar_region.cpp
    src/taskbar_layout.cpp src/taskbar_layer.cpp src/taskbar_latch.cpp src/taskbar_preview.cpp
    src/taskbar_overflow.cpp src/taskbar_wave.cpp src/taskbar_start.cpp src/taskbar_pin_input.cpp
    src/taskbar_icon_ink.cpp src/sticker_atlas.rc)
set_source_files_properties(src/sticker_atlas.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sticker_atlas.bin")
fullwintb_native(FullWinTBAppearance)
target_link_libraries(FullWinTBAppearance PRIVATE user32 gdi32 shell32 comctl32 advapi32 ole32 oleaut32 windowsapp)

file(READ "src/feel/FEEL-LICENSE.txt" feel_license)
file(READ "src/sticker-forge-LICENSE.txt" sticker_forge_license)
file(READ "${fullwintb_webview2_SOURCE_DIR}/LICENSE.txt" webview2_license)
file(READ "${fullwintb_webview2_SOURCE_DIR}/NOTICE.txt" webview2_notices)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "src/feel/FEEL-LICENSE.txt" "src/sticker-forge-LICENSE.txt"
    "${fullwintb_webview2_SOURCE_DIR}/LICENSE.txt" "${fullwintb_webview2_SOURCE_DIR}/NOTICE.txt")
set(third_party_notices "${generated_dir}/Third-Party-Notices.txt")
file(GENERATE OUTPUT "${third_party_notices}" CONTENT
    "Feel · https://github.com/diip3sh/feel\n${feel_license}\nSticker Forge (rendered peel frames) · https://github.com/CatsJuice/sticker-forge\n${sticker_forge_license}\nMicrosoft WebView2 Loader\n${webview2_license}\n${webview2_notices}")
set(embed_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_appearance.cmake")
set(embedded_rc "${generated_dir}/$<CONFIG>/embedded.rc")
add_custom_command(OUTPUT "${embedded_rc}"
    COMMAND "${CMAKE_COMMAND}"
        "-Dcomponent=$<TARGET_FILE:FullWinTBAppearance>"
        "-Dnotices=${third_party_notices}"
        "-Doutput=${embedded_rc}"
        -P "${embed_script}"
    DEPENDS FullWinTBAppearance "${third_party_notices}" "${embed_script}"
    VERBATIM)

add_executable(FullWinTB WIN32 src/main.cpp src/overlay.cpp src/work_area.cpp src/tray.cpp
    src/appearance.cpp src/embedded_component.cpp src/ink_sampling.cpp src/motion_dialog.cpp
    src/fullwintb.rc "${embedded_rc}")
fullwintb_native(FullWinTB)
target_include_directories(FullWinTB PRIVATE "${webview2_native}/include")
target_link_libraries(FullWinTB PRIVATE user32 gdi32 shell32 shlwapi advapi32 comctl32 dwmapi ole32 oleaut32 wtsapi32
    "${webview2_native}/${webview2_arch}/WebView2LoaderStatic.lib")
set_source_files_properties(src/fullwintb.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/feel/motion.html;${CMAKE_CURRENT_SOURCE_DIR}/src/feel/feel.css;${CMAKE_CURRENT_SOURCE_DIR}/src/feel/feel.js;${CMAKE_CURRENT_SOURCE_DIR}/src/feel/rig.js;${CMAKE_CURRENT_SOURCE_DIR}/src/feel/preview.js")
set_property(SOURCE src/fullwintb.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/media/FullWinTB.ico"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fullwintb_disabled.ico")
if(MINGW)
    target_link_options(FullWinTB PRIVATE -municode)
endif()

install(TARGETS FullWinTB RUNTIME DESTINATION .)

option(BUILD_TESTING "Build FullWinTB's geometry tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_executable(FullWinTBGeometryTests tests/geometry_tests.cpp src/ink_sampling.cpp
        src/taskbar_region.cpp src/taskbar_layer.cpp src/sticker_atlas.rc)
    fullwintb_native(FullWinTBGeometryTests)
    target_link_libraries(FullWinTBGeometryTests PRIVATE user32 gdi32 comctl32)
    add_test(NAME FullWinTBGeometry COMMAND FullWinTBGeometryTests)
    add_test(NAME FullWinTBCanvas COMMAND FullWinTBGeometryTests --canvas)
    set_tests_properties(FullWinTBCanvas PROPERTIES SKIP_RETURN_CODE 77)
endif()
