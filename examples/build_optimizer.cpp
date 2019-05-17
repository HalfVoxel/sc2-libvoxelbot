#include <libvoxelbot/buildorder/build_state.h>
#include <libvoxelbot/common/unit_lists.h>
#include <libvoxelbot/buildorder/build_time_estimator.h>

using namespace std;
using namespace sc2;

int main () {
    initMappings();

    // Start with 1 nexus and 12 probes
    BuildState state {{
        { UNIT_TYPEID::PROTOSS_NEXUS, 1 },
        { UNIT_TYPEID::PROTOSS_PROBE, 12 },
    }};
    // Start with 50 minerals
    state.resources.minerals = 50;
    state.resources.vespene = 0;
    // Our Nexus has 50 energy at the start of the game
    state.chronoInfo.addNexusWithEnergy(state.time, 50);

    // Find a good build order for producing some units
    BuildOrder buildOrder = findBestBuildOrderGenetic(state, {
        { BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT), 4 },
        { BuildOrderItem(UNIT_TYPEID::PROTOSS_STALKER), 4 },
        { BuildOrderItem(UNIT_TYPEID::PROTOSS_IMMORTAL), 2 },
        { BuildOrderItem(UPGRADE_ID::PROTOSSGROUNDARMORSLEVEL1), 1 },
    });

    // Print the build order
    cout << buildOrder.toString() << endl;

    // Print a more detailed build order output which includes timing information
    cout << buildOrder.toString(state, BuildOrderPrintMode::Detailed);

    return 0;
}
