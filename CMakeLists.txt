cmake_minimum_required(VERSION 3.16)
project(ScopeOne VERSION 1.3.30 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(GNUInstallDirs)

set(ScopeOneCore_ROOT
        "${CMAKE_SOURCE_DIR}/ScopeOneCore/install"
        CACHE PATH "ScopeOneCore install prefix"
)
list(PREPEND CMAKE_PREFIX_PATH
        "C:/Qt/6.9.1/msvc2022_64"
        "${ScopeOneCore_ROOT}"
)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include")
configure_file(
        "${CMAKE_SOURCE_DIR}/src/AppVersion.h.in"
        "${CMAKE_BINARY_DIR}/include/AppVersion.h"
        @ONLY
)

find_package(Qt6 COMPONENTS
        Core
        Gui
        Widgets
        OpenGL
        OpenGLWidgets
        Network
        Concurrent
        REQUIRED
)

find_package(ScopeOneCore CONFIG REQUIRED)

if (WIN32 AND TARGET Qt6::qmake)
    get_target_property(qmake_executable Qt6::qmake IMPORTED_LOCATION)
    if (qmake_executable)
        get_filename_component(qt_bin_dir "${qmake_executable}" DIRECTORY)
        set(SCOPEONE_QT_BIN_DIR "${qt_bin_dir}")
        find_program(WINDEPLOYQT_EXECUTABLE
                NAMES windeployqt
                HINTS "${qt_bin_dir}"
        )
    endif ()
endif ()

set(SCOPEONE_UI_SOURCES
        src/main.cpp
        src/InspectWidget.cpp
        src/MainWindow.cpp
        src/ImageProcessingWidget.cpp
        src/PreviewWidget.cpp
        src/ConsoleWidget.cpp
        src/AboutDialog.cpp
        src/SettingsDialog.cpp
        src/DeviceControlWidget.cpp
        src/RecordingWidget.cpp
        src/ImageSessionDialog.cpp
        src/DevicePropertyWidget.cpp
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

set(SCOPEONE_UI_HEADERS
        src/InspectWidget.h
        src/MainWindow.h
        src/ImageProcessingWidget.h
        src/PreviewWidget.h
        src/ConsoleWidget.h
        src/AboutDialog.h
        src/SettingsDialog.h
        src/DeviceControlWidget.h
        src/RecordingWidget.h
        src/ImageSessionDialog.h
        src/DevicePropertyWidget.h
)

if (WIN32)
    list(APPEND SCOPEONE_UI_SOURCES ${CMAKE_SOURCE_DIR}/resources/app_icon.rc)
endif ()

add_executable(ScopeOne ${SCOPEONE_UI_SOURCES} ${SCOPEONE_UI_HEADERS})
target_link_libraries(ScopeOne PRIVATE
        scopeone::ScopeOneCore
        Qt::Widgets
        Qt::OpenGL
        Qt::OpenGLWidgets
        Qt::Concurrent
)
target_include_directories(ScopeOne PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/include
)

install(TARGETS ScopeOne
        RUNTIME_DEPENDENCIES
            DIRECTORIES "${ScopeOneCore_ROOT}/bin"
            PRE_EXCLUDE_REGEXES
                "api-ms-win-.*"
                "ext-ms-.*"
            POST_EXCLUDE_REGEXES
                ".*[\\\\/]Windows[\\\\/]System32[\\\\/].*"
        RUNTIME DESTINATION .
)

if (WIN32)
    add_custom_command(TARGET ScopeOne POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ScopeOneCore_ROOT}/bin"
            "$<TARGET_FILE_DIR:ScopeOne>"
    )

    if (WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET ScopeOne POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                --no-compiler-runtime
                --release
                "$<TARGET_FILE:ScopeOne>"
                WORKING_DIRECTORY "${SCOPEONE_QT_BIN_DIR}"
                VERBATIM
        )
    else ()
        message(WARNING "windeployqt was not found; ScopeOne may not start outside Qt Creator.")
    endif ()
endif ()

if (MSVC)
    target_compile_options(ScopeOne PRIVATE
            $<$<CONFIG:Release>:/O2 /GL /fp:fast /arch:AVX2>
    )
    target_compile_definitions(ScopeOne PRIVATE
            $<$<CONFIG:Release>:NDEBUG>
    )
    target_link_options(ScopeOne PRIVATE
            $<$<CONFIG:Release>:/LTCG>
    )
endif ()

set(SCOPEONE_PACKAGE_DOCS
        "${CMAKE_SOURCE_DIR}/README.md"
)
if (EXISTS "${CMAKE_SOURCE_DIR}/LICENSE.txt")
    list(APPEND SCOPEONE_PACKAGE_DOCS "${CMAKE_SOURCE_DIR}/LICENSE.txt")
endif ()
install(FILES ${SCOPEONE_PACKAGE_DOCS}
        DESTINATION .
)

if (WIN32)
    install(CODE "
        set(_ui_runtime_dir \"${CMAKE_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}\")

        file(GLOB _qt_dlls \"\${_ui_runtime_dir}/Qt6*.dll\")
        if (NOT _qt_dlls)
            message(FATAL_ERROR \"Missing Qt runtime files matching '\${_ui_runtime_dir}/Qt6*.dll'.\")
        endif()

        file(INSTALL
            DESTINATION \"\${CMAKE_INSTALL_PREFIX}\"
            TYPE FILE
            FILES
                \${_qt_dlls}
                \"\${_ui_runtime_dir}/opengl32sw.dll\"
                \"\${_ui_runtime_dir}/D3Dcompiler_47.dll\"
                \"\${_ui_runtime_dir}/dxcompiler.dll\"
                \"\${_ui_runtime_dir}/dxil.dll\"
        )

        file(INSTALL
            DESTINATION \"\${CMAKE_INSTALL_PREFIX}\"
            TYPE DIRECTORY
            FILES
                \"\${_ui_runtime_dir}/platforms\"
                \"\${_ui_runtime_dir}/imageformats\"
                \"\${_ui_runtime_dir}/iconengines\"
                \"\${_ui_runtime_dir}/networkinformation\"
                \"\${_ui_runtime_dir}/styles\"
                \"\${_ui_runtime_dir}/tls\"
                \"\${_ui_runtime_dir}/generic\"
        )
    ")
endif ()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
include(InstallRequiredSystemLibraries)

set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_FILE_NAME "ScopeOne-${PROJECT_VERSION}-win-x64")

include(CPack)
