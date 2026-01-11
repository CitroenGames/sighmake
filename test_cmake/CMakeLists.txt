cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 17)
project(AudioEngine)

option(BUILD_EXAMPLES "Build miniaudio examples" OFF)

add_subdirectory(miniaudio)

file(GLOB_RECURSE SRC_FILES "src/*.cpp" "src/*.h")

add_library(${PROJECT_NAME} STATIC ${SRC_FILES})

target_include_directories(${PROJECT_NAME} PUBLIC src)

target_link_libraries(${PROJECT_NAME} miniaudio)

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
