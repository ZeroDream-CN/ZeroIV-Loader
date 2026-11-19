cmake_minimum_required(VERSION 3.17)

# VERSION is the single source of truth for the release number: the tag, the
# binary and the source archive all read it, so a build can always be traced
# back to the exact sources it came from.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ZEROIV_VERSION)
string(STRIP "${ZEROIV_VERSION}" ZEROIV_VERSION)

project(ZeroIV-Loader VERSION ${ZEROIV_VERSION} LANGUAGES CXX)

# The commit is recorded when building inside a git checkout and is empty when
# building from a source archive, where it would only repeat the tag anyway.
set(ZEROIV_COMMIT "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE ZEROIV_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE ZEROIV_COMMIT_RESULT)
    if(NOT ZEROIV_COMMIT_RESULT EQUAL 0)
        set(ZEROIV_COMMIT "")
    endif()
endif()

# Link the C/C++ runtime statically. The distributed artifact is a bare .asi
# that is dropped next to the game executable, so importing msvcp140.dll or
# vcruntime140.dll would make it fail to load on any machine without the Visual
# C++ redistributable installed — and silently, because the ASI loader only
# reports a missing dependency in its own log.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB_RECURSE PROJECT_SOURCE_FILES "src/*.h" "src/*.cpp")

macro(GroupSources curdir groupindex)
    file(GLOB children RELATIVE ${curdir} ${curdir}/*)

    foreach(child ${children})
        if(IS_DIRECTORY ${curdir}/${child})
            GroupSources(${curdir}/${child} ${groupindex}/${child})
        else()
            string(REPLACE "/" "\\" groupname ${groupindex})
            source_group(${groupname} FILES ${curdir}/${child})
        endif()
    endforeach()
endmacro()

GroupSources(${CMAKE_SOURCE_DIR}/src "Source Files")

add_subdirectory("src/vendor/minhook")

add_library(ZeroIVLoader SHARED ${PROJECT_SOURCE_FILES})
add_dependencies(ZeroIVLoader minhook)

include_directories(
    ./src
    ./src/vendor/minhook/include
)

set_target_properties(ZeroIVLoader PROPERTIES
    OUTPUT_NAME "ZeroIV"
    SUFFIX ".asi"
    CXX_STANDARD 20
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BIN/"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/BIN/Debug/"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BIN/Release/"
)

target_compile_definitions(ZeroIVLoader PRIVATE
    ZEROIV_LOADER_VERSION="${PROJECT_VERSION}"
    ZEROIV_LOADER_COMMIT="${ZEROIV_COMMIT}")

set_target_properties( minhook
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/LIB"
)

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
if(ENABLE_LOGS)
    target_compile_definitions(ZeroIVLoader PRIVATE _LOG)
endif()

target_link_directories(ZeroIVLoader PUBLIC "${CMAKE_BINARY_DIR}/LIB")
target_link_libraries(ZeroIVLoader version.lib ws2_32.lib debug minhook.x64d.lib optimized minhook.x64.lib)
