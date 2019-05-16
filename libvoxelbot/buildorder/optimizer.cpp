#include <libvoxelbot/buildorder/optimizer.h>
#include <algorithm>
#include <limits>
#include <map>
#include <random>
#include <stack>
#include <iostream>
#include <cmath>
#include <libvoxelbot/utilities/mappings.h>
#include <libvoxelbot/utilities/predicates.h>
#include <libvoxelbot/utilities/profiler.h>
#include <libvoxelbot/utilities/stdutils.h>
#include <libvoxelbot/common/unit_lists.h>
#include <libvoxelbot/buildorder/tracker.h>
#include <libvoxelbot/utilities/build_state_serialization.h>

#include <fstream>

using namespace std;
using namespace sc2;
using namespace pybind11::literals;

const BuildOrderFitness BuildOrderFitness::ReallyBad = { 100000, BuildResources(0,0), { 0, 0 }, { 0, 0 } };

void printBuildOrder(const vector<UNIT_TYPEID>& buildOrder);
void printBuildOrder(const BuildOrder& buildOrder);

BuildState::BuildState(std::vector<std::pair<sc2::UNIT_TYPEID, int>> unitCounts) {
    assert(unitCounts.size() > 0);
    race = getUnitData(unitCounts[0].first).race;
    for (auto u : unitCounts) {
        addUnits(u.first, u.second);
        if (u.first == sc2::UNIT_TYPEID::PROTOSS_NEXUS) {
            // Add a nexus with some dummy energy
            for (int i = 0; i < u.second; i++) chronoInfo.addNexusWithEnergy(time, 25);
        }
    }
}

BuildState::BuildState(const ObservationInterface* observation, Unit::Alliance alliance, Race race, BuildResources resources, float time) : time(time), race(race), resources(resources) {
    map<UNIT_TYPEID, int> startingUnitsCount;
    map<UNIT_TYPEID, int> targetUnitsCount;

    auto ourUnits = observation->GetUnits(alliance);

    for (size_t i = 0; i < ourUnits.size(); i++) {
        auto unitType = ourUnits[i]->unit_type;

        // Addons are handled when the unit they are attached to are handled
        if (isAddon(unitType)) continue;

        // Exists at the same time as the non-phaseshift unit. Ignore it to avoid duplicates
        if (unitType == UNIT_TYPEID::PROTOSS_ADEPTPHASESHIFT) continue;

        // If the building is still in progress.
        // Don't do this for warpgates as they are just morphed, not in progress really
        if (ourUnits[i]->build_progress < 1 && unitType != UNIT_TYPEID::PROTOSS_WARPGATE) {
            if (race == Race::Terran) {
                // Ignore (will be handled by the order code below)
            } else {
                unitType = canonicalize(unitType);
                float buildTime = ticksToSeconds(getUnitData(unitType).build_time);
                float remainingTime = (1 - ourUnits[i]->build_progress) * buildTime;
                auto event = BuildEvent(BuildEventType::FinishedUnit, time + remainingTime, UNIT_TYPEID::INVALID, getUnitData(unitType).ability_id);
                // Note: don't bother to handle addons as this code never runs for terran
                addEvent(event);
            }
        } else {
            if (ourUnits[i]->add_on_tag != NullTag) {
                auto* addon = observation->GetUnit(ourUnits[i]->add_on_tag);
                // Simplify to e.g. TERRAN_REACTOR instead of TERRAN_BARRACKSREACTOR
                auto addonType = simplifyUnitType(addon->unit_type);
                assert(addonType != UNIT_TYPEID::INVALID);
                addUnits(canonicalize(unitType), addonType, 1);
            } else {
                addUnits(canonicalize(unitType), 1);
            }
        }

        // Note: at least during replays, orders don't seem to contain the target_unit_tag for build construction
        
        for (auto order : ourUnits[i]->orders) {
            auto createdUnit = abilityToUnit(order.ability_id);
            if (createdUnit != UNIT_TYPEID::INVALID) {
                if (canonicalize(createdUnit) == canonicalize(unitType)) {
                    // This is just a morph ability (e.g. lower a supply depot)
                    break;
                }

                float buildProgress = order.progress;
                if (order.target_unit_tag != NullTag) {
                    auto* targetUnit = observation->GetUnit(order.target_unit_tag);
                    if (targetUnit == nullptr) {
                        cerr << "Target for order does not seem to exist!??" << endl;
                        break;
                    }

                    assert(targetUnit != nullptr);

                    // In some cases the target unit can be something else, for example when constructing a refinery the target unit is the vespene geyser for a while
                    if (targetUnit->owner == ourUnits[i]->owner) {
                        buildProgress = targetUnit->build_progress;
                        // TODO: build_progress is not empirically always order.progress, when is it not so?
                        assert(targetUnit->build_progress >= 0 && targetUnit->build_progress <= 1);
                    }
                }

                float buildTime = ticksToSeconds(getUnitData(createdUnit).build_time);
                // TODO: Is this linear?
                float remainingTime = (1 - buildProgress) * buildTime;
                auto event = BuildEvent(BuildEventType::FinishedUnit, time + remainingTime, canonicalize(unitType), order.ability_id);
                
                if (ourUnits[i]->add_on_tag != NullTag) {
                    auto* addon = observation->GetUnit(ourUnits[i]->add_on_tag);
                    assert(addon != nullptr);
                    // Normalize from e.g. TERRAN_BARRACKSTECHLAB to TERRAN_TECHLAB
                    event.casterAddon = simplifyUnitType(addon->unit_type);
                }

                // Make the caster busy first
                makeUnitsBusy(event.caster, event.casterAddon, 1);
                if (event.caster == UNIT_TYPEID::PROTOSS_PROBE) {
                    // Probes are special cased as they are not busy for the whole duration of the build.
                    // Assume the probe will be free in a few seconds
                    addEvent(BuildEvent(BuildEventType::MakeUnitAvailable, time + min(remainingTime, 4.0f), event.caster, sc2::ABILITY_ID::INVALID));
                }
                // if (event.ability == ABILITY_ID::MORPH_WARPGATE) {
                //     cout << "Got morph ability on " << UnitTypeToName(ourUnits[i]->unit_type) << " " << UnitTypeToName(createdUnit) << endl;
                //     // Just make the unit busy instead otherwise the simulator will crash when it cannot find a gateway to remove when the ability is finished
                //     addEvent(BuildEvent(BuildEventType::MakeUnitAvailable, event.time, event.caster, sc2::ABILITY_ID::INVALID));
                // } else {
                addEvent(event);
                // }
            }

            // Only process the first order (this bot should never have more than one anyway)
            break;
        }
    }

    if (alliance == Unit::Alliance::Self) {
    // Add all upgrades
        auto& availableUnits = getAvailableUnitsForRace(race, UnitCategory::BuildOrderOptions);
        for (size_t i = 0; i < availableUnits.size(); i++) {
            auto item = availableUnits.getBuildOrderItem(i);
            if (!item.isUnitType()) {
                // Note: slightly incorrect. Assumes warpgate research is already done when it may actually be in progress.
                // But since the proper event for upgrades is not added above, this is better than nothing.
                if (isUpgradeDoneOrInProgress(observation, item.upgradeID())) {
                    upgrades.add(item.upgradeID());
                }
            }
        }
    }

    vector<Point2D> basePositions;
    for (auto u : ourUnits) {
        if (isTownHall(u->unit_type) && u->build_progress >= 1) {
            basePositions.push_back(u->pos);
            baseInfos.push_back(BaseInfo(0, 0, 0));

            if (u->unit_type == UNIT_TYPEID::PROTOSS_NEXUS) {
                chronoInfo.addNexusWithEnergy(time, u->energy);
            }
        }
    }
    auto neutralUnits = observation->GetUnits(Unit::Alliance::Neutral);
    for (auto u : neutralUnits) {
        if (u->mineral_contents > 0) {
            for (size_t i = 0; i < baseInfos.size(); i++) {
                if (DistanceSquared2D(u->pos, basePositions[i]) < 10*10) {
                    baseInfos[i].remainingMinerals += u->mineral_contents;
                    break;
                }
            }
        }
    }

    // Sanity check
    for (auto u : units) {
        assert(u.availableUnits() >= 0);
    }
}

void BuildState::transitionToWarpgates (const function<void(const BuildEvent&)>* eventCallback) {
    assert(upgrades.hasUpgrade(sc2::UPGRADE_ID::WARPGATERESEARCH));
    const float WarpGateTransitionTime = 7;
    for (auto& u : units) {
        if (u.type == UNIT_TYPEID::PROTOSS_GATEWAY && u.busyUnits < u.units) {
            int delta = u.units - u.busyUnits;
            u.units -= delta;
            assert(u.units >= 0);
            assert(u.availableUnits() >= 0);
            addUnits(UNIT_TYPEID::PROTOSS_WARPGATE, delta);
            makeUnitsBusy(UNIT_TYPEID::PROTOSS_WARPGATE, UNIT_TYPEID::INVALID, delta);
            for (int i = 0; i < delta; i++) {
                addEvent(BuildEvent(BuildEventType::MakeUnitAvailable, time + WarpGateTransitionTime, UNIT_TYPEID::PROTOSS_WARPGATE, ABILITY_ID::MORPH_WARPGATE));
            }

            // Note: event not actually used in the simulator, only used for the callback
            if (eventCallback != nullptr) {
                for(int i = 0; i < delta; i++) {
                    (*eventCallback)(BuildEvent(BuildEventType::WarpGateTransition, time, UNIT_TYPEID::PROTOSS_GATEWAY, ABILITY_ID::MORPH_WARPGATE));
                }
            }
            // Note: important to break as the addUnits call may invalidate the iterator
            break;
        }
    }
}

void BuildState::makeUnitsBusy(UNIT_TYPEID type, UNIT_TYPEID addon, int delta) {
    if (delta == 0)
        return;

    for (auto& u : units) {
        if (u.type == type && u.addon == addon) {
            u.busyUnits += delta;
            assert(u.availableUnits() >= 0);
            assert(u.busyUnits >= 0);

            // Ensure gateways transition to warpgates as soon as possible
            // Note: cannot do this because it may invalidate the events vector which may mess with the BuildEvent::apply method.
            // if (type == UNIT_TYPEID::PROTOSS_GATEWAY && hasWarpgateResearch && delta < 0) transitionToWarpgates();
            return;
        }
    }
    assert(false);
}

void BuildState::addUnits(UNIT_TYPEID type, int delta) {
    addUnits(type, UNIT_TYPEID::INVALID, delta);
}

void BuildState::addUnits(UNIT_TYPEID type, UNIT_TYPEID addon, int delta) {
    if (delta == 0)
        return;

    for (auto& u : units) {
        if (u.type == type && u.addon == addon) {
            u.units += delta;
            if (u.availableUnits() < 0) {
                cout << "Buggy units? " << UnitTypeToName(u.type) << " " << u.availableUnits() << " " << u.units << " " << u.busyUnits << endl;
            }
            assert(u.availableUnits() >= 0);
            return;
        }
    }

    if (delta > 0) {
        units.emplace_back(type, addon, delta);
    } else {
        cerr << "Cannot remove " << UnitTypeToName(type) << endl;
        assert(false);
    }
}

void BuildState::killUnits(UNIT_TYPEID type, UNIT_TYPEID addon, int count) {
    if (count == 0)
        return;
    
    assert(count > 0);

    for (auto& u : units) {
        if (u.type == type && u.addon == addon) {
            u.units -= count;
            assert(u.units >= 0);
            while(u.availableUnits() < 0) {
                bool found = false;
                for (int i = events.size() - 1; i >= 0; i--) {
                    auto& ev = events[i];
                    if (ev.caster == type && ev.casterAddon == addon && (ev.type == BuildEventType::MakeUnitAvailable || (ev.type == BuildEventType::FinishedUnit && type != UNIT_TYPEID::PROTOSS_PROBE) || ev.type == BuildEventType::FinishedUpgrade)) {
                        // This event is guaranteed to keep a unit busy
                        // Let's erase the event to free the unit for other work
                        // Note that FinishedUnit events with caster==Probe do not keep the probe busy: there will be a second MakeUnitAvailable event that marks the probe as busy for a shorter time
                        events.erase(events.begin() + i);
                        u.busyUnits--;
                        found = true;
                        break;
                    }
                }

                // TODO: Check if this happens oftens, if so it might be worth it to optimize this case
                if (!found) {
                    // Forcefully remove busy units.
                    // Usually they are occupied with some event, but in some cases they are just marked as busy.
                    // For example workers for a few seconds at the start of the game to simulate a delay.
                    u.busyUnits--;
                    cerr << "Forcefully removed busy unit " << UnitTypeToName(u.type) << " " << u.units << " " << u.busyUnits << " " << count << endl;
                    for (auto u : units) {
                        cerr << "Unit " << UnitTypeToName(u.type) << " " << u.units << "(-" << u.busyUnits << ")" << endl;
                    }
                    cerr << "Event count: " << events.size() << endl;
                    for (auto e : events) {
                        cerr << "Event " << e.time << " " << e.type << " " << UnitTypeToName(e.caster) << endl;
                    }
                    assert(false);
                }
            }
            assert(u.availableUnits() >= 0);
            return;
        }
    }

    assert(false);
}

