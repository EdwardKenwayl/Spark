cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

project(Spark VERSION 0.1.0 LANGUAGES C CXX)

option(SPARK_BUILD_TESTS "Build standalone agent tests" ON)
option(SPARK_WIN7_COMPAT_PROBE "Build the Windows 7 compatibility probe" ON)
option(SPARK_PLATFORM_TESTS_ONLY "Build portable foundation tests without the Windows UI/runtime" OFF)
option(SPARK_RUNTIME_TESTS_ONLY "Build portable provider/HTTP tests without the native GUI" OFF)
option(SPARK_CORE_ONLY "Build the complete agent and MCP runtime without the GUI" OFF)
option(SPARK_WINDOWS_XP "Build the experimental x86 XP target with an XP toolset and compatible libcurl" OFF)
if(SPARK_WINDOWS_XP)
    if(NOT WIN32 OR NOT MSVC OR MSVC_VERSION GREATER_EQUAL 1920 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "XP requires a 32-bit v141_xp toolset; a recent Windows compiler is not an XP toolchain.")
    endif()
    set(SPARK_WINDOWS_VERSION 0x0501)
    set(SPARK_SUBSYSTEM_VERSION 5.01)
else()
    set(SPARK_WINDOWS_VERSION 0x0601)
    set(SPARK_SUBSYSTEM_VERSION 6.01)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(Threads REQUIRED)

add_library(spark_foundation STATIC
    src/core/common.cpp
    src/platform/user_directories.cpp
    src/core/agent_instructions.cpp
    src/core/agent_types.cpp
    src/core/config.cpp
    src/core/model_catalog.cpp
    src/core/context_compactor.cpp
    src/core/dsml_tool_calls.cpp
    src/core/skills.cpp
    src/core/session.cpp
    src/platform/journal_io.cpp
    src/render/markdown.cpp
    third_party/md4c/md4c.c
)
target_include_directories(spark_foundation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c
)
target_link_libraries(spark_foundation PUBLIC Threads::Threads)
if(WIN32)
    target_sources(spark_foundation PRIVATE
        src/platform/directory_enumeration.cpp src/platform/windows_compat.cpp)
    target_compile_definitions(spark_foundation PUBLIC
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=${SPARK_WINDOWS_VERSION} WINVER=${SPARK_WINDOWS_VERSION} MD4C_USE_UTF16
    )
    if(SPARK_WINDOWS_XP)
        target_compile_definitions(spark_foundation PUBLIC SPARK_WINDOWS_XP=1)
    endif()
    target_link_libraries(spark_foundation PUBLIC shell32)
else()
    target_sources(spark_foundation PRIVATE src/platform/common_posix.cpp)
