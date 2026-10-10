# Resolves the ScopeOne version as MAJOR.MINOR.MMDD.
#
# The VERSION file holds MAJOR.MINOR. The patch is the month and day of the last commit,
# so every build of one commit has the same version and a new day gives a new version
# without editing any file. Outside a git checkout the patch is 0.
#
# Run it as a script to print the version: cmake -DSCOPEONE_ROOT=<repository> -P ScopeOneVersion.cmake

function(scopeone_resolve_version out_var repo_root)
    file(STRINGS "${repo_root}/VERSION" base LIMIT_COUNT 1)
    string(STRIP "${base}" base)
    if (NOT base MATCHES "^[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "VERSION must contain MAJOR.MINOR, found '${base}'")
    endif ()

    set(patch 0)
    find_package(Git QUIET)
    if (GIT_FOUND)
        execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${repo_root}" log -1 "--format=%cd" "--date=format:%m%d"
                RESULT_VARIABLE git_result
                OUTPUT_VARIABLE commit_day
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
        )
        if (git_result EQUAL 0 AND commit_day MATCHES "^[0-9][0-9][0-9][0-9]$")
            # A leading zero would read as octal
            string(REGEX REPLACE "^0+" "" patch "${commit_day}")
        endif ()
    endif ()

    if (NOT CMAKE_SCRIPT_MODE_FILE)
        # Configure again when the version file or the checked out commit changes
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${repo_root}/VERSION")
        if (EXISTS "${repo_root}/.git/logs/HEAD")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${repo_root}/.git/logs/HEAD")
        endif ()
    endif ()
    set(${out_var} "${base}.${patch}" PARENT_SCOPE)
endfunction()

if (CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if (NOT SCOPEONE_ROOT)
        message(FATAL_ERROR "Pass the repository with -DSCOPEONE_ROOT=<path>")
    endif ()
    scopeone_resolve_version(scopeone_version "${SCOPEONE_ROOT}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${scopeone_version}")
endif ()