void MiningSpeed::simulateMining (BuildState& state, float dt) const {
    float totalWeight = 0;
    for (auto& base : state.baseInfos) {
        auto slots = base.mineralSlots();
        float weight = slots.first * 1.5f + slots.second;
        totalWeight += weight;
    }
    float normalizationFactor = 1.0f / (totalWeight + 0.0001f);
    float deltaMineralsPerWeight = mineralsPerSecond * dt * normalizationFactor;
    for (auto& base : state.baseInfos) {
        auto slots = base.mineralSlots();
        float weight = slots.first * 1.5f + slots.second;
        base.mineMinerals(deltaMineralsPerWeight * weight);
    }
    state.resources.minerals += mineralsPerSecond * dt;
    state.resources.vespene += vespenePerSecond * dt;
}

/** Returns a new build time assuming the process is accelerated using a chrono boost that ends at the specified time */
float modifyBuildTimeWithChronoBoost (float currentTime, float chronoEndTime, float buildTime) {
    if (chronoEndTime <= currentTime) return buildTime;

    // Amount of time which will be chrono boosted
    float chronoTime = min(buildTime * 0.666666f, chronoEndTime - currentTime);
    buildTime -= chronoTime/2.0f;
    return buildTime;
}

std::pair<bool, float> ChronoBoostInfo::getChronoBoostEndTime(sc2::UNIT_TYPEID caster, float currentTime) {
    for (int i = chronoEndTimes.size() - 1; i >= 0; i--) {
        auto p = chronoEndTimes[i];
        if (p.first == caster) {
            if (p.second <= currentTime) {
                // Too late, chrono has ended already
                chronoEndTimes.erase(chronoEndTimes.begin() + i);
                continue;
            }

            // Chrono still active!
            float endTime = p.second;
            chronoEndTimes.erase(chronoEndTimes.begin() + i);
            return make_pair(true, endTime);
        }
    }

    return make_pair(false, 0);
}

std::pair<bool, float> ChronoBoostInfo::useChronoBoost(float time) {
    for (auto& offset : energyOffsets) {
        float currentEnergy = time * NexusEnergyPerSecond + offset;
        if (currentEnergy >= ChronoBoostEnergy) {
            offset -= ChronoBoostEnergy;
            return make_pair(true, time + ChronoBoostDuration);
        }
    }

    return make_pair(false, 0);
}

MiningSpeed BuildState::miningSpeed(bool debug) const {
    int harvesters = 0;
    int mules = 0;
    int bases = 0;
    int geysers = 0;
    for (auto& unit : units) {
        // TODO: Normalize type?
        if (isBasicHarvester(unit.type)) {
            harvesters += unit.availableUnits();
        }

        if (unit.type == UNIT_TYPEID::TERRAN_MULE) {
            mules += unit.availableUnits();
        }

        if (isTownHall(unit.type)) {
            bases += unit.units;
        }

        if (isVespeneHarvester(unit.type)) {
            geysers += unit.units;
        }
    }

    int highYieldMineralHarvestingSlots = 0;
    int lowYieldMineralHarvestingSlots = 0;
    for (int i = 0; i < bases; i++) {
        if (i < (int)baseInfos.size()) {
            auto t = baseInfos[i].mineralSlots();
            highYieldMineralHarvestingSlots += t.first;
            lowYieldMineralHarvestingSlots += t.second;
        } else {
            // Assume lots of minerals
            highYieldMineralHarvestingSlots += 16;
            lowYieldMineralHarvestingSlots += 8;
        }
    }

    int vespeneMining = min(harvesters / 2, geysers * 3);
    int mineralMining = harvesters - vespeneMining;

    // Maximum effective harvesters (todo: account for more things)
    // First 2 harvesters per mineral field yield more minerals than the 3rd one.
    int highYieldHarvesters = min(highYieldMineralHarvestingSlots, mineralMining);
    int lowYieldHarvesters = min(lowYieldMineralHarvestingSlots, mineralMining - highYieldHarvesters);

    // TODO: Check units here!
    const float FasterSpeedMultiplier = 1.4f;
    const float LowYieldMineralsPerMinute = 22 * FasterSpeedMultiplier;
    const float HighYieldMineralsPerMinute = 40 * FasterSpeedMultiplier;
    const float VespenePerMinute = 38 * FasterSpeedMultiplier;
    const float MinutesPerSecond = 1 / 60.0f;

    MiningSpeed speed;
    // cout << mineralMining << " " << highYieldHarvesters << " " << lowYieldHarvesters << " " << foodAvailable() << endl;
    speed.mineralsPerSecond = (lowYieldHarvesters * LowYieldMineralsPerMinute + highYieldHarvesters * HighYieldMineralsPerMinute) * MinutesPerSecond;
    speed.vespenePerSecond = vespeneMining * VespenePerMinute * MinutesPerSecond;

    return speed;
}

float BuildState::timeToGetResources(MiningSpeed miningSpeed, float mineralCost, float vespeneCost) const {
    mineralCost -= resources.minerals;
    vespeneCost -= resources.vespene;
    float time = 0;
    if (mineralCost > 0) {
        if (miningSpeed.mineralsPerSecond == 0)
            return numeric_limits<float>::infinity();
        time = mineralCost / miningSpeed.mineralsPerSecond;
    }
    if (vespeneCost > 0) {
        if (miningSpeed.vespenePerSecond == 0)
            return numeric_limits<float>::infinity();
        time = max(time, vespeneCost / miningSpeed.vespenePerSecond);
    }
    return time;
}

void BuildState::addEvent(BuildEvent event) {
    // TODO: Insertion sort or something
    events.push_back(event);
    sort(events.begin(), events.end());
}

// All actions up to and including the end time will have been completed
void BuildState::simulate(float endTime, const function<void(const BuildEvent&)>* eventCallback) {
    if (endTime <= time)
        return;

    auto currentMiningSpeed = miningSpeed();
    // int eventIndex;
    // for (eventIndex = 0; eventIndex < events.size(); eventIndex++) {
    while(events.size() > 0) {
        // TODO: Slow
        auto ev = *events.begin();

        // auto& ev = events[eventIndex];
        if (ev.time > endTime) {
            break;
        }

        events.erase(events.begin());
        float dt = ev.time - time;
        currentMiningSpeed.simulateMining(*this, dt);
        time = ev.time;

        ev.apply(*this);

        if (ev.impactsEconomy()) {
            currentMiningSpeed = miningSpeed();
        } else {
            // Ideally this would always hold, but when we simulate actual bases with mineral patches the approximations used are not entirely accurate and the mining rate may change even when there is no economically significant event
            assert(baseInfos.size() > 0 || currentMiningSpeed == miningSpeed());
        }

        if (eventCallback != nullptr) (*eventCallback)(ev);
        
        // TODO: Maybe a bit slow...
        if (upgrades.hasUpgrade(sc2::UPGRADE_ID::WARPGATERESEARCH)) transitionToWarpgates(eventCallback);
    }

    // events.erase(events.begin(), events.begin() + eventIndex);

    {
        float dt = endTime - time;
        currentMiningSpeed.simulateMining(*this, dt);
        time = endTime;
    }
}

bool BuildState::simulateBuildOrder(const BuildOrder& buildOrder, const function<void(int)> callback, bool waitUntilItemsFinished) {
    BuildOrderState state(make_shared<BuildOrder>(buildOrder));
    return simulateBuildOrder(state, callback, waitUntilItemsFinished);
}

/** Returns the time it takes for a warpgate to build the unit.
 * If the unit cannot be built in a warpgate the default building time is returned.
 */
float getWarpgateBuildingTime(UNIT_TYPEID unitType, float defaultTime) {
    switch(unitType) {
        case UNIT_TYPEID::PROTOSS_ZEALOT:
            return 20;
        case UNIT_TYPEID::PROTOSS_SENTRY:
            return 23;
        case UNIT_TYPEID::PROTOSS_STALKER:
            return 23;
        case UNIT_TYPEID::PROTOSS_ADEPT:
            return 20;
        case UNIT_TYPEID::PROTOSS_HIGHTEMPLAR:
            return 32;
        case UNIT_TYPEID::PROTOSS_DARKTEMPLAR:
            return 32;
        default:
            return defaultTime;
    }
}

bool BuildState::simulateBuildOrder(BuildOrderState& buildOrder, const function<void(int)> callback, bool waitUntilItemsFinished, float maxTime, const function<void(const BuildEvent&)>* eventCallback) {
    float lastEventInBuildOrder = 0;

    // Loop through the build order
    for (; buildOrder.buildIndex < (int)buildOrder.buildOrder->size(); buildOrder.buildIndex++) {
        auto item = (*buildOrder.buildOrder)[buildOrder.buildIndex];
        if (item.chronoBoosted) buildOrder.lastChronoUnit = item.rawType();

        while (true) {
            float nextSignificantEvent = numeric_limits<float>::infinity();
            for (auto& ev : events) {
                if (ev.impactsEconomy()) {
                    nextSignificantEvent = ev.time;
                    break;
                }
            }

            bool isUnitAddon;
            int mineralCost, vespeneCost;
            ABILITY_ID ability;
            UNIT_TYPEID techRequirement;
            bool techRequirementIsAddon, isItemStructure;
            float buildTime;

            if (item.isUnitType()) {
                auto unitType = item.typeID();
                auto& unitData = getUnitData(unitType);

                if ((unitData.tech_requirement != UNIT_TYPEID::INVALID && !unitData.require_attached && !hasEquivalentTech(unitData.tech_requirement)) || (unitData.food_required > 0 && foodAvailable() < unitData.food_required)) {
                    if (events.empty()) {
                        // cout << "No tech at index " << buildOrder.buildIndex << endl;
                        return false;
                        cout << "Requires " << UnitTypeToName(unitData.tech_requirement) << endl;
                        cout << foodAvailable() << " " << unitData.food_required << endl;
                        cout << UnitTypeToName(unitType) << endl;
                        printBuildOrder(*buildOrder.buildOrder);
                        cout << "Current unit counts:" << endl;
                        for (auto u : units) {
                            cout << UnitTypeToName(u.type) << " " << UnitTypeToName(u.addon) << " " << u.units << endl;
                        }
                        // __builtin_trap();
                        // exit(1);
                        return false;
                    }

                    if (events[0].time > maxTime) {
                        simulate(maxTime, eventCallback);
                        return true;
                    }
                    simulate(events[0].time, eventCallback);
                    continue;
                }

                isUnitAddon = isAddon(unitType);

                // TODO: Maybe just use lookup table
                mineralCost = unitData.mineral_cost;
                vespeneCost = unitData.vespene_cost;
                UNIT_TYPEID previous = upgradedFrom(unitType);
                if (previous != UNIT_TYPEID::INVALID && !isUnitAddon) {
                    auto& previousUnitData = getUnitData(previous);
                    mineralCost -= previousUnitData.mineral_cost;
                    vespeneCost -= previousUnitData.vespene_cost;
                }

                ability = unitData.ability_id;
                techRequirement = unitData.tech_requirement;
                techRequirementIsAddon = unitData.require_attached;
                isItemStructure = isStructure(unitType);
                buildTime = ticksToSeconds(unitData.build_time);
            } else {
                auto upgrade = item.upgradeID();
                auto upgradeDependency = getUpgradeUpgradeDependency(upgrade);
                auto unitDependency = getUpgradeUnitDependency(upgrade);
                if ((upgradeDependency != UPGRADE_ID::INVALID && !upgrades.hasUpgrade(upgradeDependency)) || (unitDependency != UNIT_TYPEID::INVALID && !hasEquivalentTech(unitDependency))) {
                    if (events.empty()) {
                        cout << "No tech at index " << buildOrder.buildIndex << endl;
                        return false;
                    }

                    if (events[0].time > maxTime) {
                        simulate(maxTime, eventCallback);
                        return true;
                    }
                    simulate(events[0].time, eventCallback);
                    continue;
                }

                auto& upgradeData = getUpgradeData(item.upgradeID());

                isUnitAddon = false;
                mineralCost = upgradeData.mineral_cost;
                vespeneCost = upgradeData.vespene_cost;
                ability = upgradeData.ability_id;
                techRequirement = UNIT_TYPEID::INVALID;
                techRequirementIsAddon = false;
                isItemStructure = false;
                buildTime = ticksToSeconds(upgradeData.research_time);
            }

            auto currentMiningSpeed = miningSpeed();
            // When do we have enough resources for this item
            float eventTime = time + timeToGetResources(currentMiningSpeed, mineralCost, vespeneCost);

            // If it would be after the next economically significant event then the time estimate is likely not accurate (the mining speed might change in the middle)
            if (eventTime > nextSignificantEvent && nextSignificantEvent <= maxTime) {
                simulate(nextSignificantEvent, eventCallback);
                continue;
            }
            
            if (eventTime > maxTime) {
                simulate(maxTime, eventCallback);
                return true;
            }

            if (isinf(eventTime)) {
                // This can happen in some cases.
                // Most common is when the unit requires vespene gas, but the player only has 1 scv and that one will be allocated to minerals.
                return false;
            }

            // Fast forward until we can pay for the item
            simulate(eventTime, eventCallback);

            // Make sure that some unit can cast this ability
            if (abilityToCasterUnit(ability).size() == 0) {
                cerr << "No known caster of the ability " << AbilityTypeToName(ability) << " for unit " << UnitTypeToName(item.rawType()) << endl;
                assert(false);
            }

            // Find an appropriate caster for this ability
            BuildUnitInfo* casterUnit = nullptr;
            UNIT_TYPEID casterUnitType = UNIT_TYPEID::INVALID;
            UNIT_TYPEID casterAddonType = UNIT_TYPEID::INVALID;
            for (UNIT_TYPEID caster : abilityToCasterUnit(ability)) {
                for (auto& casterCandidate : units) {
                    if (casterCandidate.type == caster && casterCandidate.availableUnits() > 0 && (!techRequirementIsAddon || casterCandidate.addon == techRequirement)) {
                        // Addons can only be added to units that do not yet have any other addons
                        if (isUnitAddon && casterCandidate.addon != UNIT_TYPEID::INVALID)
                            continue;

                        // Prefer to use casters that do not have addons
                        if (casterUnit == nullptr || casterUnit->addon != UNIT_TYPEID::INVALID) {
                            casterUnit = &casterCandidate;
                            casterUnitType = caster;
                            casterAddonType = casterCandidate.addon;
                        }
                    }
                }
            }

            // If we don't have a caster yet, then we might have to simulate a bit (the caster might be training right now, or currently busy)
            if (casterUnit == nullptr) {
                if (casterUnitType == UNIT_TYPEID::ZERG_LARVA) {
                    addUnits(UNIT_TYPEID::ZERG_LARVA, 1);
                    continue;
                }

                if (events.empty()) {
                    // cout << "No possible caster " << UnitTypeToName(unitType) << endl;
                    return false;
                    printBuildOrder(*buildOrder.buildOrder);
                    for (auto& casterCandidate : units) {
                        cout << "Caster: " << UnitTypeToName(casterCandidate.type) << " " << casterCandidate.units << "/" << casterCandidate.availableUnits() << " " << UnitTypeToName(casterCandidate.addon) << endl;
                    }
                    // exit(1);
                    return false;
                }

                if (events[0].time > maxTime) {
                    simulate(maxTime, eventCallback);
                    return true;
                }
                simulate(events[0].time, eventCallback);
                continue;
            }

            // Pay for the item
            resources.minerals -= mineralCost;
            resources.vespene -= vespeneCost;

            // Mark the caster as being busy
            casterUnit->busyUnits++;
            assert(casterUnit->availableUnits() >= 0);

            if (casterUnit->type == UNIT_TYPEID::PROTOSS_WARPGATE) {
                buildTime = getWarpgateBuildingTime(item.typeID(), buildTime);
            }

            // Try to use an existing chrono boost
            pair<bool, float> chrono = chronoInfo.getChronoBoostEndTime(casterUnit->type, time);

            if (buildOrder.lastChronoUnit == item.rawType()) {
                // Use a new chrono boost if this unit is supposed to be chrono boosted
                if (!chrono.first) {
                    chrono = chronoInfo.useChronoBoost(time);
                }

                if (chrono.first) {
                    // Clear the flag because the unit was indeed boosted
                    // If it was not, the flag will remain and the next unit of the same type will be boosted
                    buildOrder.lastChronoUnit = UNIT_TYPEID::INVALID;
                }
            }

            if (chrono.first) {
                // cout << "Modified build time " << time << "," << chrono.second << " = " << buildTime << " -- > ";
                buildTime = modifyBuildTimeWithChronoBoost(time, chrono.second, buildTime);                
                // cout << buildTime << endl;
            }

            // Compensate for workers having to move to the building location
            /*if (isItemStructure) {
                buildTime += 4;
            }*/

            // Create a new event for when the item is complete
            auto newEvent = BuildEvent(item.isUnitType() ? BuildEventType::FinishedUnit : BuildEventType::FinishedUpgrade, time + buildTime, casterUnit->type, ability);
            newEvent.casterAddon = casterUnit->addon;
            newEvent.chronoEndTime = chrono.second;
            lastEventInBuildOrder = max(lastEventInBuildOrder, newEvent.time);
            addEvent(newEvent);
            if (casterUnit->type == UNIT_TYPEID::PROTOSS_PROBE) {
                addEvent(BuildEvent(BuildEventType::MakeUnitAvailable, time + 6, UNIT_TYPEID::PROTOSS_PROBE, ABILITY_ID::INVALID));
            }

            if (callback != nullptr)
                callback(buildOrder.buildIndex);
            break;
        }
    }

    if (waitUntilItemsFinished) simulate(lastEventInBuildOrder, eventCallback);
    return true;
}

