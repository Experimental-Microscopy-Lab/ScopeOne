cmake_minimum_required(VERSION 3.16)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" SCOPEONE_VERSION LIMIT_COUNT 1)
project(ScopeOne VERSION "${SCOPEONE_VERSION}" LANGUAGES CXX)

set(SCOPEONE_GIT_COMMIT "unknown")
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short=7 HEAD
            RESULT_VARIABLE GIT_COMMIT_RESULT
            OUTPUT_VARIABLE GIT_COMMIT_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    if (GIT_COMMIT_RESULT EQUAL 0 AND NOT GIT_COMMIT_OUTPUT STREQUAL "")
        set(SCOPEONE_GIT_COMMIT "${GIT_COMMIT_OUTPUT}")
    endif ()
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)

set(ScopeOneCore_ROOT
        "${CMAKE_SOURCE_DIR}/ScopeOneCore/install"
        CACHE PATH "ScopeOneCore install prefix"
)
if (WIN32)
    list(PREPEND CMAKE_PREFIX_PATH "C:/Qt/6.11.0/msvc2022_64")
endif ()
list(PREPEND CMAKE_PREFIX_PATH "${ScopeOneCore_ROOT}")

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
        src/ImageGalleryWidget.cpp
        src/ImageWorkspace.cpp
        src/ImageToolsDialog.cpp
        src/PreviewWidget.cpp
        src/ConsoleWidget.cpp
        src/AboutDialog.cpp
        src/SettingsDialog.cpp
        src/DeviceControlWidget.cpp
        src/RecordingWidget.cpp
        src/DevicePropertyWidget.cpp
        src/ConfigPresetWidget.cpp
        src/ScopeOneLocalApiServer.cpp
        src/ScopeOneToolPlugin.cpp
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

set(SCOPEONE_UI_HEADERS
        src/InspectWidget.h
        src/MainWindow.h
        src/ImageProcessingWidget.h
        src/ImageGalleryWidget.h
        src/ImageWorkspace.h
        src/ImageToolsDialog.h
        src/PreviewWidget.h
        src/ConsoleWidget.h
        src/AboutDialog.h
        src/SettingsDialog.h
        src/DeviceControlWidget.h
        src/RecordingWidget.h
        src/DevicePropertyWidget.h
        src/ConfigPresetWidget.h
        src/ScopeOneLocalApiServer.h
        src/ScopeOneToolPlugin.h
)

if (WIN32)
    list(APPEND SCOPEONE_UI_SOURCES ${CMAKE_SOURCE_DIR}/resources/app_icon.rc)
endif ()

qt_add_executable(ScopeOne
        WIN32
        ${SCOPEONE_UI_SOURCES}
        ${SCOPEONE_UI_HEADERS}
)
qt_add_executable(ScopeOneMcpServer
        src/ScopeOneMcpServer.cpp
)
target_link_libraries(ScopeOne PRIVATE
        scopeone::ScopeOneCore
        Qt::Widgets
        Qt::OpenGL
        Qt::OpenGLWidgets
        Qt::Network
        Qt::Concurrent
)
target_compile_definitions(ScopeOne PRIVATE
        SCOPEONE_GIT_COMMIT="${SCOPEONE_GIT_COMMIT}"
)
target_link_libraries(ScopeOneMcpServer PRIVATE
        Qt::Core
        Qt::Network
)
target_include_directories(ScopeOne PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/include
)
target_include_directories(ScopeOneMcpServer PRIVATE
        ${CMAKE_BINARY_DIR}/include
)
target_precompile_headers(ScopeOne PRIVATE
        <QtGlobal>
        <QWidget>
        <QDialog>
        <QString>
        <QStringList>
        <QVector>
        <QList>
        <QHash>
        <QSet>
        <QDateTime>
        <QTimer>
        <QDebug>
        <QSignalBlocker>
        <QVBoxLayout>
        <QHBoxLayout>
        <QGridLayout>
        <QFormLayout>
        <QPushButton>
        <QLabel>
        <QCheckBox>
        <QComboBox>
        <QSpinBox>
        <QDoubleSpinBox>
        <QLineEdit>
        <algorithm>
        <limits>
        <memory>
        <utility>
        <vector>
)

install(TARGETS ScopeOne ScopeOneMcpServer
        RUNTIME DESTINATION .
)
install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/config/MMConfig_demo.cfg"
        "${CMAKE_CURRENT_SOURCE_DIR}/config/MMConfig_demo_dual.cfg"
        DESTINATION .
)

