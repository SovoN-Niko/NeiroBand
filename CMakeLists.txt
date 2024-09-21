cmake_minimum_required(VERSION 3.13)

project(CapsuleClientExample)

if(WIN32)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .dll)
else()
    set(CMAKE_FIND_LIBRARY_SUFFIXES .dylib)
    add_library(CapsuleClient SHARED IMPORTED)
    set_target_properties(CapsuleClient PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/libCapsuleClient.dylib")
endif()

set(CCESources 
    Source/main.cpp
)

set(RawSignalExampleSources
    Source/RawSignalExample.cpp
)

set(FilteredSignalExampleSources
    Source/FilteredSignalExample.cpp
)

set(CCEHeaders
    Include/Platforms.hpp
    Include/CClientAPI.h
)

add_executable(CapsuleClientExample ${CCESources} ${CCEHeaders})
target_include_directories(CapsuleClientExample
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include/Core)

add_executable(CapsuleRawSignalExample ${RawSignalExampleSources} ${CCEHeaders})
target_include_directories(CapsuleRawSignalExample
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include/Core)

add_executable(CapsuleFilteredSignalExample ${FilteredSignalExampleSources} ${CCEHeaders})
target_include_directories(CapsuleFilteredSignalExample
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include/Core)


# C++20 standard requirement to build code
set_target_properties(CapsuleClientExample CapsuleRawSignalExample CapsuleFilteredSignalExample PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/build
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)

target_link_libraries(CapsuleClientExample CapsuleClient)
target_link_libraries(CapsuleRawSignalExample CapsuleClient)
target_link_libraries(CapsuleFilteredSignalExample CapsuleClient)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(BUILD_OUTPUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug)
  message("DEBUG")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(BUILD_OUTPUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release)
  message("RELEASE")
endif()
