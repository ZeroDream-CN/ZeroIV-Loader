cmake_minimum_required(VERSION 3.17)

project(ZeroIV-Loader VERSION 1.0.0 LANGUAGES CXX)

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

target_compile_definitions(ZeroIVLoader PRIVATE ZEROIV_LOADER_VERSION="${PROJECT_VERSION}")

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