float BuildState::foodCap() const {
    float totalSupply = 0;
    for (auto& unit : units) {
        auto& data = getUnitData(unit.type);
        totalSupply += data.food_provided * unit.units;
    }
    return totalSupply;
}

// Note that food is a floating point number, zerglings in particular use 0.5 food.
// It is still safe to work with floating point numbers because they can exactly represent whole numbers and whole numbers + 0.5 exactly up to very large values.
float BuildState::foodAvailable() const {
    float totalSupply = 0;
    for (auto& unit : units) {
        auto& data = getUnitData(unit.type);
        totalSupply += (data.food_provided - data.food_required) * unit.units;
    }
    // Units in construction use food, but they don't provide food (yet)
    for (auto& ev : events) {
        UNIT_TYPEID unit = abilityToUnit(ev.ability);
        if (unit != UNIT_TYPEID::INVALID) {
            totalSupply -= getUnitData(unit).food_required;
        }
    }

    // Not necessarily true in all game states
    // assert(totalSupply >= 0);
    return totalSupply;
}

float BuildState::foodAvailableInFuture() const {
    float totalSupply = 0;
    for (auto& unit : units) {
        auto& data = getUnitData(unit.type);
        totalSupply += (data.food_provided - data.food_required) * unit.units;
    }
    // Units in construction use food and will provide food
    for (auto& ev : events) {
        UNIT_TYPEID unit = abilityToUnit(ev.ability);
        if (unit != UNIT_TYPEID::INVALID) {
            totalSupply += getUnitData(unit).food_provided - getUnitData(unit).food_required;
        }
    }

    // Not necessarily true in all game states
    // assert(totalSupply >= 0);
    return totalSupply;
}

bool BuildState::hasEquivalentTech(UNIT_TYPEID type) const {
    for (auto& unit : units) {
        auto& unitData = getUnitData(unit.type);
        if (unit.units > 0) {
            if (unit.type == type) {
                return true;
            }
            for (auto t : unitData.tech_alias)
                if (t == type)
                    return true;
        }
    }
    return false;
}

bool BuildEvent::impactsEconomy() const {
    // TODO: Optimize?
    // TODO: isStructure is very aggressive, isTownHall is more appropriate?
    UNIT_TYPEID unit = abilityToUnit(ability);
    return isBasicHarvester(unit) || isBasicHarvester(caster) || isStructure(unit) || getUnitData(unit).food_provided > 0;
}

void BuildEvent::apply(BuildState& state) const {
    switch (type) {
        case FinishedUnit: {
            UNIT_TYPEID unit = abilityToUnit(ability);
            assert(unit != UNIT_TYPEID::INVALID);

            // Probes are special because they don't actually have to stay while the building is being built
            // Another event will make them available a few seconds after the order has been issued.
            if (caster != UNIT_TYPEID::PROTOSS_PROBE && caster != UNIT_TYPEID::INVALID) {
                state.makeUnitsBusy(caster, casterAddon, -1);
            }

            if (isAddon(unit)) {
                // Normalize from e.g. TERRAN_BARRACKSTECHLAB to TERRAN_TECHLAB
                assert(caster != UNIT_TYPEID::INVALID);
                state.addUnits(caster, simplifyUnitType(unit), 1);
            } else {
                state.addUnits(unit, 1);
            }

            auto upgradedFromUnit = upgradedFrom(unit);
            if (upgradedFromUnit != UNIT_TYPEID::INVALID) {
                state.addUnits(upgradedFromUnit, casterAddon, -1);
            }

            if (unit == UNIT_TYPEID::PROTOSS_NEXUS) {
                state.chronoInfo.addNexusWithEnergy(state.time, NexusInitialEnergy);
            }
            break;
        }
        case FinishedUpgrade: {
            state.upgrades.add(abilityToUpgrade(ability));
            // Make the caster unit available again
            assert(caster != UNIT_TYPEID::INVALID);
            state.makeUnitsBusy(caster, casterAddon, -1);
            break;
        }
        case SpawnLarva: {
            state.addUnits(UNIT_TYPEID::ZERG_LARVA, 3);
            break;
        }
        case MuleTimeout: {
            state.addUnits(UNIT_TYPEID::TERRAN_MULE, -1);
            break;
        }
        case MakeUnitAvailable: {
            assert(caster != UNIT_TYPEID::INVALID);
            state.makeUnitsBusy(caster, casterAddon, -1);
            break;
        }
        case WarpGateTransition: {
            assert(false);
            break;
        }
    }

    // Add remaining chrono boost to be used for other things
    if (chronoEndTime > state.time) state.chronoInfo.addRemainingChronoBoost(caster, chronoEndTime);
}

/** Adds all dependencies of the required type to the requirements stack in the order that they need to be performed in order to fulfill all preconditions for building/training the required type
 * 
 * For example if the player only has some SCVs and a command center and the required type is a marine, then both a barracks and a supply depot will be added to the stack.
 * Only takes care of direct tech dependencies, not indirect ones like supply or resource requirements.
 */
