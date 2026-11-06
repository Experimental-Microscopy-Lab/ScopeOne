cmake_minimum_required(VERSION 3.23)
include("${CMAKE_CURRENT_LIST_DIR}/ScopeOneCore/cmake/ScopeOneVersion.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ScopeOneCore/cmake/ScopeOneQt.cmake")
scopeone_resolve_version(SCOPEONE_VERSION "${CMAKE_CURRENT_LIST_DIR}")
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
list(PREPEND CMAKE_PREFIX_PATH "${ScopeOneCore_ROOT}")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include")
configure_file(
        "${CMAKE_SOURCE_DIR}/src/AppVersion.h.in"
        "${CMAKE_BINARY_DIR}/include/AppVersion.h"
        @ONLY
)

find_package(Qt6 ${SCOPEONE_QT_MINIMUM_VERSION} COMPONENTS
        Core
        Gui
        Widgets
        OpenGL
        OpenGLWidgets
        Network
        Concurrent
        REQUIRED
)
message(STATUS "Using Qt ${Qt6_VERSION} from ${Qt6_DIR}")

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
        src/ConfigPresetWidget.cpp
        src/DeviceControlWidget.cpp
        src/DevicePropertyWidget.cpp
        src/PositionListFile.cpp
        src/RecordingWidget.cpp
        src/DriversDialog.cpp
        src/ScopeOneLocalApiServer.cpp
        src/ToolRegistry.cpp
        src/AboutDialog.cpp
        src/Appearance.cpp
        src/CollapsibleSection.cpp
        src/CommandSearchBox.cpp
        src/ConsoleWidget.cpp
        src/EmptyHint.cpp
        src/MainWindow.cpp
        src/PtyProcess.cpp
        src/SettingsDialog.cpp
        src/TerminalWidget.cpp
        src/ToolIcons.cpp
        src/UpdateChecker.cpp
        src/WelcomeOverlay.cpp
        src/ImageAnalysis.cpp
        src/ImageGalleryWidget.cpp
        src/ImageProcessingWidget.cpp
        src/ImageWorkspace.cpp
        src/InspectWidget.cpp
        src/OrthogonalViewsDialog.cpp
        src/ParameterDialog.cpp
        src/PreviewWidget.cpp
        src/ProfilePlotWindow.cpp
        src/ResultsWindow.cpp
        src/RoiManagerWindow.cpp
        ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

# Every tool in src/tools registers itself, so a new tool needs no change here
file(GLOB_RECURSE SCOPEONE_TOOL_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/tools/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/tools/*.h"
)
list(APPEND SCOPEONE_UI_SOURCES ${SCOPEONE_TOOL_FILES})

set(SCOPEONE_UI_HEADERS
        src/ConfigPresetWidget.h
        src/DeviceControlWidget.h
        src/DevicePropertyWidget.h
        src/PositionListFile.h
        src/RecordingWidget.h
        src/DriversDialog.h
        src/ScopeOneLocalApiServer.h
        src/ToolRegistry.h
        src/AboutDialog.h
        src/Appearance.h
        src/CollapsibleSection.h
        src/CommandSearchBox.h
        src/ConsoleWidget.h
        src/EmptyHint.h
        src/MainWindow.h
        src/PtyProcess.h
        src/SettingsDialog.h
        src/TerminalWidget.h
        src/ToolIcons.h
        src/UpdateChecker.h
        src/WelcomeOverlay.h
        src/ImageAnalysis.h
        src/ImageGalleryWidget.h
        src/ImageProcessingWidget.h
        src/ImageWorkspace.h
        src/InspectWidget.h
        src/OrthogonalViewsDialog.h
        src/ParameterDialog.h
        src/PreviewWidget.h
        src/ProfilePlotWindow.h
        src/ResultsWindow.h
        src/RoiManagerWindow.h
        src/StackAxes.h
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
if (UNIX AND NOT APPLE)
    set_target_properties(ScopeOne PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN/${CMAKE_INSTALL_LIBDIR}"
    )
    # forkpty for the console terminal
    target_link_libraries(ScopeOne PRIVATE util)
endif ()
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
    # The lib folder of the Core install holds only Core's libraries and ONNX Runtime, and the
    # copied DriverHost resolves them beside itself. Named one by one, the list had missed the
    # macOS dylibs.
    file(GLOB SCOPEONE_CORE_LIBRARIES
            LIST_DIRECTORIES false
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_LIBDIR}/*.so*"
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_LIBDIR}/*.dylib"
    )
    set(SCOPEONE_CORE_RUNTIME_FILES
            "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_BINDIR}/ScopeOne_DriverHost"
            ${SCOPEONE_CORE_LIBRARIES}
    )
endif ()

if (SCOPEONE_CORE_RUNTIME_FILES)
    # A target of its own that runs on every build. As a post-build step of ScopeOne it ran only
    # when the application itself was relinked, so a rebuilt core library was left behind next
    # to an older copy that the application kept loading.
    if (WIN32)
        add_custom_target(ScopeOneCoreRuntime ALL
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SCOPEONE_CORE_RUNTIME_FILES}
                "$<TARGET_FILE_DIR:ScopeOne>"
                COMMAND_EXPAND_LISTS
        )
    else ()
        # A library comes as a file and its links, libonnxruntime.so, .so.1 and .so.1.30.0, and
        # copying them as files put three full copies of ONNX Runtime beside ScopeOne
        string(REPLACE ";" "|" _core_runtime_files "${SCOPEONE_CORE_RUNTIME_FILES}")
        add_custom_target(ScopeOneCoreRuntime ALL
                COMMAND ${CMAKE_COMMAND}
                "-DFILES=${_core_runtime_files}"
                "-DDEST=$<TARGET_FILE_DIR:ScopeOne>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/CopyRuntimeFiles.cmake"
                VERBATIM
        )
    endif ()
    add_dependencies(ScopeOneCoreRuntime ScopeOne)
endif ()

if (WIN32)
    install(FILES ${SCOPEONE_CORE_RUNTIME_FILES} DESTINATION .)
elseif (NOT APPLE)
    install(FILES ${SCOPEONE_CORE_LIBRARIES} DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif ()

set(_driver_dir "${ScopeOneCore_ROOT}/${CMAKE_INSTALL_BINDIR}/drivers")
if (EXISTS "${_driver_dir}")
    # Copied on every build. As a step after linking ScopeOne it ran only when ScopeOne itself was
    # relinked, so rebuilt drivers stayed old beside the application and crashed a newer DriverHost.
    add_custom_target(ScopeOneDrivers ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_driver_dir}"
            "$<TARGET_FILE_DIR:ScopeOne>/drivers"
    )
    add_dependencies(ScopeOneDrivers ScopeOne)
    install(DIRECTORY "${_driver_dir}/" DESTINATION "drivers")
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

# /MP compiles the sources of one project in parallel, which the Visual Studio generator does not,
# and Release builds use LTO
if (MSVC)
    target_compile_options(ScopeOne PRIVATE /MP $<$<CONFIG:Release>:/GL /fp:fast /arch:AVX2>)
    target_link_options(ScopeOne PRIVATE $<$<CONFIG:Release>:/LTCG>)
    target_compile_options(ScopeOneMcpServer PRIVATE /MP)
endif ()
if (WIN32)
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
# A Core built with CUDA makes the GPU package
if (ScopeOneCore_WITH_CUDA)
    set(CPACK_PACKAGE_FILE_NAME "ScopeOne-${PROJECT_VERSION}-win-x64-gpu")
else ()
    set(CPACK_PACKAGE_FILE_NAME "ScopeOne-${PROJECT_VERSION}-win-x64")
endif ()

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
