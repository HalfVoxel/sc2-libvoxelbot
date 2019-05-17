#include <libvoxelbot/buildorder/build_time_estimator.h>
#include <iostream>
#if LIBVOXELBOT_ENABLE_PYTHON
#include <pybind11/stl.h>
#include <pybind11/embed.h>
#endif
#include <libvoxelbot/utilities/python_utils.h>
#include <libvoxelbot/combat/simulator.h>
#include <libvoxelbot/buildorder/optimizer.h>

using namespace std;
using namespace sc2;

void BuildOptimizerNN::init() {
#if LIBVOXELBOT_ENABLE_PYTHON
    lock_guard<mutex> lock(python_thread_mutex);
    pybind11::exec(R"(
        import sys
        sys.path.append("bot/optimizer_train")
    )");
    pybind11::module trainer = pybind11::module::import("optimizer_train");
    predictFunction = trainer.attr("predict");
    cout << "Finished initializing NN" << endl;
#endif
}

vector<vector<float>> BuildOptimizerNN::predictTimeToBuild(const vector<pair<int, int>>& startingState, const BuildResources& startingResources, const vector < vector<pair<int, int>>>& targets) const {
#if LIBVOXELBOT_ENABLE_PYTHON
    lock_guard<mutex> lock(python_thread_mutex);
    auto res = predictFunction(startingState, tuple<int,int>(startingResources.minerals, startingResources.vespene), targets);
    return res.cast<vector<vector<float>>>();
#else
    return vector<vector<float>>(targets.size(), vector<float>(3));
#endif
}