static void traceDependencies(const vector<int>& unitCounts, const AvailableUnitTypes& availableUnitTypes, stack<BuildOrderItem>& requirements, UNIT_TYPEID requiredType) {
    // Need to break here to avoid an infinite loop of SCV requires command center requires SCV ...
    if (isBasicHarvester(requiredType))
        return;

    // Protoss buildings need pylons (in theory not strictly necessary, but they can only be placed in power fields... so... in practice yes)
    if (getUnitData(requiredType).race == Race::Protoss && isStructure(requiredType) && (requiredType != UNIT_TYPEID::PROTOSS_NEXUS && requiredType != UNIT_TYPEID::PROTOSS_PYLON && requiredType != UNIT_TYPEID::PROTOSS_ASSIMILATOR)) {
        requiredType = UNIT_TYPEID::PROTOSS_PYLON;
        if (unitCounts[availableUnitTypes.getIndex(requiredType)] == 0) {
            // Need to add this type to the build order
            requirements.emplace(requiredType);
            traceDependencies(unitCounts, availableUnitTypes, requirements, requiredType);
        }
    }

    auto& unitData = getUnitData(requiredType);
    if (unitData.tech_requirement != UNIT_TYPEID::INVALID) {
        requiredType = unitData.tech_requirement;

        // Check if the tech requirement is an addon
        if (unitData.require_attached) {
            // techlab -> barracks-techlab for example.
            // We need to do this as otherwise the build order becomes ambiguous.
            requiredType = getSpecificAddonType(abilityToCasterUnit(unitData.ability_id)[0], unitData.tech_requirement);

            if (unitCounts[availableUnitTypes.getIndex(requiredType)] == 0) {
                // Need to add this type to the build order
                requirements.emplace(requiredType);
                // Note: don't trace dependencies for addons as they will only depend on the caster of this unit, which we will already trace.
                // This is really a bit of a hack to avoid having to prune the requirements for duplicates, but it's good for performance too.
            }
        } else if (unitCounts[availableUnitTypes.getIndex(requiredType)] == 0) {
            // Do a more thorough check because we might have a unit which is tech aliased to the required type
            // This can happen for example when we have warpgates and some unit has a gateway as a dependency
            bool anyValid = false;
            for (auto t : availableUnitTypes.getUnitTypes()) {
                for (auto t2 : getUnitData(t).tech_alias) {
                    if (t2 == requiredType) {
                        anyValid = true;
                        break;
                    }
                }
            }

            if (!anyValid) {
                // Need to add this type to the build order
                requirements.emplace(requiredType);
                traceDependencies(unitCounts, availableUnitTypes, requirements, requiredType);
            }
        }
    }

    if (abilityToCasterUnit(unitData.ability_id).size() > 0) {
        bool found = false;
        for (auto possibleCaster : abilityToCasterUnit(unitData.ability_id)) {
            if (unitCounts[availableUnitTypes.getIndex(possibleCaster)] > 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            requiredType = abilityToCasterUnit(unitData.ability_id)[0];

            // Ignore larva
            if (requiredType != UNIT_TYPEID::ZERG_LARVA) {
                requirements.emplace(requiredType);
                traceDependencies(unitCounts, availableUnitTypes, requirements, requiredType);
            }
        }
    }
}

static void traceDependencies(const vector<int>& unitCounts, const AvailableUnitTypes& availableUnitTypes, stack<BuildOrderItem>& requirements, UPGRADE_ID requiredUpgrade) {
    {
        auto unitType = getUpgradeUnitDependency(requiredUpgrade);
        if (unitType != UNIT_TYPEID::INVALID) {
            if (unitCounts[availableUnitTypes.getIndex(unitType)] == 0) {
                requirements.push(BuildOrderItem(unitType));
                traceDependencies(unitCounts, availableUnitTypes, requirements, unitType);
            }
        }
    }

    {
        auto upgrade = getUpgradeUpgradeDependency(requiredUpgrade);
        if (upgrade != UPGRADE_ID::INVALID) {
            // This upgrade depends on an earlier upgrade (previous level)
            if (unitCounts[availableUnitTypes.getIndex(upgrade)] == 0) {
                requirements.push(BuildOrderItem(upgrade));
                traceDependencies(unitCounts, availableUnitTypes, requirements, upgrade);
            }
        } else {
            // This upgrade does not depend on an earlier upgrade (previous level)
            // Add the building which one can research the research from as a dependency.
            // We don't do this if it depended on an upgrade because then we might add the caster as a dependency twice
            // (it is assumed that the caster is the same for the dependency)
            auto unitType = abilityToCasterUnit(getUpgradeData(requiredUpgrade).ability_id)[0];
            if (unitCounts[availableUnitTypes.getIndex(unitType)] == 0) {
                requirements.push(BuildOrderItem(unitType));
                traceDependencies(unitCounts, availableUnitTypes, requirements, unitType);
            }
        }
    }
}

/** Finalizes the gene's build order by adding in all implicit steps */
BuildOrder addImplicitBuildOrderSteps(const vector<GeneUnitType>& buildOrder, Race race, float startingFood, const vector<int>& startingUnitCounts, const vector<int>& startingAddonCountPerUnitType, const AvailableUnitTypes& availableUnitTypes, vector<bool>* outIsOriginalItem = nullptr) {
    vector<int> unitCounts = startingUnitCounts;
    vector<int> addonCountPerUnitType = startingAddonCountPerUnitType;
    assert(unitCounts.size() == availableUnitTypes.size());
    BuildOrder finalBuildOrder;
    float totalFood = startingFood;
    UNIT_TYPEID currentSupplyUnit = getSupplyUnitForRace(race);
    UNIT_TYPEID currentVespeneHarvester = getVespeneHarvesterForRace(race);
    UNIT_TYPEID currentTownHall = getTownHallForRace(race);

    // Note: stack always starts empty at each iteration, so it could be moved to inside the loop
    // but having it outside avoids some allocations+deallocations.
    stack<BuildOrderItem> reqs;

    for (GeneUnitType type : buildOrder) {
        auto item = availableUnitTypes.getBuildOrderItem(type);
        reqs.push(item);

        if (item.isUnitType()) {
            UNIT_TYPEID unitType;
            unitType = item.typeID();

            // Analyze the prerequisites for the action and add in implicit dependencies
            // (e.g to train a marine, we first need a baracks)
            // TODO: Need more sophisticated tracking because some dependencies can become invalid by other actions
            // (e.g. when building a planetary fortress, a command center is 'used up')
            // auto requiredType = unitType;
            traceDependencies(unitCounts, availableUnitTypes, reqs, unitType);
        } else {
            traceDependencies(unitCounts, availableUnitTypes, reqs, item.upgradeID());
        }

        while (!reqs.empty()) {
            auto requirement = reqs.top();
            if (requirement.isUnitType()) {
                UNIT_TYPEID requirementUnitType = requirement.typeID();
                auto& d = getUnitData(requirementUnitType);
                // If we don't have enough food, push a supply unit (e.g. supply depot) to the stack
                float foodDelta = d.food_provided - d.food_required;

                // Check which unit (if any) this unit was created from (e.g. command center -> orbital command)
                auto& previous = hasBeen(requirementUnitType);
                if (previous.size() > 1) {
                    auto& d2 = getUnitData(previous[1]);
                    foodDelta -= (d2.food_provided - d2.food_required);
                }

                // BUILD ADDITIONAL PYLONS
                if (totalFood + foodDelta < 0 && foodDelta < 0) {
                    reqs.emplace(currentSupplyUnit);
                    continue;
                }

                // Make sure we have a refinery if we need vespene for this unit
                if (d.vespene_cost > 0 && unitCounts[availableUnitTypes.getIndex(currentVespeneHarvester)] == 0) {
                    reqs.emplace(currentVespeneHarvester);
                    continue;
                }

                // Only allow 2 vespene harvesting buildings per base
                // TODO: Might be better to account for this by having a much lower harvesting rate?
                if (requirementUnitType == currentVespeneHarvester) {
                    int numBases = 0;
                    for (size_t i = 0; i < availableUnitTypes.size(); i++) {
                        if (isTownHall(availableUnitTypes.getUnitType(i)))
                            numBases += unitCounts[i];
                    }

                    if (unitCounts[availableUnitTypes.getIndex(currentVespeneHarvester)] >= numBases * 2) {
                        reqs.emplace(currentTownHall);
                        continue;
                    }
                }

                // Addons should always list the original building in the previous list
                assert(!isAddon(requirementUnitType) || previous.size() > 1);

                if (previous.size() > 1) {
                    int idx = availableUnitTypes.getIndex(previous[1]);
                    assert(unitCounts[idx] > 0);
                    if (isAddon(requirementUnitType)) {
                        // Try to mark another building has having an addon
                        if (addonCountPerUnitType[idx] < unitCounts[idx]) {
                            addonCountPerUnitType[idx]++;
                        } else {
                            // If there are no possible such buildings, then we need to add a new one of those buildings
                            reqs.emplace(previous[1]);
                            traceDependencies(unitCounts, availableUnitTypes, reqs, previous[1]);
                            continue;
                        }
                    } else {
                        // Remove the previous unit if this is an upgrade (e.g. command center -> planetary fortress)
                        // However make sure not to do it for addons, as the original building is still kept in that case
                        unitCounts[idx]--;
                    }
                }

                totalFood += foodDelta;
                unitCounts[availableUnitTypes.getIndex(requirementUnitType)] += 1;
            } else {
                unitCounts[availableUnitTypes.getIndex(requirement.upgradeID())] += 1;
            }
            finalBuildOrder.items.push_back(requirement);
            reqs.pop();
            // The last item that we add is the one from the original build order
            if (outIsOriginalItem != nullptr) outIsOriginalItem->push_back(reqs.empty());
        }
    }

    return finalBuildOrder;
}

/** A gene represents a build order.
 * The build order may contain many implicit steps which will be added when the build order is finalized.
 * For example if any build item has any preconditions (e.g. training a marine requires a barracks which requires a supply depot, etc.) then when the build order is finalized any
 * implicit steps are added such that the build order can always be performed almost no matter what the build order is.
 * That is, the simulation will not return an error because for example it tried to train a marine even though the player has no baracks.
 * 
 * The build order is represented as a vector of integers which are indices into a provided availableUnitTypes list (for example all relevant unit types for a terran player).
 */
struct BuildOrderGene {
    // Indices are into the availableUnitTypes list
    vector<GeneUnitType> buildOrder;

    /** Validates that the build order will train/build the given units and panics otherwise */
    void validate(const vector<int>& actionRequirements) const {
#if DEBUG
        vector<int> remainingRequirements = actionRequirements;
        for (auto type : buildOrder)
            remainingRequirements[type.type]--;
        for (auto r : remainingRequirements)
            assert(r <= 0);
#endif
    }

    /** Mutates the build order by moving items around randomly.
     * Ensures that the build order will still create the units given by the action requirements.
     * 
     * The action requirements is a list as long as availableUnitTypes that specifies for each unit type how many that the build order should train/build.
     */
    void mutateMove(float amount, const vector<int>& actionRequirements, default_random_engine& seed) {
        validate(actionRequirements);

        bernoulli_distribution shouldMutate(amount);
        for (size_t i = 0; i < buildOrder.size(); i++) {
            if (shouldMutate(seed)) {
                normal_distribution<float> dist(i, buildOrder.size() * 0.25f);
                while (true) {
                    int moveIndex = (int)round(dist(seed));

                    // Try again until we get a number in the right range
                    if (moveIndex < 0 || moveIndex >= (int)buildOrder.size())
                        continue;

                    int s = 0;
                    for (auto c : buildOrder)
                        s += c.type;

                    auto elem = buildOrder[i];
                    // Move element i to the new position by pushing the elements in between one step
                    if (moveIndex < (int)i) {
                        for (int j = i; j > moveIndex; j--)
                            buildOrder[j] = buildOrder[j - 1];
                    } else {
                        for (int j = i; j < moveIndex; j++)
                            buildOrder[j] = buildOrder[j + 1];
                    }
                    buildOrder[moveIndex] = elem;

                    int s2 = 0;
                    for (auto c : buildOrder)
                        s2 += c.type;
                    assert(s == s2);
                    break;
                }
            }
        }

        validate(actionRequirements);
    }

    /** Mutates the build order by adding or removing items.
     * Ensures that the build order will still create the units given by the action requirements.
     * 
     * The action requirements is a list as long as availableUnitTypes that specifies for each unit type how many that the build order should train/build.
     */
    void mutateAddRemove(float amount, default_random_engine& seed, const vector<int>& actionRequirements, const vector<int>& addableUnits, const AvailableUnitTypes& availableUnitTypes, bool allowChronoBoost) {
        vector<int> remainingRequirements = actionRequirements;
        for (size_t i = 0; i < buildOrder.size(); i++) {
            remainingRequirements[buildOrder[i].type]--;
        }

        // Remove elements randomly unless that violates the requirements
        bernoulli_distribution shouldRemove(amount);
        bernoulli_distribution shouldChrono(amount);
        bernoulli_distribution shouldRemoveChrono(0.4);

        for (int i = (int)buildOrder.size() - 1; i >= 0; i--) {
            if (remainingRequirements[buildOrder[i].type] < 0 && shouldRemove(seed)) {
                // Remove it!
                remainingRequirements[buildOrder[i].type]++;
                buildOrder.erase(buildOrder.begin() + i);
            }
        }

        // Add elements randomly
        bernoulli_distribution shouldAdd(amount);
        for (size_t i = 0; i < buildOrder.size(); i++) {
            if (shouldAdd(seed)) {
                // Add something!
                uniform_int_distribution<int> dist(0, addableUnits.size() - 1);
                buildOrder.insert(buildOrder.begin() + i, GeneUnitType(addableUnits[dist(seed)]));
            }

            if (allowChronoBoost) {
                if (shouldChrono(seed) && availableUnitTypes.canBeChronoBoosted(buildOrder[i].type)) {
                    buildOrder[i].chronoBoosted = true;
                } else if (buildOrder[i].chronoBoosted && shouldRemoveChrono(seed)) {
                    buildOrder[i].chronoBoosted = false;
                }
            } else {
                buildOrder[i].chronoBoosted = false;
            }
        }

        validate(actionRequirements);
    }

    /*static BuildOrderGene crossover(const BuildOrderGene& parent1, const BuildOrderGene& parent2, default_random_engine& seed, const vector<int>& actionRequirements) {
        uniform_real_distribution<float> dist(0, 1);
        float split = dist(seed);
        int index1 = min((int)floor(split * parent1.buildOrder.size()), (int)parent1.buildOrder.size());
        int index2 = min((int)floor(split * parent2.buildOrder.size()), (int)parent2.buildOrder.size());

        // Add the elements from parent1
        BuildOrderGene gene;
        for (int i = 0; i < index1; i++) {
            gene.buildOrder.push_back(parent1.buildOrder[i]);
        }

        // In the middle: insert required actions that are missing from the crossover
        // Calculate any missing requirements
        vector<int> remainingRequirements = actionRequirements;
        for (auto type : gene.buildOrder)
            remainingRequirements[type]--;
        for (int i = index2; i < parent2.buildOrder.size(); i++)
            remainingRequirements[parent2.buildOrder[i]]--;

        for (GeneUnitType i = 0; i < remainingRequirements.size(); i++) {
            for (int j = remainingRequirements[i] - 1; j >= 0; j--)
                gene.buildOrder.push_back(i);
        }

        // Shuffle the elements in the middle that we just added (not the part from parent1)
        shuffle(gene.buildOrder.begin() + index1, gene.buildOrder.end(), seed);

        // Add the elements from parent2
        for (int i = index2; i < parent2.buildOrder.size(); i++) {
            gene.buildOrder.push_back(parent2.buildOrder[i]);
        }

        gene.validate(actionRequirements);

        return gene;
    }*/

    BuildOrderGene()
        : buildOrder() {}
    
    BuildOrderGene(vector<int> buildOrder) : buildOrder(buildOrder.size()) {
        for (size_t i = 0; i < buildOrder.size(); i++) this->buildOrder[i] = GeneUnitType(buildOrder[i]);
    }

    /** Creates a random build order that will train/build the given units.
     * 
     * The action requirements is a list as long as availableUnitTypes that specifies for each unit type how many that the build order should train/build.
     */
    BuildOrderGene(default_random_engine& seed, const vector<int>& actionRequirements) {
        for (size_t i = 0; i < actionRequirements.size(); i++) {
            for (int j = actionRequirements[i] - 1; j >= 0; j--)
                buildOrder.push_back(GeneUnitType(i));
        }
        shuffle(buildOrder.begin(), buildOrder.end(), seed);
    }

    /** Creates a gene from a given build order */
    BuildOrderGene(const BuildOrder& seedBuildOrder, const AvailableUnitTypes& availableUnitTypes, const vector<int>& actionRequirements) {
        vector<int> remainingRequirements = actionRequirements;
        for (auto u : seedBuildOrder.items) {
            auto item = availableUnitTypes.getGeneItem(u);
            buildOrder.push_back(item);
            remainingRequirements[item.type]--;
        }
        for (size_t i = 0; i < remainingRequirements.size(); i++) {
            int r = remainingRequirements[i];
            for (auto j = 0; j < r; j++) {
                buildOrder.push_back(GeneUnitType(i));
            }
        }
    }
    
    BuildOrder constructBuildOrder(Race race, float startingFood, const vector<int>& startingUnitCounts, const vector<int>& startingAddonCountPerUnitType, const AvailableUnitTypes& availableUnitTypes) const {
        return addImplicitBuildOrderSteps(buildOrder, race, startingFood, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes);
    }
};

int miningSpeedFutureColor = 0;
void printMiningSpeedFuture(const BuildState& startState);

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

    if (success) printMiningSpeedFuture(state);
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

float BuildOrderFitness::score() const {
    float s = -fmax(time, 2 * 60.0f);
    s += ((resources.minerals + 2 * resources.vespene) + (miningSpeed.mineralsPerSecond + 2 * miningSpeed.vespenePerSecond) * 60) * 0.001f;
    // s = log(s) - time/400.0f;
    return s;
}

pair<vector<int>, vector<int>> calculateStartingUnitCounts(const BuildState& startState, const AvailableUnitTypes& availableUnitTypes) {
    vector<int> startingUnitCounts(availableUnitTypes.size());
    vector<int> startingAddonCountPerUnitType(availableUnitTypes.size());

    for (auto p : startState.units) {
        int index = availableUnitTypes.getIndexMaybe(p.type);
        if (index != -1) {
            startingUnitCounts[index] += p.units;
            if (p.addon != UNIT_TYPEID::INVALID) {
                startingUnitCounts[availableUnitTypes.getIndex(getSpecificAddonType(p.type, p.addon))] += p.units;
                startingAddonCountPerUnitType[index] += p.units;
            }
        }
    }
    for (auto u : startState.upgrades) {
        auto index = availableUnitTypes.getIndexMaybe(u);
        if (index != -1) startingUnitCounts[index]++;
    }
    return { startingUnitCounts, startingAddonCountPerUnitType };
}

/** Calculates the fitness of a given build order gene, a higher value is better */
BuildOrderFitness calculateFitness(const BuildState& startState, const vector<int>& startingUnitCounts, const vector<int>& startingAddonCountPerUnitType, const AvailableUnitTypes& availableUnitTypes, const BuildOrderGene& gene) {
    BuildState state = startState;
    vector<float> finishedTimes;
    auto buildOrder = gene.constructBuildOrder(startState.race, startState.foodAvailableInFuture(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes);
    if (!state.simulateBuildOrder(buildOrder, [&](int index) {
            finishedTimes.push_back(state.time);
        })) {
        // Build order could not be executed, that is bad.
        return BuildOrderFitness::ReallyBad;
    }

    // Find the average completion time of the army units in the build order (they are usually the important parts, though non army units also contribute a little bit)
    float avgTime = state.time * 0.00001f;
    float totalWeight = 0.00001f;
    for (size_t i = 0; i < finishedTimes.size(); i++) {
        float t = finishedTimes[i];
        // Really this is just a proxy for if not in economic units. Should fix
        float w = !buildOrder[i].isUnitType() || isArmy(buildOrder[i].typeID()) ? 1.0f : 0.1f;
        if (!buildOrder[i].isUnitType() && buildOrder[i].upgradeID() == UPGRADE_ID::WARPGATERESEARCH) w = 0.1f;
        totalWeight += w;
        avgTime += w * (t + 20);  // +20 to take into account that we want the finished time of the unit, but we only have the start time
    }

    avgTime /= totalWeight;
    float originalTime = state.time;
    float time = originalTime + (avgTime*2) * 0.001f;
    // float time = avgTime*2;

    // Simulate until at least the 2 minutes mark, this ensures that the agent will do some economic stuff if nothing else
    state.simulate(60 * 2);

    auto miningSpeed = state.miningSpeed();
    auto resources = state.resources;
    resources.minerals += miningSpeed.mineralsPerSecond * (time - state.time);
    resources.vespene += miningSpeed.vespenePerSecond * (time - state.time);

    float mineralEndTime = originalTime + 60;
    while(state.time < mineralEndTime) {
        if (state.foodAvailableInFuture() <= 2) {
            if (!state.simulateBuildOrder({ UNIT_TYPEID::PROTOSS_PYLON }, nullptr, false)) break;
        } else {
            if (!state.simulateBuildOrder({ UNIT_TYPEID::PROTOSS_PROBE }, nullptr, false)) break;
        }
    }

    auto miningSpeed2 = state.miningSpeed();
    float dt = max(state.time - originalTime, 1.0f);
    MiningSpeed miningSpeedPerSecond = { (miningSpeed2.mineralsPerSecond - miningSpeed.mineralsPerSecond) / dt, (miningSpeed2.vespenePerSecond - miningSpeed.vespenePerSecond) / dt };

    return BuildOrderFitness(time, resources, miningSpeed, miningSpeedPerSecond);
    // return -max(avgTime * 2, 2 * 60.0f) + (state.resources.minerals + 2 * state.resources.vespene) * 0.001 + (miningSpeed.mineralsPerSecond + 2 * miningSpeed.vespenePerSecond) * 60 * 0.005;
}

/** Convenience function */
BuildOrderFitness calculateFitness(const BuildState& startState, const BuildOrder& buildOrder) {
    const AvailableUnitTypes& availableUnitTypes = getAvailableUnitsForRace(startState.race, UnitCategory::BuildOrderOptions);
    const AvailableUnitTypes& allEconomicUnits = getAvailableUnitsForRace(startState.race, UnitCategory::Economic);

    // Simulate the starting state until all current events have finished, only then do we know which exact unit types the player will start with.
    // This is important for implicit dependencies in the build order.
    // If say a factory is under construction, we don't want to implictly build another factory if the build order specifies that a tank is supposed to be built.
    BuildState startStateAfterEvents = startState;
    startStateAfterEvents.simulate(startStateAfterEvents.time + 1000000);

    vector<int> startingUnitCounts;
    vector<int> startingAddonCountPerUnitType;
    tie(startingUnitCounts, startingAddonCountPerUnitType) = calculateStartingUnitCounts(startStateAfterEvents, availableUnitTypes);

    vector<int> economicUnits;
    for (size_t i = 0; i < allEconomicUnits.size(); i++) {
        economicUnits.push_back(remapAvailableUnitIndex(i, allEconomicUnits, availableUnitTypes));
    }

    vector<int> actionRequirements(availableUnitTypes.size());
    BuildOrderGene gene;

    for (auto item : buildOrder.items) {
        gene.buildOrder.push_back(availableUnitTypes.getGeneItem(item));
    }

    auto newFitness = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, gene);
    return newFitness;
}

void printMiningSpeedFuture(const BuildState& startState) {
    BuildState state = startState;
    float et = state.time + 2*60;
    cout << "= [" << endl;
    vector<float> times;
    vector<float> minerals;
    vector<float> vespene;
    while(state.time < et) {
        minerals.push_back(2 * (state.miningSpeed().mineralsPerSecond + state.miningSpeed().vespenePerSecond - startState.miningSpeed().mineralsPerSecond - startState.miningSpeed().vespenePerSecond) + startState.miningSpeed().mineralsPerSecond + startState.miningSpeed().vespenePerSecond);
        times.push_back(state.time);
        if (state.foodAvailableInFuture() <= 2) {
            if (!state.simulateBuildOrder({ UNIT_TYPEID::PROTOSS_PYLON }, nullptr, false)) break;
        } else {
            if (!state.simulateBuildOrder({ UNIT_TYPEID::PROTOSS_PROBE }, nullptr, false)) break;
        }
        // cout << "[" << state.time << ", " << state.miningSpeed().mineralsPerSecond << " " << state.miningSpeed().vespenePerSecond << "]," << endl;
        vespene.push_back(state.miningSpeed().vespenePerSecond);
        // break;
    }

    vector<string> colors = {
        "#E50021",
        "#E30032",
        "#E20043",
        "#E10053",
        "#DF0063",
        "#DE0073",
        "#DD0083",
        "#DB0093",
        "#DA00A2",
        "#D900B1",
        "#D700C0",
        "#D600CF",
        "#CC00D5",
        "#BB00D3",
        "#AA00D2",
        "#9900D1",
        "#8900D0",
        "#7800CE",
        "#6800CD",
        "#5900CC",
        "#4900CA",
        "#3900C9",
        "#2A00C8",
        "#1B00C6",
        "#0C00C5",
        "#0002C4",
        "#0010C2",
        "#001EC1",
        "#002DC0",
        "#003ABF",
    };
    pybind11::module::import("matplotlib.pyplot").attr("plot")(times, minerals, "color"_a=colors[miningSpeedFutureColor % colors.size()]);
    // pybind11::module::import("matplotlib.pyplot").attr("scatter")(times, vespene);
}

/** Try really hard to do optimize the gene.
 * This will try to swap adjacent items in the build order as well as trying to remove all non-essential items.
 */
// TODO: Add operation to remove all items that are implied anyway (i.e. if removing the item and then adding in implicit steps returns the same result as just adding in the implicit steps)
BuildOrderGene locallyOptimizeGene(const BuildState& startState, const vector<int>& startingUnitCounts, const vector<int>& startingAddonCountPerUnitType, const AvailableUnitTypes& availableUnitTypes, const vector<int>& actionRequirements, const BuildOrderGene& gene) {
    vector<int> currentActionRequirements = actionRequirements;
    for (auto b : gene.buildOrder)
        currentActionRequirements[b.type]--;

    auto startFitness = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, gene);
    auto fitness = startFitness;
    BuildOrderGene newGene = gene;
    for (int i = 0; i < 2; i++) {
        for (size_t j = 0; j < newGene.buildOrder.size(); j++) {
            bool lastItem = j == newGene.buildOrder.size() - 1;
            if (lastItem || newGene.buildOrder[j] != newGene.buildOrder[j + 1]) {
                // Check if the item is non-essential
                if (currentActionRequirements[newGene.buildOrder[j].type] < 0) {
                    // Try removing
                    auto orig = newGene.buildOrder[j];
                    newGene.buildOrder.erase(newGene.buildOrder.begin() + j);

                    auto newFitness = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, newGene);

                    // Check if the new fitness is better
                    // Also always remove non-essential items at the end of the build order
                    // Note: !(a < b) == (a >= b)
                    if (!(newFitness < fitness) || lastItem) {
                        currentActionRequirements[orig.type] += 1;
                        fitness = newFitness;
                        j--;
                        continue;
                    } else {
                        // Revert erase
                        newGene.buildOrder.insert(newGene.buildOrder.begin() + j, orig);
                    }
                }

                // Try swapping
                if (j + 1 < newGene.buildOrder.size()) {
                    swap(newGene.buildOrder[j], newGene.buildOrder[j + 1]);
                    auto newFitness = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, newGene);

                    if (fitness < newFitness) {
                        fitness = newFitness;
                    } else {
                        // Revert swap
                        swap(newGene.buildOrder[j], newGene.buildOrder[j + 1]);
                    }
                }
            }
        }
    }

    return newGene;
}



