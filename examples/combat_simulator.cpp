#include <libvoxelbot/combat/simulator.h>
#include <iostream>

using namespace std;
using namespace sc2;

int main() {
    initMappings();
    CombatPredictor simulator;
    simulator.init();
    CombatState state = {{
        makeUnit(1, UNIT_TYPEID::TERRAN_MARINE),
        makeUnit(1, UNIT_TYPEID::TERRAN_MEDIVAC),
        makeUnit(1, UNIT_TYPEID::TERRAN_MARINE),

        makeUnit(2, UNIT_TYPEID::ZERG_ROACH),
        makeUnit(2, UNIT_TYPEID::ZERG_ROACH),
        makeUnit(2, UNIT_TYPEID::ZERG_ZERGLING),
    }};
    CombatResult outcome = simulator.predict_engage(state);
    cout << outcome.state.toString() << endl;
    cout << "Winner player is " << outcome.state.owner_with_best_outcome() << endl;

    return 0;
}