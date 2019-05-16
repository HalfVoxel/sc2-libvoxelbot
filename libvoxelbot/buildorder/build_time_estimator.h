#pragma once
//#include <pybind11/embed.h>
//#include <pybind11/stl.h>
#include <vector>
#include <libvoxelbot/utilities/mappings.h>
#include <pybind11/pybind11.h>

struct BuildResources;

struct BuildOptimizerNN {
    pybind11::object predictFunction;

    void init();

    std::vector<std::vector<float>> predictTimeToBuild(const std::vector<std::pair<int, int>>& startingState, const BuildResources& startingResources, const std::vector < std::vector<std::pair<int, int>>>& targets) const;
};