vector<UNIT_TYPEID> unitTypesZerg = {
    UNIT_TYPEID::ZERG_BANELING,
    UNIT_TYPEID::ZERG_BANELINGNEST,
    UNIT_TYPEID::ZERG_BROODLORD,
    UNIT_TYPEID::ZERG_CORRUPTOR,
    UNIT_TYPEID::ZERG_DRONE,
    UNIT_TYPEID::ZERG_EVOLUTIONCHAMBER,
    UNIT_TYPEID::ZERG_EXTRACTOR,
    UNIT_TYPEID::ZERG_GREATERSPIRE,
    UNIT_TYPEID::ZERG_HATCHERY,
    UNIT_TYPEID::ZERG_HIVE,
    UNIT_TYPEID::ZERG_HYDRALISK,
    UNIT_TYPEID::ZERG_HYDRALISKDEN,
    UNIT_TYPEID::ZERG_INFESTATIONPIT,
    UNIT_TYPEID::ZERG_INFESTOR,
    UNIT_TYPEID::ZERG_LAIR,
    UNIT_TYPEID::ZERG_LURKERDENMP,
    // UNIT_TYPEID::ZERG_LURKERMP,
    UNIT_TYPEID::ZERG_MUTALISK,
    UNIT_TYPEID::ZERG_NYDUSCANAL,
    UNIT_TYPEID::ZERG_NYDUSNETWORK,
    UNIT_TYPEID::ZERG_OVERLORD,
    UNIT_TYPEID::ZERG_OVERLORDTRANSPORT,
    UNIT_TYPEID::ZERG_OVERSEER,
    UNIT_TYPEID::ZERG_QUEEN,
    UNIT_TYPEID::ZERG_RAVAGER,
    UNIT_TYPEID::ZERG_ROACH,
    UNIT_TYPEID::ZERG_ROACHWARREN,
    UNIT_TYPEID::ZERG_SPAWNINGPOOL,
    UNIT_TYPEID::ZERG_SPINECRAWLER,
    UNIT_TYPEID::ZERG_SPIRE,
    UNIT_TYPEID::ZERG_SPORECRAWLER,
    UNIT_TYPEID::ZERG_SWARMHOSTMP,
    UNIT_TYPEID::ZERG_ULTRALISK,
    UNIT_TYPEID::ZERG_ULTRALISKCAVERN,
    UNIT_TYPEID::ZERG_VIPER,
    UNIT_TYPEID::ZERG_ZERGLING,
};