endif()
if(MSVC)
    target_compile_options(spark_foundation PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/utf-8 /Zc:__cplusplus /EHsc>)
    if(NOT SPARK_WINDOWS_XP)
        target_compile_options(spark_foundation PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
    endif()
    target_compile_options(spark_foundation PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /wd4100 /wd4127>)
endif()
if(SPARK_BUILD_TESTS)
    enable_testing()
    add_executable(spark_platform_tests tests/platform_tests.cpp)
    target_link_libraries(spark_platform_tests PRIVATE spark_foundation)
    add_test(NAME spark_platform_tests COMMAND spark_platform_tests)
    set_tests_properties(spark_platform_tests PROPERTIES TIMEOUT 15)
    add_executable(spark_platform_session_tests tests/platform_session_tests.cpp)
    target_link_libraries(spark_platform_session_tests PRIVATE spark_foundation)
    add_test(NAME spark_platform_session_tests COMMAND spark_platform_session_tests)
    set_tests_properties(spark_platform_session_tests PROPERTIES TIMEOUT 15)
    add_executable(spark_markdown_tests tests/markdown_tests.cpp)
    target_link_libraries(spark_markdown_tests PRIVATE spark_foundation)
    add_test(NAME spark_markdown_tests COMMAND spark_markdown_tests)
    if(WIN32)
        add_executable(spark_directory_enumeration_tests tests/directory_enumeration_tests.cpp)
        target_link_libraries(spark_directory_enumeration_tests PRIVATE spark_foundation)
        add_test(NAME spark_directory_enumeration_tests COMMAND spark_directory_enumeration_tests)
        set_tests_properties(spark_directory_enumeration_tests PROPERTIES TIMEOUT 10)
    endif()
endif()
if(SPARK_PLATFORM_TESTS_ONLY)
    return()
endif()

add_library(spark_provider STATIC
    src/core/provider.cpp
    src/core/provider_models.cpp
    src/core/provider_probe.cpp
    src/platform/binary_io.cpp
)
target_link_libraries(spark_provider PUBLIC spark_foundation)
if(WIN32 AND NOT SPARK_WINDOWS_XP)
    target_sources(spark_provider PRIVATE src/platform/http.cpp)
    target_link_libraries(spark_provider PUBLIC winhttp)
else()
    find_package(CURL 7.68 REQUIRED)
    if(SPARK_WINDOWS_XP AND CURL_VERSION_STRING VERSION_GREATER_EQUAL 8.19)
        message(FATAL_ERROR "XP requires a libcurl release with an XP baseline, at most 8.18.x.")
    endif()
    target_sources(spark_provider PRIVATE src/platform/http_curl.cpp)
    target_link_libraries(spark_provider PUBLIC CURL::libcurl)
    if(SPARK_WINDOWS_XP)
        target_sources(spark_provider PRIVATE src/platform/xp_tls.cpp)
        target_compile_definitions(spark_provider PRIVATE MBEDTLS_USER_CONFIG_FILE="spark_xp_tls_config.h")
        target_link_libraries(spark_provider PUBLIC advapi32)
    endif()
endif()
if(SPARK_BUILD_TESTS)
    add_executable(spark_http_tests tests/http_tests.cpp)
    target_link_libraries(spark_http_tests PRIVATE spark_provider)
    if(WIN32)
        target_link_libraries(spark_http_tests PRIVATE ws2_32)
    endif()
    add_test(NAME spark_http_tests COMMAND spark_http_tests)
    set_tests_properties(spark_http_tests PROPERTIES TIMEOUT 25)
    add_executable(spark_binary_io_tests tests/binary_io_tests.cpp)
    target_link_libraries(spark_binary_io_tests PRIVATE spark_provider)
    add_test(NAME spark_binary_io_tests COMMAND spark_binary_io_tests)
    set_tests_properties(spark_binary_io_tests PROPERTIES TIMEOUT 10)
    add_executable(spark_runtime_provider_tests tests/runtime_provider_tests.cpp)
    target_link_libraries(spark_runtime_provider_tests PRIVATE spark_provider)
    if(WIN32)
        target_link_libraries(spark_runtime_provider_tests PRIVATE ws2_32)
    endif()
    add_test(NAME spark_runtime_provider_tests COMMAND spark_runtime_provider_tests)
    set_tests_properties(spark_runtime_provider_tests PROPERTIES TIMEOUT 15)
    if(NOT WIN32)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        find_program(SPARK_OPENSSL_EXECUTABLE openssl)
        if(Python3_Interpreter_FOUND AND SPARK_OPENSSL_EXECUTABLE)
            add_test(NAME spark_tls_rejection_tests
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_rejection_test.py
                    $<TARGET_FILE:spark_runtime_provider_tests> ${SPARK_OPENSSL_EXECUTABLE})
            set_tests_properties(spark_tls_rejection_tests PROPERTIES TIMEOUT 25)
        endif()
    endif()
endif()
if(SPARK_RUNTIME_TESTS_ONLY)
    return()
endif()
add_library(spark_core STATIC
    src/core/live_turn.cpp
    src/core/compaction_runner.cpp
    src/core/external_agents.cpp
    src/core/session_exchange.cpp
    src/core/tools.cpp
    src/core/tool_scan.cpp
    src/core/tool_process.cpp
    src/core/mcp.cpp
    src/core/subagents.cpp
    src/core/agent.cpp
)

target_include_directories(spark_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/md4c
)

if(MSVC)
    target_compile_options(spark_core PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/utf-8 /Zc:__cplusplus /EHsc>)
    target_compile_options(spark_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /wd4100 /wd4127>)
endif()

target_link_libraries(spark_core PUBLIC spark_provider)
if(WIN32)
    target_sources(spark_core PRIVATE src/platform/window_text.cpp src/platform/child_process_windows.cpp)
    target_link_libraries(spark_core PUBLIC comctl32 shell32 ole32 advapi32 user32)
else()
    target_sources(spark_core PRIVATE src/platform/child_process_posix.cpp)
endif()

add_library(spark_transcript STATIC src/gui/transcript.cpp src/gui/session_loader.cpp
    src/gui/native/history_loader.cpp)
target_link_libraries(spark_transcript PUBLIC spark_core)
add_library(spark_native_runtime STATIC src/gui/native/runtime.cpp)
target_link_libraries(spark_native_runtime PUBLIC spark_transcript)
if(SPARK_BUILD_TESTS)
    add_executable(spark_native_runtime_tests tests/native_runtime_tests.cpp)
    target_link_libraries(spark_native_runtime_tests PRIVATE spark_native_runtime)
    if(WIN32)
        target_link_libraries(spark_native_runtime_tests PRIVATE ws2_32)
    endif()
    add_test(NAME spark_native_runtime_tests COMMAND spark_native_runtime_tests)
    set_tests_properties(spark_native_runtime_tests PROPERTIES TIMEOUT 15)
    add_executable(spark_transcript_tests tests/transcript_tests.cpp)
    target_link_libraries(spark_transcript_tests PRIVATE spark_transcript)
    add_test(NAME spark_transcript_tests COMMAND spark_transcript_tests)
    set_tests_properties(spark_transcript_tests PROPERTIES TIMEOUT 20)
    add_executable(spark_process_tests tests/process_tests.cpp)
    target_link_libraries(spark_process_tests PRIVATE spark_core)
    add_test(NAME spark_process_tests COMMAND spark_process_tests $<TARGET_FILE:spark_fake_mcp_server>)
    set_tests_properties(spark_process_tests PROPERTIES TIMEOUT 25)
endif()

if(NOT SPARK_CORE_ONLY)
if(WIN32)
add_library(spark_gui STATIC
    src/gui/localization.cpp
    src/gui/app_shell.cpp
    src/gui/chat_view.cpp
    src/gui/settings_window.cpp
    src/gui/settings_providers.cpp
    src/gui/settings_provider_actions.cpp
    src/gui/provider_editor.cpp
    src/gui/provider_editor_config.cpp
    src/gui/app_session_load.cpp
    src/gui/summary_view.cpp
    src/gui/app_summary.cpp
    src/gui/add_menu.cpp
    src/gui/model_picker.cpp
    src/gui/app_composer.cpp
    src/gui/attachment_picker.cpp
    src/platform/smart_snapshot.cpp
    src/platform/snapshot_capture.cpp
)
target_link_libraries(spark_gui PUBLIC spark_transcript imm32 gdiplus uuid)
target_include_directories(spark_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_sources(spark_gui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/resources/localization.rc)
set_property(SOURCE resources/localization.rc APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/locales/shell.json
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/locales/settings.json
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/locales/chat.json
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/locales/auxiliary.json
)

add_executable(spark_agent WIN32
    src/main_win32.cpp
    resources/spark_agent.rc
)

target_link_libraries(spark_agent PRIVATE spark_gui dbghelp)
set_target_properties(spark_agent PROPERTIES OUTPUT_NAME "Spark")
if(MSVC)
    target_link_options(spark_agent PRIVATE "/SUBSYSTEM:WINDOWS,${SPARK_SUBSYSTEM_VERSION}")
endif()
else()
    find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
    foreach(locale SHELL SETTINGS CHAT AUXILIARY)
        string(TOLOWER "${locale}" filename)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/resources/locales/${filename}.json" SPARK_LOCALE_${locale})
    endforeach()
    configure_file(cmake/locale_resources.h.in generated/spark_locale_resources.h @ONLY)
    add_library(spark_gui STATIC
        src/gui/localization.cpp
        src/gui/native/chat_surface.cpp
        src/gui/native/chat_markdown.cpp
        src/gui/native/chat_usage.cpp
        src/gui/native/controls.cpp
        src/gui/native/app.cpp
        src/gui/native/sidebar.cpp
        src/gui/native/sidebar_layout.cpp
        src/gui/native/shell_layout.cpp
        src/gui/native/sidebar_list.cpp
        src/gui/native/sidebar_rail.cpp
        src/gui/native/project_actions.cpp
        src/gui/native/project_dialog.cpp
        src/gui/native/popup_menu.cpp
        src/gui/native/add_menu.cpp
        src/gui/native/model_picker.cpp
        src/gui/native/text_prompt.cpp
        src/gui/native/approval_prompt.cpp
        src/gui/native/conversation.cpp
        src/gui/native/attachments.cpp
        src/gui/native/attachment_picker.cpp
        src/gui/native/attachment_strip.cpp
        src/gui/native/composer_layout.cpp
        src/gui/native/summary_panel.cpp
        src/gui/native/summary_popup.cpp
        src/gui/native/summary_integration.cpp
        src/gui/native/thumbnail_cache.cpp
        src/gui/native/settings.cpp
        src/gui/native/archive_list.cpp
        src/gui/native/provider_list.cpp
        src/gui/native/reasoning_popup.cpp
        src/gui/native/provider_editor.cpp)
    target_include_directories(spark_gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(spark_gui PUBLIC spark_native_runtime ${wxWidgets_LIBRARIES})
    if(APPLE)
        enable_language(OBJCXX)
        set_target_properties(spark_gui PROPERTIES OBJCXX_STANDARD 17
            OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
        target_sources(spark_gui PRIVATE src/platform/desktop_capture_macos.mm
            src/gui/native/attachment_picker_macos.mm)
        target_link_libraries(spark_gui PUBLIC "-framework AppKit" "-framework CoreFoundation"
            "-framework CoreGraphics" "-framework ImageIO")
        add_executable(spark_agent MACOSX_BUNDLE src/main_native.cpp)
        target_sources(spark_agent PRIVATE resources/spark_agent.icns)
        set_source_files_properties(resources/spark_agent.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(spark_agent PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "Spark"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.spark.agent"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    else()
        find_package(X11 QUIET)
        target_sources(spark_gui PRIVATE src/platform/desktop_capture_x11.cpp)
        if(X11_FOUND AND X11_Xcomposite_FOUND)
            target_compile_definitions(spark_gui PRIVATE SPARK_HAVE_XCOMPOSITE)
            target_include_directories(spark_gui PRIVATE ${X11_INCLUDE_DIR})
            target_link_libraries(spark_gui PUBLIC ${X11_LIBRARIES} ${X11_Xcomposite_LIB})
        endif()
        add_executable(spark_agent src/main_native.cpp)
    endif()
    target_link_libraries(spark_agent PRIVATE spark_gui)
    set_target_properties(spark_agent PROPERTIES OUTPUT_NAME "Spark")
    if(SPARK_BUILD_TESTS)
        add_executable(spark_native_gui_tests tests/native_gui_tests.cpp)
        target_link_libraries(spark_native_gui_tests PRIVATE spark_gui)
        add_executable(spark_native_provider_editor_tests tests/native_provider_editor_tests.cpp)
        target_link_libraries(spark_native_provider_editor_tests PRIVATE spark_gui)
        find_program(SPARK_XVFB_RUN xvfb-run)
        if(SPARK_XVFB_RUN AND NOT APPLE)
            add_test(NAME spark_native_gui_tests COMMAND ${SPARK_XVFB_RUN} -a -s "-screen 0 1920x1080x24"
                $<TARGET_FILE:spark_native_gui_tests> "${CMAKE_CURRENT_BINARY_DIR}/native-screenshots")
            set_tests_properties(spark_native_gui_tests PROPERTIES TIMEOUT 90)
            add_test(NAME spark_native_provider_editor_tests COMMAND ${SPARK_XVFB_RUN} -a
                -s "-screen 0 1920x1080x24" $<TARGET_FILE:spark_native_provider_editor_tests>)
            set_tests_properties(spark_native_provider_editor_tests PROPERTIES TIMEOUT 20)
            add_test(NAME spark_native_summary_tests COMMAND ${SPARK_XVFB_RUN} -a
                -s "-screen 0 1920x1080x24" $<TARGET_FILE:spark_native_gui_tests> --summary)
        elseif(APPLE)
            add_test(NAME spark_native_summary_tests COMMAND spark_native_gui_tests --summary)
        endif()
        if(TEST spark_native_summary_tests)
            set_tests_properties(spark_native_summary_tests PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()
endif()

add_executable(spark_agent_mcp
    src/mcp_server_main.cpp
)

target_link_libraries(spark_agent_mcp PRIVATE spark_core)
set_target_properties(spark_agent_mcp PROPERTIES OUTPUT_NAME "SparkMCP")
if(MSVC)
    target_link_options(spark_agent_mcp PRIVATE "/SUBSYSTEM:CONSOLE,${SPARK_SUBSYSTEM_VERSION}")
endif()

if(WIN32 AND NOT SPARK_WINDOWS_XP AND SPARK_WIN7_COMPAT_PROBE AND NOT SPARK_CORE_ONLY)
    add_executable(spark_win7_probe WIN32
        src/win7_probe.cpp
        resources/spark_probe.rc
    )
    target_link_libraries(spark_win7_probe PRIVATE spark_core)
    set_target_properties(spark_win7_probe PROPERTIES OUTPUT_NAME "SparkWin7Probe")
    if(MSVC)
        target_link_options(spark_win7_probe PRIVATE "/SUBSYSTEM:WINDOWS,6.01")
    endif()
endif()

if(SPARK_BUILD_TESTS)
    enable_testing()
    add_executable(spark_fake_mcp_server tests/fake_mcp_server.cpp)
    target_link_libraries(spark_fake_mcp_server PRIVATE spark_core)
if(WIN32 AND NOT SPARK_CORE_ONLY)
    add_executable(spark_core_tests tests/core_tests.cpp)
    target_link_libraries(spark_core_tests PRIVATE spark_core)
    add_test(NAME spark_core_tests COMMAND spark_core_tests)

    add_executable(spark_config_defaults_tests tests/config_defaults_tests.cpp)
    target_link_libraries(spark_config_defaults_tests PRIVATE spark_core)
    target_compile_definitions(spark_config_defaults_tests PRIVATE SPARK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME spark_config_defaults_tests COMMAND spark_config_defaults_tests)

    add_executable(spark_agent_tests tests/agent_tests.cpp)
    target_link_libraries(spark_agent_tests PRIVATE spark_core ws2_32)
    add_test(NAME spark_agent_tests COMMAND spark_agent_tests $<TARGET_FILE:spark_fake_mcp_server>)

    add_executable(spark_exit_tests tests/exit_tests.cpp)
    target_link_libraries(spark_exit_tests PRIVATE spark_gui)
    add_test(NAME spark_exit_tests COMMAND spark_exit_tests)
    set_tests_properties(spark_exit_tests PROPERTIES TIMEOUT 10)

    add_executable(spark_composer_tests tests/composer_tests.cpp)
    target_link_libraries(spark_composer_tests PRIVATE spark_gui)
    add_test(NAME spark_composer_tests COMMAND spark_composer_tests)
    set_tests_properties(spark_composer_tests PROPERTIES TIMEOUT 10)

    add_executable(spark_snapshot_tests tests/snapshot_tests.cpp)
    target_link_libraries(spark_snapshot_tests PRIVATE spark_gui)
    add_test(NAME spark_snapshot_tests COMMAND spark_snapshot_tests)
    set_tests_properties(spark_snapshot_tests PROPERTIES TIMEOUT 10)

    add_executable(spark_session_loading_tests tests/session_loading_tests.cpp)
    target_link_libraries(spark_session_loading_tests PRIVATE spark_gui)
    add_test(NAME spark_session_loading_tests COMMAND spark_session_loading_tests)
    set_tests_properties(spark_session_loading_tests PROPERTIES TIMEOUT 15)

    add_executable(spark_localization_tests tests/localization_tests.cpp)
    target_link_libraries(spark_localization_tests PRIVATE spark_gui)
    target_compile_definitions(spark_localization_tests PRIVATE SPARK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME spark_localization_tests COMMAND spark_localization_tests)

    add_executable(spark_settings_provider_tests tests/settings_provider_tests.cpp)
    target_link_libraries(spark_settings_provider_tests PRIVATE spark_gui ws2_32)
    add_test(NAME spark_settings_provider_tests COMMAND spark_settings_provider_tests)
    set_tests_properties(spark_settings_provider_tests PROPERTIES TIMEOUT 10)

    add_executable(spark_settings_archive_tests tests/settings_archive_tests.cpp)
    target_link_libraries(spark_settings_archive_tests PRIVATE spark_gui)
    add_test(NAME spark_settings_archive_tests COMMAND spark_settings_archive_tests)
    set_tests_properties(spark_settings_archive_tests PROPERTIES TIMEOUT 15)

    add_executable(spark_settings_layout_tests tests/settings_layout_tests.cpp)
    target_link_libraries(spark_settings_layout_tests PRIVATE spark_gui)
    add_test(NAME spark_settings_layout_tests COMMAND spark_settings_layout_tests)
    set_tests_properties(spark_settings_layout_tests PROPERTIES TIMEOUT 10)

    add_executable(spark_provider_models_tests tests/provider_models_tests.cpp)
    target_link_libraries(spark_provider_models_tests PRIVATE spark_core ws2_32)
    add_test(NAME spark_provider_models_tests COMMAND spark_provider_models_tests)
    set_tests_properties(spark_provider_models_tests PROPERTIES TIMEOUT 15)

    add_executable(spark_provider_probe_tests tests/provider_probe_tests.cpp)
    target_link_libraries(spark_provider_probe_tests PRIVATE spark_core ws2_32)
    add_test(NAME spark_provider_probe_tests COMMAND spark_provider_probe_tests)
    set_tests_properties(spark_provider_probe_tests PROPERTIES TIMEOUT 30)
endif()
endif()

if(WIN32)
    set(SPARK_INSTALL_BIN ".")
    set(SPARK_INSTALL_DATA ".")
else()
    set(SPARK_INSTALL_BIN "bin")
    set(SPARK_INSTALL_DATA "share/spark")
endif()
if(NOT SPARK_CORE_ONLY)
    install(TARGETS spark_agent RUNTIME DESTINATION "${SPARK_INSTALL_BIN}" BUNDLE DESTINATION ".")
    if(APPLE)
        # The MCP executable lives next to the GUI executable in the application bundle.
        set(SPARK_INSTALL_BIN "Spark.app/Contents/MacOS")
        set(SPARK_INSTALL_DATA "Spark.app/Contents/Resources")
    endif()
endif()
install(TARGETS spark_agent_mcp RUNTIME DESTINATION "${SPARK_INSTALL_BIN}")
install(FILES config.example.json README.md README.zh-CN.md CONFIGURATION.md THIRD_PARTY_NOTICES.md
    resources/spark_agent.ico DESTINATION "${SPARK_INSTALL_DATA}")
install(FILES third_party/nlohmann/LICENSE.MIT DESTINATION "${SPARK_INSTALL_DATA}" RENAME THIRD_PARTY_NLOHMANN_JSON_LICENSE.txt)
install(FILES third_party/md4c/LICENSE.md DESTINATION "${SPARK_INSTALL_DATA}" RENAME THIRD_PARTY_MD4C_LICENSE.txt)
if(SPARK_WINDOWS_XP)
    install(FILES third_party/ghc/LICENSE DESTINATION "${SPARK_INSTALL_DATA}" RENAME THIRD_PARTY_GHC_FILESYSTEM_LICENSE.txt)
endif()
if(UNIX AND NOT APPLE AND NOT SPARK_CORE_ONLY)
    install(FILES resources/spark.desktop DESTINATION share/applications)
    install(FILES resources/spark_agent.png DESTINATION share/icons/hicolor/1024x1024/apps RENAME spark.png)
endif()
# wxWidgets is built statically for the macOS package and the remaining
# dependencies are system frameworks, so BundleUtilities has no non-system
# libraries to repair here. The package script signs the finished app.