if (WIN32)
    file(GLOB SCOPEONE_CORE_RUNTIME_FILES
            LIST_DIRECTORIES false
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_BINDIR}/*.dll"
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_BINDIR}/ScopeOne_DriverHost.exe"
    )
else ()
    file(GLOB SCOPEONE_CORE_RUNTIME_FILES
            LIST_DIRECTORIES false
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_BINDIR}/ScopeOne_DriverHost"
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_LIBDIR}/libScopeOneCore.so*"
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_LIBDIR}/libScopeWriter.so*"
    )
endif ()

if (SCOPEONE_CORE_RUNTIME_FILES)
    add_custom_command(TARGET ScopeOne POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SCOPEONE_CORE_RUNTIME_FILES}
            "$<TARGET_FILE_DIR:ScopeOne>"
            COMMAND_EXPAND_LISTS
    )
endif ()

set(SCOPEONE_PROVIDER_DIR "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_BINDIR}/plugins/hardware")
if (EXISTS "${SCOPEONE_PROVIDER_DIR}")
    add_custom_command(TARGET ScopeOne POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SCOPEONE_PROVIDER_DIR}"
            "$<TARGET_FILE_DIR:ScopeOne>/plugins/hardware"
    )
    install(DIRECTORY "${SCOPEONE_PROVIDER_DIR}/" DESTINATION plugins/hardware)
endif ()

if (WIN32)
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

# # Fast Release build
# if (MSVC)
#     target_compile_options(ScopeOne PRIVATE
#             $<$<CONFIG:Release>:/MP>
#     )
# endif ()

# LTO Release build
if (MSVC)
    target_compile_options(ScopeOne PRIVATE
            $<$<CONFIG:Release>:/GL /fp:fast /arch:AVX2>
    )
    target_link_options(ScopeOne PRIVATE
            $<$<CONFIG:Release>:/LTCG>
    )
endif ()
if (WIN32)
    install(CODE "
        file(GLOB _core_runtime_files
            \"$<TARGET_FILE_DIR:scopeone::ScopeOneCore>/*.dll\"
            \"$<TARGET_FILE_DIR:scopeone::ScopeOneCore>/ScopeOne_DriverHost.exe\"
        )
        if (_core_runtime_files)
            file(INSTALL
                DESTINATION \"\${CMAKE_INSTALL_PREFIX}\"
                TYPE FILE
                FILES \${_core_runtime_files}
            )
        endif()
    ")

    install(CODE "
        set(_runtime_dir \"${CMAKE_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}\")
        if (NOT EXISTS \"\${_runtime_dir}/$<TARGET_FILE_NAME:ScopeOne>\")
            return()
        endif()

        foreach(_pattern IN ITEMS
            Qt6*.dll
            opengl32sw.dll
            D3Dcompiler_47.dll
            dxcompiler.dll
            dxil.dll
        )
            file(GLOB _qt_files \"\${_runtime_dir}/\${_pattern}\")
            if (_qt_files)
                file(INSTALL
                    DESTINATION \"\${CMAKE_INSTALL_PREFIX}\"
                    TYPE FILE
                    FILES \${_qt_files}
                )
            endif()
        endforeach()

        foreach(_subdir IN ITEMS
            platforms
            imageformats
            iconengines
            styles
        )
            if (EXISTS \"\${_runtime_dir}/\${_subdir}\")
                file(INSTALL
                    DESTINATION \"\${CMAKE_INSTALL_PREFIX}\"
                    TYPE DIRECTORY
                    FILES \"\${_runtime_dir}/\${_subdir}\"
                )
            endif()
        endforeach()
    ")
endif ()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
include(InstallRequiredSystemLibraries)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_VENDOR "ScopeOne Project")
set(CPACK_PACKAGE_CONTACT "https://github.com/Experimental-Microscopy-Lab/ScopeOne/issues")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Experimental-Microscopy-Lab/ScopeOne")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_FILE_NAME "ScopeOne-${PROJECT_VERSION}-win-x64")

if (WIN32)
    find_program(SCOPEONE_MAKENSIS_EXECUTABLE
            NAMES makensis.exe makensis
            HINTS
            "C:/Program Files/NSIS"
            "C:/Program Files (x86)/NSIS"
    )
    if (SCOPEONE_MAKENSIS_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "NSIS")
        set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
        set(CPACK_NSIS_PACKAGE_NAME "ScopeOne")
        set(CPACK_NSIS_DISPLAY_NAME "ScopeOne")
        set(CPACK_NSIS_BRANDING_TEXT "ScopeOne")
        set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
        set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")
        set(CPACK_PACKAGE_INSTALL_DIRECTORY "ScopeOne")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        set(CPACK_NSIS_UNINSTALL_NAME "Uninstall ScopeOne")
        set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
        set(CPACK_NSIS_INSTALLED_ICON_NAME "ScopeOne.exe")
        set(CPACK_PACKAGE_EXECUTABLES "ScopeOne" "ScopeOne")
        set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/resources/icon.ico")
        set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/resources/icon.ico")
        set(CPACK_NSIS_MUI_FINISHPAGE_RUN "ScopeOne.exe")
    else ()
        message(WARNING "NSIS was not found; CPack will produce only the portable ZIP package.")
    endif ()
endif ()

include(CPack)