static vector<UNIT_TYPEID> unitTypesTerran = {
    UNIT_TYPEID::TERRAN_ARMORY,
    UNIT_TYPEID::TERRAN_BANSHEE,
    UNIT_TYPEID::TERRAN_BARRACKS,
    UNIT_TYPEID::TERRAN_BARRACKSREACTOR,
    UNIT_TYPEID::TERRAN_BARRACKSTECHLAB,
    UNIT_TYPEID::TERRAN_BATTLECRUISER,
    UNIT_TYPEID::TERRAN_BUNKER,
    UNIT_TYPEID::TERRAN_COMMANDCENTER,
    UNIT_TYPEID::TERRAN_CYCLONE,
    UNIT_TYPEID::TERRAN_ENGINEERINGBAY,
    UNIT_TYPEID::TERRAN_FACTORY,
    UNIT_TYPEID::TERRAN_FACTORYREACTOR,
    UNIT_TYPEID::TERRAN_FACTORYTECHLAB,
    UNIT_TYPEID::TERRAN_FUSIONCORE,
    UNIT_TYPEID::TERRAN_GHOST,
    UNIT_TYPEID::TERRAN_GHOSTACADEMY,
    UNIT_TYPEID::TERRAN_HELLION,
    UNIT_TYPEID::TERRAN_HELLIONTANK,
    UNIT_TYPEID::TERRAN_LIBERATOR,
    UNIT_TYPEID::TERRAN_MARAUDER,
    UNIT_TYPEID::TERRAN_MARINE,
    UNIT_TYPEID::TERRAN_MEDIVAC,
    UNIT_TYPEID::TERRAN_MISSILETURRET,
    UNIT_TYPEID::TERRAN_MULE,
    UNIT_TYPEID::TERRAN_ORBITALCOMMAND,
    UNIT_TYPEID::TERRAN_PLANETARYFORTRESS,
    UNIT_TYPEID::TERRAN_RAVEN,
    UNIT_TYPEID::TERRAN_REAPER,
    UNIT_TYPEID::TERRAN_REFINERY,
    UNIT_TYPEID::TERRAN_SCV,
    UNIT_TYPEID::TERRAN_SENSORTOWER,
    UNIT_TYPEID::TERRAN_SIEGETANK,
    UNIT_TYPEID::TERRAN_STARPORT,
    UNIT_TYPEID::TERRAN_STARPORTREACTOR,
    UNIT_TYPEID::TERRAN_STARPORTTECHLAB,
    UNIT_TYPEID::TERRAN_SUPPLYDEPOT,
    UNIT_TYPEID::TERRAN_THOR,
    UNIT_TYPEID::TERRAN_VIKINGFIGHTER,
    UNIT_TYPEID::TERRAN_WIDOWMINE,
};

vector<UNIT_TYPEID> unitTypesProtoss = {
    UNIT_TYPEID::PROTOSS_ADEPT,
    // UNIT_TYPEID::PROTOSS_ADEPTPHASESHIFT,
    // UNIT_TYPEID::PROTOSS_ARCHON, // TODO: Special case creation rule
    UNIT_TYPEID::PROTOSS_ASSIMILATOR,
    UNIT_TYPEID::PROTOSS_CARRIER,
    UNIT_TYPEID::PROTOSS_COLOSSUS,
    UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
    UNIT_TYPEID::PROTOSS_DARKSHRINE,
    UNIT_TYPEID::PROTOSS_DARKTEMPLAR,
    UNIT_TYPEID::PROTOSS_DISRUPTOR,
    // UNIT_TYPEID::PROTOSS_DISRUPTORPHASED,
    UNIT_TYPEID::PROTOSS_FLEETBEACON,
    UNIT_TYPEID::PROTOSS_FORGE,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_HIGHTEMPLAR,
    UNIT_TYPEID::PROTOSS_IMMORTAL,
    // UNIT_TYPEID::PROTOSS_INTERCEPTOR,
    UNIT_TYPEID::PROTOSS_MOTHERSHIP,
    // UNIT_TYPEID::PROTOSS_MOTHERSHIPCORE,
    UNIT_TYPEID::PROTOSS_NEXUS,
    UNIT_TYPEID::PROTOSS_OBSERVER,
    UNIT_TYPEID::PROTOSS_ORACLE,
    // UNIT_TYPEID::PROTOSS_ORACLESTASISTRAP,
    UNIT_TYPEID::PROTOSS_PHOENIX,
    UNIT_TYPEID::PROTOSS_PHOTONCANNON,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PYLON,
    // UNIT_TYPEID::PROTOSS_PYLONOVERCHARGED,
    UNIT_TYPEID::PROTOSS_ROBOTICSBAY,
    UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
    UNIT_TYPEID::PROTOSS_SENTRY,
    UNIT_TYPEID::PROTOSS_SHIELDBATTERY,
    UNIT_TYPEID::PROTOSS_STALKER,
    UNIT_TYPEID::PROTOSS_STARGATE,
    UNIT_TYPEID::PROTOSS_TEMPEST,
    UNIT_TYPEID::PROTOSS_TEMPLARARCHIVE,
    UNIT_TYPEID::PROTOSS_TWILIGHTCOUNCIL,
    UNIT_TYPEID::PROTOSS_VOIDRAY,
    UNIT_TYPEID::PROTOSS_WARPGATE,
    UNIT_TYPEID::PROTOSS_WARPPRISM,
    // UNIT_TYPEID::PROTOSS_WARPPRISMPHASING,
    UNIT_TYPEID::PROTOSS_ZEALOT,
};

pair<BuildOrder, vector<bool>> expandBuildOrderWithImplicitSteps (const BuildState& startState, BuildOrder buildOrder) {
    const AvailableUnitTypes& availableUnitTypes = getAvailableUnitsForRace(startState.race, UnitCategory::BuildOrderOptions);

    // Simulate the starting state until all current events have finished, only then do we know which exact unit types the player will start with.
    // This is important for implicit dependencies in the build order.
    // If say a factory is under construction, we don't want to implictly build another factory if the build order specifies that a tank is supposed to be built.
    BuildState startStateAfterEvents = startState;
    startStateAfterEvents.simulate(startStateAfterEvents.time + 1000000);

    vector<int> startingUnitCounts;
    vector<int> startingAddonCountPerUnitType;
    tie(startingUnitCounts, startingAddonCountPerUnitType) = calculateStartingUnitCounts(startStateAfterEvents, availableUnitTypes);

    vector<GeneUnitType> remappedBuildOrder(buildOrder.size());
    for (size_t i = 0; i < buildOrder.size(); i++) remappedBuildOrder[i] = availableUnitTypes.getGeneItem(buildOrder[i]);

    vector<bool> partOfOriginalBuildOrder;
    BuildOrder finalBuildOrder = addImplicitBuildOrderSteps(remappedBuildOrder, startStateAfterEvents.race, startStateAfterEvents.foodAvailable(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, &partOfOriginalBuildOrder);
    return { finalBuildOrder, partOfOriginalBuildOrder };
}

// https://siderite.blogspot.com/2007/01/super-fast-string-distance-algorithm.html
float geneDistance(const BuildOrderGene& g1, const BuildOrderGene& g2) {
    int c = 0;
	int offset1 = 0;
	int offset2 = 0;
	int dist = 0;
    int l1 = g1.buildOrder.size();
    int l2 = g2.buildOrder.size();
    const int maxOffset = 3;
	while ((c + offset1 < l1) && (c + offset2 < l2)) {
        if (g1.buildOrder[c + offset1] != g2.buildOrder[c + offset2]) {
            offset1 = 0;
            offset2 = 0;
            bool found = false;
            for (int i = 0; i < maxOffset; i++) {
                if ((c + i < l1) && (g1.buildOrder[c + i] == g2.buildOrder[c])) {
                    if (i > 0) {
                        dist++;
                        offset1 = i;
                    }
                    found = true;
                    break;
                }
                if ((c + i < l2) && (g1.buildOrder[c] == g2.buildOrder[c + i])) {
                    if (i > 0) {
                        dist++;
                        offset2 = i;
                    }
                    found = true;
                    break;
                }
            }

		    if (!found) dist++;
        }
        c++;
    }
    float fDist = dist + (l1 - offset1 + l2 - offset2) / 2 - c;
    fDist /= max(1, max(l1, l2));
    return fDist;
}

BuildOrder findBestBuildOrderGenetic(const std::vector<std::pair<sc2::UNIT_TYPEID, int>>& startingUnits, const std::vector<std::pair<sc2::UNIT_TYPEID, int>>& target) {
    return findBestBuildOrderGenetic(BuildState(startingUnits), target, nullptr);
}

BuildOrder findBestBuildOrderGenetic(const BuildState& startState, const vector<pair<sc2::UNIT_TYPEID, int>>& target, const BuildOrder* seed, BuildOptimizerParams params) {
    auto target2 = vector<pair<BuildOrderItem, int>>(target.size());
    for (size_t i = 0; i < target.size(); i++) target2[i] = { BuildOrderItem(target[i].first), target[i].second };
    return findBestBuildOrderGenetic(startState, target2, seed, params).first;
}

// BuildOrder findBestBuildOrderGenetic(const BuildState& startState, const vector<pair<BuildOrderItem, int>>& target, const BuildOrder* seed) {
//     return findBestBuildOrderGenetic(startState, target, seed, BuildOptimizerParams()).first;
// }

/** Finds the best build order using an evolutionary algorithm */
pair<BuildOrder, BuildOrderFitness> findBestBuildOrderGenetic(const BuildState& startState, const vector<pair<BuildOrderItem, int>>& target, const BuildOrder* seed, BuildOptimizerParams params) {
    const AvailableUnitTypes& availableUnitTypes = getAvailableUnitsForRace(startState.race, UnitCategory::BuildOrderOptions);
    const AvailableUnitTypes& allEconomicUnits = getAvailableUnitsForRace(startState.race, UnitCategory::Economic);

    // Simulate the starting state until all current events have finished, only then do we know which exact unit types the player will start with.
    // This is important for implicit dependencies in the build order.
    // If say a factory is under construction, we don't want to implictly build another factory if the build order specifies that a tank is supposed to be built.
    BuildState startStateAfterEvents = startState;
    startStateAfterEvents.simulate(startStateAfterEvents.time + 1000000);

    vector<int> startingUnitCounts;
    vector<int> startingAddonCountPerUnitType;
    tie(startingUnitCounts, startingAddonCountPerUnitType) = calculateStartingUnitCounts(startStateAfterEvents, availableUnitTypes);

    vector<int> actionRequirements(availableUnitTypes.size());
    for (auto p : target) {
        int index = availableUnitTypes.getIndexMaybe(p.first.rawType());
        if (index != -1) {
            actionRequirements[index] += p.second;
        }
    }
    for (size_t i = 0; i < actionRequirements.size(); i++) {
        auto item = availableUnitTypes.getBuildOrderItem(i);
        if (item.isUnitType()) {
            UNIT_TYPEID type = item.typeID();
            for (auto p : startStateAfterEvents.units)
                if (p.type == type || getUnitData(p.type).unit_alias == type)
                    actionRequirements[i] -= p.units;
            actionRequirements[i] = max(0, actionRequirements[i]);
        } else {
            // Check if we already have the upgrade
            if (startStateAfterEvents.upgrades.hasUpgrade(item.upgradeID())) {
                actionRequirements[i] = 0;
            }
        }
    }

    vector<int> economicUnits;
    for (size_t i = 0; i < allEconomicUnits.size(); i++) {
        economicUnits.push_back(remapAvailableUnitIndex(i, allEconomicUnits, availableUnitTypes));
    }

    Stopwatch watch;
    float lastBestFitness = -100000000000;

    vector<BuildOrderGene> generation(params.genePoolSize);
    default_random_engine rnd(time(0));
    // default_random_engine rnd(rand());
    for (auto& gene : generation) {
        gene = BuildOrderGene(rnd, actionRequirements);
        gene.validate(actionRequirements);
    }
    for (int i = 0; i <= params.iterations; i++) {
        if (i == 150 && seed != nullptr) {
            // Add in the seed here
            generation[generation.size() - 1] = BuildOrderGene(*seed, availableUnitTypes, actionRequirements);
            generation[generation.size() - 1].validate(actionRequirements);
        }

        vector<BuildOrderFitness> fitness(generation.size());
        vector<int> indices;
        vector<BuildOrderGene> nextGeneration;
        
        if (params.varianceBias <= 0) {
            indices = vector<int>(generation.size());
            for (size_t j = 0; j < generation.size(); j++) {
                indices[j] = j;
                fitness[j] = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, generation[j]);
            }

            sortByValueDescending<int, float>(indices, [=](int index) { return -fitness[index].time; });
            sortByValueDescendingBubble<int, BuildOrderFitness>(indices, [=](int index) { return fitness[index]; });
            // Add the N best performing genes
            for (int j = 0; j < min(5, params.genePoolSize); j++) {
                nextGeneration.push_back(generation[indices[j]]);
            }
            // Add a random one as well
            nextGeneration.push_back(generation[uniform_int_distribution<int>(0, indices.size() - 1)(rnd)]);
        } else {
            for (size_t j = 0; j < generation.size(); j++) {
                fitness[j] = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, generation[j]);
            }

            // Add the N best performing genes
            for (int j = 0; j < min(5, params.genePoolSize); j++) {
                float bestScore = -100000000;
                int bestIndex = -1;
                for (size_t k = 0; k < generation.size(); k++) {
                    float score = fitness[k].score();
                    float minDistance = 1;
                    for (auto& g : nextGeneration) minDistance = min(minDistance, geneDistance(generation[k], g));

                    score -= fitness[k].time * (1 - minDistance) * params.varianceBias;

                    if (score > bestScore) {
                        bestScore = score;
                        bestIndex = k;
                    }
                }

                assert(bestIndex != -1);
                indices.push_back(bestIndex);
                nextGeneration.push_back(generation[bestIndex]);
            }
        }

        if ((i % 50) == 0 && i != 0) {
            for (auto& g : nextGeneration) {
                g.validate(actionRequirements);
                g = locallyOptimizeGene(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, actionRequirements, g);
                g.validate(actionRequirements);
            }

            // Expand build orders
            if (i > 150) {
                for (auto& g : nextGeneration) {
                    // float f1 = calculateFitness(startState, uniqueStartingUnits, availableUnitTypes, g);
                    auto order = g.constructBuildOrder(startState.race, startState.foodAvailableInFuture(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes);
                    // cout << "Order size " << order.size() << endl;
                    g.buildOrder.clear();
                    for (BuildOrderItem t : order.items)
                        g.buildOrder.push_back(availableUnitTypes.getGeneItem(t));
                    // float f2 = calculateFitness(startState, uniqueStartingUnits, availableUnitTypes, g);
                    // if (f1 != f2) {
                    //     cout << "Fitness don't match " << f1 << " " << f2 << endl;
                    // }
                }
            }
        }

        uniform_int_distribution<int> randomParentIndex(0, nextGeneration.size() - 1);
        while ((int)nextGeneration.size() < params.genePoolSize) {
            // nextGeneration.push_back(BuildOrderGene::crossover(generation[randomParentIndex(rnd)], generation[randomParentIndex(rnd)], rnd, actionRequirements));
            nextGeneration.push_back(generation[randomParentIndex(rnd)]);
        }

        // Note: do not mutate the first gene
        for (size_t i = 1; i < nextGeneration.size(); i++) {
            nextGeneration[i].mutateMove(params.mutationRateMove, actionRequirements, rnd);
            nextGeneration[i].mutateAddRemove(params.mutationRateAddRemove, rnd, actionRequirements, economicUnits, availableUnitTypes, params.allowChronoBoost);
        }

        swap(generation, nextGeneration);

        // if ((i % 10) == 0) cout << "Best fitness " << fitness[indices[0]] << endl;
        // calculateFitness(startState, uniqueStartingUnits, availableUnitTypes, generation[indices[0]]);

        
        // Note: locallyOptimizeGene *can* in some cases make the score worse.
        // In particular it always removes non-essential items at the end of the build order which can make it worse (this is kinda a bug though)
        // assert(lastBestFitness <= fitness[indices[0]].score());
        lastBestFitness = fitness[indices[0]].score();
    }
    
    generation[0] = locallyOptimizeGene(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, actionRequirements, generation[0]);

    if (false) {
        auto indices = vector<int>(generation.size());
        auto fitness = vector<BuildOrderFitness>(generation.size());
        for (int j = 0; j < generation.size(); j++) {
            indices[j] = j;
            fitness[j] = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, generation[j]);
        }

        sortByValueDescending<int, float>(indices, [=](int index) { return -fitness[index].time; });
        for (auto index : indices) {
            cout << index << " " << fitness[index].time << endl;
        }
        cout << endl;
        sortByValueDescendingBubble<int, BuildOrderFitness>(indices, [=](int index) { return fitness[index]; });
        for (auto index : indices) {
            cout << index << " " << fitness[index].time << endl;
        }
        
        miningSpeedFutureColor = 0;
        assert(indices[0] == 0);
        for (auto index : indices) {
            auto bo = generation[index].constructBuildOrder(startState.race, startState.foodAvailableInFuture(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes);
            printBuildOrderDetailed(startState, bo);
            miningSpeedFutureColor++;
        }
        pybind11::module::import("matplotlib.pyplot").attr("show")();
    }
    // cout << "Best fitness " << calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, generation[0]) << endl;
    // printBuildOrder(generation[0].constructBuildOrder(startState.foodAvailable(), startingUnitCounts, availableUnitTypes));
    // printBuildOrderDetailed(startState, generation[0].constructBuildOrder(startState.race, startState.foodAvailable(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes));
    auto fitness = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, generation[0]);

    // for(auto u : startState.units) {
    //     cout << "Starting unit " << UnitTypeToName(u.type) << " x" << u.units << endl;
    // }
    // cout << "Implicit build order" << endl;
    // for (auto u : generation[0].buildOrder) {
    //     cout << availableUnitTypes.getBuildOrderItem(u.type).name() << endl;
    // }
    // cout << endl;
    // cout << "Explicit build order" << endl;
    // for (auto u : generation[0].constructBuildOrder(startState.race, startState.foodAvailableInFuture(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes).items) {
    //     cout << u.name() << endl;
    // }

    /*stack<BuildOrderItem> reqs;
    traceDependencies(startingUnitCounts, availableUnitTypes, reqs, UNIT_TYPEID::PROTOSS_GATEWAY);
    cout << "Reqs " << reqs.size() << endl;
    while(!reqs.empty()) {
        auto u = reqs.top();
        reqs.pop();
        cout << "Req " << UnitTypeToName(u.typeID()) << endl;
    }*/
    return make_pair(generation[0].constructBuildOrder(startState.race, startState.foodAvailableInFuture(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes), fitness);
}

