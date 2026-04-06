cmake_minimum_required(VERSION 3.16)
project(ScopeOne VERSION 1.3.406 LANGUAGES CXX)

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
        "C:/Qt/6.11.0/msvc2022_64"
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
        RUNTIME DESTINATION .
)

add_custom_command(TARGET ScopeOne POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "$<TARGET_FILE_DIR:scopeone::ScopeOneCore>"
        "$<TARGET_FILE_DIR:ScopeOne>"
)

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



if (WIN32)
    install(CODE "
        file(GLOB _core_runtime_files
            \"$<TARGET_FILE_DIR:scopeone::ScopeOneCore>/*.dll\"
            \"$<TARGET_FILE_DIR:scopeone::ScopeOneCore>/ScopeOne_Agent.exe\"
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
            networkinformation
            styles
            tls
            generic
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
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_FILE_NAME "ScopeOne-${PROJECT_VERSION}-win-x64")

include(CPack)
