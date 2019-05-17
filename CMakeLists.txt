cmake_minimum_required(VERSION 3.1)

project(libvoxelbot)

# PYTHON_EXECUTABLE

# Use bin as the directory for all executables.
# This will make protoc easy to find.
# set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
# set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

# set(PYTHON_EXECUTABLE "/Users/arong/anaconda3/bin/python")

# set(CMAKE_CONFIGURATION_TYPES, ${CMAKE_CONFIGURATION_TYPES} RelWithDebug)
# set(CMAKE_CXX_FLAGS_RELWITHDEBUG "-O2 -g -fPIC")

# More dependencies
# include_directories(SYSTEM "${PROJECT_BINARY_DIR}/s2client-api/generated")
# Doesn't seem to be required, so it is commented out for now (might be required on Windows or something)
# include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/s2client-api/contrib/SDL-mirror/include")
# include_directories("s2client-api/contrib/SDL-mirror/include")

# Note: trying to add SDL2-static as the extra_libs parameter here causes pybind11 modules to fail to link (for unknown reasons, maybe PIC related?)
file (GLOB libvoxelbot_sources "libvoxelbot/buildorder/*.cpp" "libvoxelbot/combat/*.cpp" "libvoxelbot/utilities/*.cpp" "libvoxelbot/generated/*.cpp" "libvoxelbot/common/*.cpp")

add_library(libvoxelbot ${libvoxelbot_sources})

# Sets the grouping in IDEs like visual studio (last parameter is the group name)
set_target_properties(libvoxelbot PROPERTIES FOLDER target)
target_link_libraries(libvoxelbot sc2api sc2lib sc2utils pybind11)
# Require C++14
set_property(TARGET libvoxelbot PROPERTY CXX_STANDARD 14)
set_property(TARGET libvoxelbot PROPERTY CXX_STANDARD_REQUIRED ON)
target_compile_options(libvoxelbot PRIVATE -Wall -Wextra -Wmissing-field-initializers -Wuninitialized -Wsometimes-uninitialized)

# Enable pybind11 bindings
target_compile_definitions(libvoxelbot PUBLIC LIBVOXELBOT_ENABLE_PYTHON=1)

target_include_directories(libvoxelbot
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../s2client-api/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../s2client-api/examples/common> # TODO: Remove?
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../s2client-api/contrib/SDL-mirror/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../cereal/include>
        src/generated
)

target_include_directories(libvoxelbot
    SYSTEM
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/../s2client-api/generated>
)