vector<UNIT_TYPEID> buildOrderProBO = {
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_NEXUS,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_ASSIMILATOR,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
    UNIT_TYPEID::PROTOSS_ASSIMILATOR,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ZEALOT,
    UNIT_TYPEID::PROTOSS_ROBOTICSBAY,
    UNIT_TYPEID::PROTOSS_ZEALOT,
    UNIT_TYPEID::PROTOSS_ZEALOT,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_ZEALOT,
    UNIT_TYPEID::PROTOSS_STALKER,
    UNIT_TYPEID::PROTOSS_STALKER,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ZEALOT,
    UNIT_TYPEID::PROTOSS_COLOSSUS,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_STALKER,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ZEALOT 3,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_PYLON,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ZEALOT 3,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_PYLON,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST STALKER,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_COLOSSUS,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ADEPT,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST COLLOSUS,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST COLLOSUS,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ADEPT,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ADEPT,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ADEPT,
};

vector<UNIT_TYPEID> buildOrderProBO2 = {
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_PROBE,
    UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_ASSIMILATOR,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_GATEWAY,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ASSIMILATOR,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ADEPT,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_PYLON,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    UNIT_TYPEID::PROTOSS_ADEPT,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ,
    // UNIT_TYPEID::PROTOSS_CHRONOBOOST ,
    UNIT_TYPEID::PROTOSS_ADEPT,
};

// http://www.proboengine.com/build1.html
BuildOrder buildOrderProBO3 = {
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ASSIMILATOR, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_NEXUS, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_CYBERNETICSCORE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UPGRADE_ID::WARPGATERESEARCH, true),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_CHRONOBOOST warpGateResearch, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ASSIMILATOR, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_TWILIGHTCOUNCIL, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, true),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_RESONATINGGLAIVES, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, true),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_WARPGATE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, true),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_CHRONOBOOST resonatingGlaives, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_WARPGATE, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_WARPGATE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_WARPGATE, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, true),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, true),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_CHRONOBOOST resonatingGlaives, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    // BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
    BuildOrderItem(UNIT_TYPEID::PROTOSS_ADEPT, false),
};

void unitTestBuildOptimizer() {

    assert(geneDistance(BuildOrderGene({ 1, 2, 3, 4 }), BuildOrderGene({ 1, 2, 3, 4 })) == 0);
    assert(geneDistance(BuildOrderGene({ 1, 2, 3, 4 }), BuildOrderGene({ 1, 3, 4 })) == 0.25f);
    assert(geneDistance(BuildOrderGene({ 1 }), BuildOrderGene({ 2 })) == 1);

    // for (int i = 0; i < unitTypesTerran.size(); i++) {
    //     cout << (int)unitTypesTerran[i] << ": " << i << ",  # " << UnitTypeToName(unitTypesTerran[i]) << endl;
    // }
    // exit(0);

    // assert(BuildState({{ UNIT_TYPEID::ZERG_HATCHERY, 1 }, { UNIT_TYPEID::ZERG_DRONE, 12 }}).simulateBuildOrder({ UNIT_TYPEID::ZERG_SPAWNINGPOOL, UNIT_TYPEID::ZERG_ZERGLING }));

    // findBestBuildOrderGenetic({ { UNIT_TYPEID::ZERG_HATCHERY, 1 }, { UNIT_TYPEID::ZERG_DRONE, 12 } }, { { UNIT_TYPEID::ZERG_HATCHERY, 1 }, { UNIT_TYPEID::ZERG_ZERGLING, 12 }, { UNIT_TYPEID::ZERG_MUTALISK, 20 }, { UNIT_TYPEID::ZERG_INFESTOR, 1 } });
    for (int i = 0; i < 0; i++) {
        // BuildState startState({ { UNIT_TYPEID::TERRAN_COMMANDCENTER, 1 }, { UNIT_TYPEID::TERRAN_SCV, 12 } });
        // startState.resources.minerals = 50;
        // startState.race = Race::Terran;

        // auto bo = findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::TERRAN_MARAUDER, 12 }, { UNIT_TYPEID::TERRAN_BANSHEE, 0 } });
        // printBuildOrderDetailed(startState, bo);
        // exit(0);

        // findBestBuildOrderGenetic({ { UNIT_TYPEID::TERRAN_COMMANDCENTER, 1 }, { UNIT_TYPEID::TERRAN_SCV, 12 } }, { { UNIT_TYPEID::TERRAN_BARRACKSREACTOR, 0 }, { UNIT_TYPEID::TERRAN_MARINE, 30 }, { UNIT_TYPEID::TERRAN_MARAUDER, 0 } });

        if (true) {
            BuildState startState({ { UNIT_TYPEID::PROTOSS_NEXUS, 1 }, { UNIT_TYPEID::PROTOSS_PROBE, 12 } });
            startState.resources.minerals = 50;
            startState.race = Race::Protoss;
            startState.chronoInfo.addNexusWithEnergy(startState.time, 50);
            // Initial delay before harvesters start mining properly
            startState.makeUnitsBusy(UNIT_TYPEID::PROTOSS_PROBE, UNIT_TYPEID::INVALID, 12);
            for (int i = 0; i < 12; i++) startState.addEvent(BuildEvent(BuildEventType::MakeUnitAvailable, 4, UNIT_TYPEID::PROTOSS_PROBE, ABILITY_ID::INVALID));

            startState.baseInfos = { BaseInfo(10800, 1000, 1000) };

            vector<UNIT_TYPEID> bo5 = {
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_OBSERVER,
                UNIT_TYPEID::PROTOSS_IMMORTAL,
                UNIT_TYPEID::PROTOSS_CARRIER,
            };

            vector<UNIT_TYPEID> bo6 = {
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_OBSERVER,
                UNIT_TYPEID::PROTOSS_CARRIER,
                UNIT_TYPEID::PROTOSS_IMMORTAL,
            };

            // findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::PROTOSS_ADEPT, 23 } });
            // findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::PROTOSS_ZEALOT, 20 }, { UNIT_TYPEID::PROTOSS_STALKER, 30 }, { UNIT_TYPEID::PROTOSS_ADEPT, 12 }, { UNIT_TYPEID::PROTOSS_COLOSSUS, 2 } });
            // auto bo = findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::PROTOSS_PHOENIX, 3 }, { UNIT_TYPEID::PROTOSS_ZEALOT, 15 }, { UNIT_TYPEID::PROTOSS_CARRIER, 1 }, { UNIT_TYPEID::PROTOSS_OBSERVER, 1 }, { UNIT_TYPEID::PROTOSS_IMMORTAL, 1 } });
            BuildOptimizerParams params;
            params.iterations = 1024;
            // auto boTuple = findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::PROTOSS_ADEPT, 23 }, { UNIT_TYPEID::PROTOSS_PROBE, 31 }, { UNIT_TYPEID::PROTOSS_NEXUS, 2 }, { UNIT_TYPEID::PROTOSS_GATEWAY, 4 } }, nullptr, params);
            auto bo = findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::PROTOSS_ADEPT, 23 } }, nullptr, params);
            printBuildOrderDetailed(startState, bo);

            
            printBuildOrderDetailed(startState, buildOrderProBO3);
            // for (auto& item : buildOrderProBO3.items) item.chronoBoosted = false;
            // printBuildOrderDetailed(startState, buildOrderProBO3);

            pybind11::module::import("matplotlib.pyplot").attr("show")();
            /*vector<UNIT_TYPEID> bo = {
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_OBSERVER,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_NEXUS,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_IMMORTAL,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_CARRIER,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
            };*/

            vector<UNIT_TYPEID> bo2 = {
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_OBSERVER,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_IMMORTAL,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_CARRIER,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
            };

            vector<UNIT_TYPEID> bo4 = {
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_OBSERVER,
                UNIT_TYPEID::PROTOSS_CARRIER,
                UNIT_TYPEID::PROTOSS_IMMORTAL,
            };

            vector<UNIT_TYPEID> bo7 = {
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_OBSERVER,
                UNIT_TYPEID::PROTOSS_CARRIER,
                UNIT_TYPEID::PROTOSS_IMMORTAL,
            };

            vector<UNIT_TYPEID> bo8 = {
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_NEXUS,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PROBE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ASSIMILATOR,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_CYBERNETICSCORE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PYLON,
                UNIT_TYPEID::PROTOSS_FLEETBEACON,
                UNIT_TYPEID::PROTOSS_ZEALOT,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_ROBOTICSFACILITY,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_NEXUS,
                UNIT_TYPEID::PROTOSS_CARRIER,
                UNIT_TYPEID::PROTOSS_STARGATE,
                UNIT_TYPEID::PROTOSS_GATEWAY,
                UNIT_TYPEID::PROTOSS_PHOENIX,
                UNIT_TYPEID::PROTOSS_IMMORTAL,
                UNIT_TYPEID::PROTOSS_PHOENIX,
            };

            // printBuildOrderDetailed(startState, bo);
            // printBuildOrderDetailed(startState, bo4);
            // printBuildOrderDetailed(startState, bo5);
            // printBuildOrderDetailed(startState, bo6);
            // printBuildOrderDetailed(startState, bo7);
            // printBuildOrderDetailed(startState, bo8);

            /*for (int i = 0; i < bo2.size(); i++) {
                auto bo3 = bo2;
                bo3.insert(bo3.begin() + i, UNIT_TYPEID::PROTOSS_PROBE);
                auto state2 = startState;
                bool s = state2.simulateBuildOrder(bo3);
                if (!s) {
                    bo3.insert(bo3.begin() + i, UNIT_TYPEID::PROTOSS_PYLON);
                    state2 = startState;
                    s = state2.simulateBuildOrder(bo3);
                }
                if (!s) continue;
                BuildOrderFitness f = BuildOrderFitness(state2.time, state2.resources, state2.miningSpeed());
                cout << "(" << i << ", " << state2.time << "," << f.score() << ")," << endl;
            }
            printBuildOrderDetailed(startState, bo);
            printBuildOrderDetailed(startState, bo2);*/
            // printBuildOrderDetailed(startState, buildOrderProBO);
        }
    }

    if (false) {
        // BuildState startState({ { UNIT_TYPEID::PROTOSS_NEXUS, 2 }, { UNIT_TYPEID::PROTOSS_PROBE, 22 }, { UNIT_TYPEID::PROTOSS_PYLON, 2 }, { UNIT_TYPEID::PROTOSS_ASSIMILATOR, 2 }, { UNIT_TYPEID::PROTOSS_GATEWAY, 2 }, { UNIT_TYPEID::PROTOSS_ADEPT, 1 } });
        // startState.resources.minerals = 362;
        // startState.resources.vespene = 808;
        // startState.chronoInfo.addNexusWithEnergy(startState.time, 25);
        BuildState startState({ { UNIT_TYPEID::PROTOSS_NEXUS, 1 }, { UNIT_TYPEID::PROTOSS_PROBE, 12 } });
        startState.resources.minerals = 50;
        startState.resources.vespene = 0;
        
        startState.race = Race::Protoss;
        startState.chronoInfo.addNexusWithEnergy(startState.time, 50);
        // Initial delay before harvesters start mining properly
        // startState.makeUnitsBusy(UNIT_TYPEID::PROTOSS_PROBE, UNIT_TYPEID::INVALID, 12);
        // for (int i = 0; i < 12; i++) startState.addEvent(BuildEvent(BuildEventType::MakeUnitAvailable, 4, UNIT_TYPEID::PROTOSS_PROBE, ABILITY_ID::INVALID));

        // startState.baseInfos = { BaseInfo(10800, 1000, 1000) };
        for (int i = 1; i < 5; i++) {
            BuildOptimizerParams params;
            params.iterations = i*100;
            auto bo = findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::PROTOSS_VOIDRAY, 1 } }, nullptr, params);
            printBuildOrderDetailed(startState, bo);
            // cout << "Build order score " << boTuple.second.score() << endl;
        }
        exit(0);

        BuildOrder primBo = {
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, true),
        };

        BuildOrder altBo = {
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            // BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            // BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, true),
        };

        printBuildOrderDetailed(startState, primBo);
        printBuildOrderDetailed(startState, altBo);
        
        // Start unit  PROTOSS_ADEPT 1
        // Start unit  PROTOSS_PROBE 22
        // Start unit  PROTOSS_NEXUS 2
        // Start unit  PROTOSS_ASSIMILATOR 2
        // Target unit  PROTOSS_ZEALOT 5
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
    }

    if (true) {
        BuildState startState({ { UNIT_TYPEID::PROTOSS_NEXUS, 1 }, { UNIT_TYPEID::PROTOSS_PROBE, 20 }, { UNIT_TYPEID::PROTOSS_CYBERNETICSCORE, 6 }, { UNIT_TYPEID::PROTOSS_GATEWAY, 3 } });
        startState.resources.minerals = 300;
        startState.resources.vespene = 200;
        startState.race = Race::Protoss;
        startState.chronoInfo.addNexusWithEnergy(startState.time, 50);
        // Initial delay before harvesters start mining properly
        // startState.makeUnitsBusy(UNIT_TYPEID::PROTOSS_PROBE, UNIT_TYPEID::INVALID, 12);
        // for (int i = 0; i < 12; i++) startState.addEvent(BuildEvent(BuildEventType::MakeUnitAvailable, 4, UNIT_TYPEID::PROTOSS_PROBE, ABILITY_ID::INVALID));

        // startState.baseInfos = { BaseInfo(10800, 1000, 1000) };
        BuildOptimizerParams params;
        params.iterations = 1024;
        auto bo = findBestBuildOrderGenetic(startState, { { UNIT_TYPEID::PROTOSS_VOIDRAY, 1 }, { UNIT_TYPEID::PROTOSS_ZEALOT, 5 } }, nullptr, params);
        printBuildOrderDetailed(startState, bo);
        // cout << "Build order score " << boTuple.second.score() << endl;

        BuildOrder primBo = {
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ASSIMILATOR, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ASSIMILATOR, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_CYBERNETICSCORE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_STARGATE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_VOIDRAY, true),
        };

        BuildOrder altBo = {
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PYLON, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            // BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            // BuildOrderItem(UNIT_TYPEID::PROTOSS_GATEWAY, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, true),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_PROBE, false),
            BuildOrderItem(UNIT_TYPEID::PROTOSS_ZEALOT, true),
        };

        // printBuildOrderDetailed(startState, primBo);
        // printBuildOrderDetailed(startState, altBo);
        
        // Start unit  PROTOSS_ADEPT 1
        // Start unit  PROTOSS_PROBE 22
        // Start unit  PROTOSS_NEXUS 2
        // Start unit  PROTOSS_ASSIMILATOR 2
        // Target unit  PROTOSS_ZEALOT 5
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
        // Additional cost for PROTOSS_GATEWAY 150 0
    }
    // optimizer.calculate_build_order(Race::Terran, { { UNIT_TYPEID::TERRAN_COMMANDCENTER, 1 }, { UNIT_TYPEID::TERRAN_SCV, 1 } }, { { UNIT_TYPEID::TERRAN_SCV, 1 } });
    // optimizer.calculate_build_order(Race::Terran, { { UNIT_TYPEID::TERRAN_COMMANDCENTER, 1 }, { UNIT_TYPEID::TERRAN_SCV, 1 } }, { { UNIT_TYPEID::TERRAN_SCV, 2 } });
    // optimizer.calculate_build_order(Race::Terran, { { UNIT_TYPEID::TERRAN_COMMANDCENTER, 1 }, { UNIT_TYPEID::TERRAN_SCV, 1 } }, { { UNIT_TYPEID::TERRAN_SCV, 5 } });
    // logBuildOrder(optimizer.calculate_build_order();
    // logBuildOrder(optimizer.calculate_build_order(Race::Terran, { { UNIT_TYPEID::TERRAN_COMMANDCENTER, 1 }, { UNIT_TYPEID::TERRAN_SCV, 12 } }, { { UNIT_TYPEID::TERRAN_MARINE, 5 } }));
}

