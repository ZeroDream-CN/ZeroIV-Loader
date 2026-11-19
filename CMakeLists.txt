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

# CONFIGURE_DEPENDS makes the build notice files added to src/ without a manual
# reconfigure: the project pulls its sources in with a glob, and an existing
# build tree would otherwise keep compiling yesterday's file list.
file(GLOB_RECURSE PROJECT_SOURCE_FILES CONFIGURE_DEPENDS "src/*.h" "src/*.cpp" "src/*.rc")

# The NG key tables come out of the retail game and are deliberately not tracked,
# so a fresh clone has no src/resources/ngdata.bin. The resource is emitted only
# when one is present: without it ngcrypt::init() reports the cipher unavailable
# and every caller fails cleanly, rather than the resource compiler aborting the
# build for everybody who did not extract the tables.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ngdata.bin")
    set(ZEROIV_NGDATA "NGDATA RCDATA \"ngdata.bin\"")
else()
    set(ZEROIV_NGDATA "// no ngdata.bin here: the RPF7 cipher stays unavailable")
endif()
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ngdata.rcinc" "${ZEROIV_NGDATA}\n")

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

# Sources are UTF-8, and the loader's prompts carry Chinese text. Without this
# MSVC reads them in the system code page and rejects the literals.
target_compile_options(ZeroIVLoader PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

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
