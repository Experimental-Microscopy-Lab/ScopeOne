# Selects the Qt of a ScopeOne project. Included before project(), because vcpkg installs the
# manifest there. SCOPEONE_QT_DIR names an installed Qt 6.8 or later; empty builds Qt with vcpkg.
set(SCOPEONE_QT_DIR "" CACHE PATH "Installed Qt 6.8 or later, such as C:/Qt/6.11.2/msvc2022_64; empty builds Qt with vcpkg")

if (SCOPEONE_QT_DIR)
    # The qt feature of vcpkg.json is the only default feature, so vcpkg leaves Qt out and CMake
    # can only find the installed one
    set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON)
    list(PREPEND CMAKE_PREFIX_PATH "${SCOPEONE_QT_DIR}")
endif ()

# The found Qt is cached, so a project switched between vcpkg's Qt and an installed one kept the
# Qt of its earlier configure
if (NOT "${SCOPEONE_QT_DIR}" STREQUAL "${SCOPEONE_QT_DIR_USED}")
    get_cmake_property(_scopeone_cache_variables CACHE_VARIABLES)
    foreach (_scopeone_variable IN LISTS _scopeone_cache_variables)
        if (_scopeone_variable MATCHES "^Qt6.*_DIR$")
            unset(${_scopeone_variable} CACHE)
        endif ()
    endforeach ()
endif ()
set(SCOPEONE_QT_DIR_USED "${SCOPEONE_QT_DIR}" CACHE INTERNAL "")

set(SCOPEONE_QT_MINIMUM_VERSION 6.8)