bool BuildOrderFitness::operator<(const BuildOrderFitness& other) const {
    if(false) return score() < other.score();
    
    float t = time;
    float otherTime = other.time;
    const float weight = 0.5f;
    t -= weight * 0.5f * ((resources.minerals / (1 + miningSpeed.mineralsPerSecond)) + (resources.vespene / (1 + miningSpeed.vespenePerSecond)));
    otherTime -= weight * 0.5f * ((other.resources.minerals / (1 + other.miningSpeed.mineralsPerSecond)) + (other.resources.vespene / (1 + other.miningSpeed.vespenePerSecond)));

    if (otherTime < t) return !(other < *this);

    if (time >= ReallyBad.time) return false;

    float resourcesPerSecond = miningSpeed.mineralsPerSecond + miningSpeed.vespenePerSecond;
    float dt = otherTime - t;

    // Manual bias factor
    dt *= 10;

    float estimatedResourcesPerSecond = resourcesPerSecond + (miningSpeedPerSecond.mineralsPerSecond + miningSpeedPerSecond.vespenePerSecond) * dt;
    float otherResourcesPerSecond = other.miningSpeed.mineralsPerSecond + other.miningSpeed.vespenePerSecond;
    // MS_a + MSS_a * (t_b - t_a) > MS_b
    // MS_a + MSS_a * 0.5 * (t_b - t_a) > MS_b - MSS_b * 0.5 * (t_b - t_a)
    // (t_b - t_a) > 2*(MS_b - MS_a)/(MSS_a - MSS_b)

    // (t_b - t_a) > 2*(MS_b - MS_a)/(MSS_a - MSS_b)
    
    // t_a - MS_a/MSS_a < t_b - MS_b/MSS_b

    return estimatedResourcesPerSecond < otherResourcesPerSecond;
    // return time > other.time;
    // float s = -fmax(time, 2 * 60.0f);
    // s += ((resources.minerals + 2 * resources.vespene) + (miningSpeed.mineralsPerSecond + 2 * miningSpeed.vespenePerSecond) * 60) * 0.001f;
    // s = log(s) - time/400.0f;
    // return s;
}

void optimizeExistingBuildOrder(const ObservationInterface* observation, const std::vector<const sc2::Unit*>& ourUnits, const BuildState& startState, BuildOrderTracker& buildOrder, bool serialize) {
    const AvailableUnitTypes& availableUnitTypes = getAvailableUnitsForRace(startState.race, UnitCategory::BuildOrderOptions);
    const AvailableUnitTypes& allEconomicUnits = getAvailableUnitsForRace(startState.race, UnitCategory::Economic);

    auto doneItems = buildOrder.update(observation, ourUnits);

    // Simulate the starting state until all current events have finished, only then do we know which exact unit types the player will start with.
    // This is important for implicit dependencies in the build order.
    // If say a factory is under construction, we don't want to implictly build another factory if the build order specifies that a tank is supposed to be built.
    BuildState startStateAfterEvents = startState;
    startStateAfterEvents.simulate(startStateAfterEvents.time + 1000000);

    vector<int> startingUnitCounts;
    vector<int> startingAddonCountPerUnitType;
    tie(startingUnitCounts, startingAddonCountPerUnitType) = calculateStartingUnitCounts(startStateAfterEvents, availableUnitTypes);

    vector<int> economicUnits;
    for (size_t i = 0; i < allEconomicUnits.size(); i++) {
        // Do not allow more than 8 bases (all the workers start to eat up the supply cap)
        if (isTownHall(availableUnitTypes.getUnitType(i)) && startingUnitCounts[availableUnitTypes.getIndex(getTownHallForRace(startState.race))] >= 8) continue;
        // Do not allow more than 100 workers
        if (isBasicHarvester(availableUnitTypes.getUnitType(i)) && startingUnitCounts[availableUnitTypes.getIndex(getHarvesterUnitForRace(startState.race))] >= 100) continue;

        economicUnits.push_back(remapAvailableUnitIndex(i, allEconomicUnits, availableUnitTypes));
    }

    vector<int> actionRequirements(availableUnitTypes.size());
    BuildOrderGene gene;

    for (size_t i = 0; i < buildOrder.buildOrder.size(); i++) {
        if (!doneItems[i]) {
            auto item = buildOrder.buildOrder[i];
            gene.buildOrder.push_back(availableUnitTypes.getGeneItem(item));

            if (item.isUnitType() && (isStructure(item.typeID()) || isBasicHarvester(item.typeID()))) {
                // Not a goal item
            } else {
                // Important stuff
                actionRequirements[availableUnitTypes.getGeneItem(item).type]++;
            }
        }
    }

    auto originalBuildOrder = gene.constructBuildOrder(startState.race, startStateAfterEvents.foodAvailable(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes);

    if (serialize) {
        // BuildState buildOrderStartingState(Observation(), Unit::Alliance::Self, Race::Protoss, BuildResources(Observation()->GetMinerals(), Observation()->GetVespene()), 0);
        ofstream json("saved_buildorder_state.json");
        {
            cereal::JSONOutputArchive archive(json);
            archive(startState);
        }
        ofstream json2("saved_buildorder_gene.json");
        {
            cereal::JSONOutputArchive archive(json2);
            archive(originalBuildOrder);
        }
        ofstream json3("saved_buildorder_bo.json");
        {
            for (auto item : originalBuildOrder.items) {
                if (item.isUnitType()) {
                    json3 << "BuildOrderItem(UNIT_TYPEID::" << UnitTypeToName(item.typeID()) << ", " << (item.chronoBoosted ? "true" : "false") << ")," << endl;
                } else {
                    json3 << "BuildOrderItem(UPGRADE_ID::" << UpgradeIDToName(item.upgradeID()) << ", " << (item.chronoBoosted ? "true" : "false") << ")," << endl;
                }
            }
        }
        json3.close();
    }

    auto fitness = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, gene);

    default_random_engine rnd(rand());
    BuildOptimizerParams params;
    for (int i = 0; i < 2; i++) {
        int r = rand();
        BuildOrderGene newGene = gene;
        if (r % 3 == 0) newGene = locallyOptimizeGene(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, actionRequirements, newGene);
        if (r % 3 == 1) newGene.mutateMove(params.mutationRateMove, actionRequirements, rnd);
        if (r % 3 == 2) newGene.mutateAddRemove(params.mutationRateAddRemove, rnd, actionRequirements, economicUnits, availableUnitTypes, params.allowChronoBoost);

        auto newFitness = calculateFitness(startState, startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes, newGene);

        if (fitness < newFitness) {
            fitness = newFitness;
            gene = move(newGene);
        }
    }

    auto newBuildOrder = gene.constructBuildOrder(startState.race, startStateAfterEvents.foodAvailable(), startingUnitCounts, startingAddonCountPerUnitType, availableUnitTypes);

    if (newBuildOrder.size() != originalBuildOrder.size()) {
        cout << "Original build order" << endl;
        printBuildOrder(originalBuildOrder);
        cout << "New build order" << endl;
        printBuildOrder(newBuildOrder);
    }
    buildOrder.tweakBuildOrder(doneItems, newBuildOrder);
}
