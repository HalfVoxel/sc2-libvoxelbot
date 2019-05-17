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

    CombatUpgrades player1upgrades = {
        UPGRADE_ID::TERRANINFANTRYWEAPONSLEVEL1,
        UPGRADE_ID::TERRANINFANTRYWEAPONSLEVEL2,
        UPGRADE_ID::TERRANINFANTRYWEAPONSLEVEL3,
        UPGRADE_ID::TERRANINFANTRYARMORSLEVEL1,
        UPGRADE_ID::TERRANINFANTRYARMORSLEVEL2,
    };

    CombatUpgrades player2upgrades = {
        UPGRADE_ID::ZERGMISSILEWEAPONSLEVEL1,
    };

    state.environment = &simulator.getCombatEnvironment(player1upgrades, player2upgrades);

    CombatSettings settings;
    // Simulate for at most 100 *game* seconds
    // Just to show that it can be configured, in this case 100 game seconds is more than enough for the battle to finish.
    settings.maxTime = 100;
    CombatResult outcome = simulator.predict_engage(state, settings);
    cout << outcome.state.toString() << endl;
    for (auto& unit : outcome.state.units) {
        cout << getUnitData(unit.type).name << " ended up with " << unit.health << " hp" << endl;
    }
    cout << "Winner player is " << outcome.state.owner_with_best_outcome() << endl;
    cout << "Battle concluded after " << outcome.time << " seconds" << endl;

    return 0;
}
