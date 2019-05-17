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

function (create_executable project_name mainfile)
	# TODO: The .h files don't seem to be necessary (think only .cpp files should be included here anyway)
    add_executable(${project_name} ${mainfile})

    # Sets the grouping in IDEs like visual studio (last parameter is the group name)
    set_target_properties(${project_name} PROPERTIES FOLDER target)

	if (MSVC)
		# set_target_properties(${project_name} PROPERTIES LINK_FLAGS "/NODEFAULTLIB:libcmt;libconcrt")
	endif ()

    target_link_libraries(${project_name} sc2api sc2lib sc2utils pybind11::embed libvoxelbot)
endfunction ()

# Note: trying to add SDL2-static as the extra_libs parameter here causes pybind11 modules to fail to link (for unknown reasons, maybe PIC related?)
set(libvoxelbot_sources
    "libvoxelbot/buildorder/build_order.cpp"
    "libvoxelbot/buildorder/build_state.cpp"
    "libvoxelbot/buildorder/build_time_estimator.cpp"
    "libvoxelbot/buildorder/optimizer.cpp"
    "libvoxelbot/buildorder/tracker.cpp"
    "libvoxelbot/combat/combat_environment.cpp"
    "libvoxelbot/combat/combat_upgrades.cpp"
    "libvoxelbot/combat/simulator.cpp"
    "libvoxelbot/common/unit_lists.cpp"
    "libvoxelbot/generated/abilities.cpp"
    "libvoxelbot/utilities/influence.cpp"
    "libvoxelbot/utilities/mappings.cpp"
    "libvoxelbot/utilities/pathfinding.cpp"
    "libvoxelbot/utilities/predicates.cpp"
    "libvoxelbot/utilities/profiler.cpp"
    "libvoxelbot/utilities/python_utils.cpp"
    "libvoxelbot/utilities/renderer.cpp"
    "libvoxelbot/utilities/unit_data_caching.cpp"
    "libvoxelbot/caching/dependency_analyzer.cpp"
)

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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cereal/include>
        src/generated
)

target_include_directories(libvoxelbot
    SYSTEM
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/../s2client-api/generated>
)


create_executable(test_combat_simulator "libvoxelbot/combat/simulator.test.cpp")
create_executable(test_build_optimizer "libvoxelbot/buildorder/optimizer.test.cpp")
create_executable(cache_mappings "libvoxelbot/caching/caching.cpp")
create_executable(example_combat_simulator "examples/combat_simulator.cpp")
create_executable(example_combat_simulator2 "examples/combat_simulator2.cpp")
create_executable(example_build_optimizer "examples/build_optimizer.cpp")