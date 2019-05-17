#include <libvoxelbot/buildorder/build_order.h>
#include <libvoxelbot/buildorder/build_state.h>

using namespace std;
using namespace sc2;

void printBuildOrderDetailed(const BuildState& startState, const BuildOrder& buildOrder, const vector<bool>* highlight) {
    BuildState state = startState;
    cout << "Starting units" << endl;
    for (auto u : startState.units) {
        cout << "\t" << u.units << "x " << UnitTypeToName(u.type);
        if (u.addon != UNIT_TYPEID::INVALID)
            cout << " + " << UnitTypeToName(u.addon);
        cout << endl;
    }
    cout << "Build order size " << buildOrder.size() << endl;
    bool success = state.simulateBuildOrder(buildOrder, [&](int i) {
        if (highlight != nullptr && (*highlight)[i]) {
            // Color the text
            cout << "\x1b[" << 48 << ";2;" << 228 << ";" << 26 << ";" << 28 << "m"; 
        }
        if (buildOrder.items[i].chronoBoosted) {
            // Color the text
            cout << "\x1b[" << 48 << ";2;" << 36 << ";" << 202 << ";" << 212 << "m"; 
        }
        string name = buildOrder[i].isUnitType() ? UnitTypeToName(buildOrder[i].typeID()) : UpgradeIDToName(buildOrder[i].upgradeID());
        cout << "Step " << i << "\t" << (int)(state.time / 60.0f) << ":" << (int)(fmod(state.time, 60.0f)) << "\t" << name << " "
             << "food: " << (state.foodCap() - state.foodAvailable()) << "/" << state.foodCap() << " resources: " << (int)state.resources.minerals << "+" << (int)state.resources.vespene << " " << (state.baseInfos.size() > 0 ? state.baseInfos[0].remainingMinerals : 0);

        // Reset color
        cout << "\033[0m";
        cout << endl;
    });

    cout << (success ? "Finished at " : "Failed at ");
    cout << (int)(state.time / 60.0f) << ":" << (int)(fmod(state.time, 60.0f)) << " resources: " << state.resources.minerals << "+" << state.resources.vespene << " mining speed: " << (int)round(state.miningSpeed().mineralsPerSecond*60) << "/min + " << (int)round(state.miningSpeed().vespenePerSecond*60) << "/min" << endl;

    // if (success) printMiningSpeedFuture(state);
}

void printBuildOrder(const vector<UNIT_TYPEID>& buildOrder) {
    cout << "Build order size " << buildOrder.size() << endl;
    for (size_t i = 0; i < buildOrder.size(); i++) {
        cout << "Step " << i << " " << UnitTypeToName(buildOrder[i]) << endl;
    }
}

void printBuildOrder(const BuildOrder& buildOrder) {
    cout << "Build order size " << buildOrder.size() << endl;
    for (size_t i = 0; i < buildOrder.size(); i++) {
        cout << "Step " << i << " ";
        if (buildOrder[i].isUnitType()) {
            cout << UnitTypeToName(buildOrder[i].typeID()) << endl;
        } else {
            cout << UpgradeIDToName(buildOrder[i].upgradeID()) << endl;
        }
    }
}