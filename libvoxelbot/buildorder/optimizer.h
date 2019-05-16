#pragma once
#include <vector>
#include <cmath>
#include <functional>
#include "sc2api/sc2_interfaces.h"
#include <iostream>
#include <libvoxelbot/combat/simulator.h>

struct BuildState;
struct BuildOrderTracker;

struct BuildResources {
    float minerals;
    float vespene;

    BuildResources(float minerals, float vespene)
        : minerals(minerals), vespene(vespene) {}
};

struct MiningSpeed {
    float mineralsPerSecond;
    float vespenePerSecond;

    /** Simulate mining with the given speed for the given amount of time */
    void simulateMining (BuildState& state, float dt) const;

    inline bool operator== (const MiningSpeed& other) const {
        return mineralsPerSecond == other.mineralsPerSecond && vespenePerSecond == other.vespenePerSecond;
    }
};

struct BuildUnitInfo {
    /** Type of the unit */
    sc2::UNIT_TYPEID type;

    /** Optional addon attached to the unit (or UNIT_TYPEID::INVALID otherwise) */
    sc2::UNIT_TYPEID addon;

    /** Total number of units */
    int units;

    /** How many units out of the total that are busy right now.
     * E.g. with constructing a building, training a unit, etc.
     * Technically this is the number of casting slots for abilities rather than the number of units,
     * in case the building has a reactor the number of busy units can be up to two times the number of units because each unit can train two units at the same time.
     */
    int busyUnits;

    BuildUnitInfo()
        : type(sc2::UNIT_TYPEID::INVALID), addon(sc2::UNIT_TYPEID::INVALID), units(0), busyUnits(0) {}
    BuildUnitInfo(sc2::UNIT_TYPEID type, sc2::UNIT_TYPEID addon, int units)
        : type(type), addon(addon), units(units), busyUnits(0) {}

    /** Number of units that are available for casting abilities */
    inline int availableUnits() const {
        if (addon == sc2::UNIT_TYPEID::TERRAN_REACTOR) {
            return units - busyUnits / 2;
        } else {
            return units - busyUnits;
        }
    }
};

enum BuildEventType {
    FinishedUnit,
    SpawnLarva,
    MuleTimeout,
    MakeUnitAvailable,  // Un-busy unit
    FinishedUpgrade,
    WarpGateTransition,
};

struct BuildEvent {
    /** Type of event */
    BuildEventType type;
    /** The ability that is being cast */
    sc2::ABILITY_ID ability;
    /** Unit type of the caster of the ability */
    sc2::UNIT_TYPEID caster;
    /** Addon unit type of the caster of the ability (if any) */
    sc2::UNIT_TYPEID casterAddon;
    /** Time at which this event will happen */
    float time;
    float chronoEndTime = 0;

    BuildEvent(BuildEventType type, float time, sc2::UNIT_TYPEID caster, sc2::ABILITY_ID ability)
        : type(type), ability(ability), caster(caster), casterAddon(sc2::UNIT_TYPEID::INVALID), time(time) {}

    explicit BuildEvent() {}

    /** True if this event may have an impact on food or mining speed */
    bool impactsEconomy() const;

    /** Applies the effects of this event on the given state */
    void apply(BuildState& state) const;

    inline bool operator<(const BuildEvent& other) const {
        return time < other.time;
    }
};

struct GeneUnitType {
    int type = -1;
    bool chronoBoosted = false;

    GeneUnitType() {}
    GeneUnitType(int type) : type(type), chronoBoosted(false) {}
    GeneUnitType(int type, bool chronoBoosted) : type(type), chronoBoosted(chronoBoosted) {}

    bool operator==(const GeneUnitType& other) const {
        return type == other.type && chronoBoosted == other.chronoBoosted;
    }

    bool operator!=(const GeneUnitType& other) const {
        return type != other.type || chronoBoosted != other.chronoBoosted;
    }
};

static const int UPGRADE_ID_OFFSET = 1000000;
struct BuildOrderItem {
  public:
    // union ish. It is assumed that unit type ids are pretty small (which they are)
    sc2::UNIT_TYPEID internalType = sc2::UNIT_TYPEID::INVALID;
    bool chronoBoosted = false;

    bool isUnitType() const {
        return (int)internalType < UPGRADE_ID_OFFSET;
    }

    sc2::UNIT_TYPEID rawType() const {
        return internalType;
    }

    sc2::UNIT_TYPEID typeID() const {
        assert((int)internalType < UPGRADE_ID_OFFSET);
        return internalType;
    }

    sc2::UPGRADE_ID upgradeID() const {
        return (sc2::UPGRADE_ID)((int)internalType - UPGRADE_ID_OFFSET);
    }

    BuildOrderItem() {}
    explicit BuildOrderItem(sc2::UPGRADE_ID upgrade, bool chronoBoosted = false) : internalType((sc2::UNIT_TYPEID)((int)upgrade + UPGRADE_ID_OFFSET)), chronoBoosted(chronoBoosted) {}
    explicit BuildOrderItem(sc2::UNIT_TYPEID type) : internalType(type), chronoBoosted(false) {}
    explicit BuildOrderItem(sc2::UNIT_TYPEID type, bool chronoBoosted) : internalType(type), chronoBoosted(chronoBoosted) {}

    bool operator==(const BuildOrderItem& other) const {
        return internalType == other.internalType && chronoBoosted == other.chronoBoosted;
    }

    bool operator!=(const BuildOrderItem& other) const {
        return internalType != other.internalType || chronoBoosted != other.chronoBoosted;
    }

    std::string name() const {
        if (isUnitType()) return sc2::UnitTypeToName(typeID());
        else return sc2::UpgradeIDToName(upgradeID());
    }
};

struct BuildOrder {
    std::vector<BuildOrderItem> items;

    BuildOrder() {}

    BuildOrder(std::initializer_list<BuildOrderItem> order) {
        for (auto tp : order) {
            items.push_back(tp);
        }
    }
    
    BuildOrder(std::initializer_list<sc2::UNIT_TYPEID> order) {
        for (auto tp : order) {
            items.push_back(BuildOrderItem(tp));
        }
    }

    explicit BuildOrder (const std::vector<sc2::UNIT_TYPEID>& order) : items(order.size()) {
        for (size_t i = 0; i < order.size(); i++) {
            items[i] = BuildOrderItem(order[i]);
        }
    }

    inline size_t size() const noexcept {
        return items.size();
    }

    inline BuildOrderItem& operator[] (int index) {
        return items[index];
    }

    inline const BuildOrderItem& operator[] (int index) const {
        return items[index];
    }
};

struct BuildOrderFitness {
    static const BuildOrderFitness ReallyBad;

    float time;
    BuildResources resources;
    MiningSpeed miningSpeed;
    MiningSpeed miningSpeedPerSecond;

    BuildOrderFitness () : time(0), resources(0,0), miningSpeed({0,0}), miningSpeedPerSecond({0, 0}) {}
    BuildOrderFitness (float time, BuildResources resources, MiningSpeed miningSpeed, MiningSpeed miningSpeedPerSecond) : time(time), resources(resources), miningSpeed(miningSpeed), miningSpeedPerSecond(miningSpeedPerSecond) {}

    float score() const;

    bool operator<(const BuildOrderFitness& other) const;
};

struct BaseInfo {
    float remainingMinerals = 0;
    float remainingVespene1 = 0;
    float remainingVespene2 = 0;

    explicit BaseInfo (float minerals, float vespene1, float vespene2) : remainingMinerals(minerals), remainingVespene1(vespene1), remainingVespene2(vespene2) {}
    explicit BaseInfo() {}

    inline void mineMinerals(float amount) {
        remainingMinerals = fmax(0.0f, remainingMinerals - amount);
    }

    /** Returns (high yield, low yield) mineral slots on this expansion */
    inline std::pair<int,int> mineralSlots () const {
        // Max is 10800 for an expansion with 8 patches
        if (remainingMinerals > 4800) return {16, 8};
        if (remainingMinerals > 4000) return {12, 6};
        if (remainingMinerals > 100) return {8, 4};
        if (remainingMinerals > 0) return {2, 1};
        return {0, 0};
    }
};

struct BuildOrderState {
    std::shared_ptr<const BuildOrder> buildOrder;
    int buildIndex = 0;
    sc2::UNIT_TYPEID lastChronoUnit = sc2::UNIT_TYPEID::INVALID;

    BuildOrderState (std::shared_ptr<const BuildOrder> buildOrder) : buildOrder(buildOrder) {}
};

const float NexusEnergyPerSecond = 1;
const float ChronoBoostEnergy = 50;
const float NexusInitialEnergy = 50;
const float ChronoBoostDuration = 20;


struct ChronoBoostInfo {
    std::vector<float> energyOffsets;
    std::vector<std::pair<sc2::UNIT_TYPEID, float>> chronoEndTimes;

    std::pair<bool, float> getChronoBoostEndTime(sc2::UNIT_TYPEID caster, float currentTime);

    std::pair<bool, float> useChronoBoost(float time);

    void addRemainingChronoBoost(sc2::UNIT_TYPEID caster, float endTime) {
        chronoEndTimes.emplace_back(caster, endTime);
    }

    void addNexusWithEnergy(float currentTime, float energy) {
        energyOffsets.push_back(energy - currentTime * NexusEnergyPerSecond);
    }
};

/** Represents all units, buildings and current build/train actions that are in progress for a given player */
struct BuildState {
    /** Time in game time seconds at normal speed */
    float time = 0;
    /** Race of the player */
    sc2::Race race = sc2::Race::Terran;

    /** All units in the current state */
    std::vector<BuildUnitInfo> units;
    /** All future events, sorted in ascending order by their time */
    std::vector<BuildEvent> events;
    /** Current resources */
    BuildResources resources = BuildResources(0,0);
    /** Metadata (in particular resource info) about the bases that the player has */
    std::vector<BaseInfo> baseInfos;

    ChronoBoostInfo chronoInfo;
    CombatUpgrades upgrades;

private:
    mutable uint64_t cachedHash = 0;
public:

    BuildState() {}
    explicit BuildState(std::vector<std::pair<sc2::UNIT_TYPEID, int>> unitCounts);

    explicit BuildState(const sc2::ObservationInterface* observation, sc2::Unit::Alliance alliance, sc2::Race race, BuildResources resources, float time);

    uint64_t hash() const {
        cachedHash = 0;
        return immutableHash();
    }

        void recalculateHash() {
        cachedHash = 0;
        immutableHash();
    }

    void transitionToWarpgates (const std::function<void(const BuildEvent&)>* eventCallback);

    /** Returns the hash of the build state.
     * Note: Assumes the object is immutable, the hash is cached the first time the method is called
     */
    uint64_t immutableHash() const {
        if (cachedHash == 0) {
            uint64_t h = 0;
            h = h*31 ^ (int)(time * 1000);
            h = h*31 ^ (int)(resources.minerals * 1000);
            h = h*31 ^ (int)(resources.vespene * 1000);
            h = h*31 ^ (int)(race);
            for (auto& u : units) {
                h = h*31 ^ (int)u.type;
                h = h*31 ^ (int)u.addon;
                h = h*31 ^ (int)u.busyUnits;
                h = h*31 ^ (int)u.units;
            }
            for (auto& s : baseInfos) {
                h = h*31 ^ ((int)s.remainingMinerals);
                h = h*31 ^ ((int)s.remainingVespene1);
                h = h*31 ^ ((int)s.remainingVespene2);
            }
            for (auto& e : events) {
                h = h*31 ^ ((int)e.ability);
                h = h*31 ^ ((int)e.caster);
                h = h*31 ^ ((int)e.casterAddon);
                h = h*31 ^ ((int)e.time);
                h = h*31 ^ ((int)e.type); // Type is really implied by the ability, but oh well
            }
            cachedHash = h;
        }
        return cachedHash;
    }

    /** Marks a number of units with the given type (and optionally addon) as being busy.
     * Delta may be negative to indicate that the units should be made available again after having been busy.
     * 
     * If this action could not be performed (e.g. there were no non-busy units that could be made busy) then the function will panic.
     */
    void makeUnitsBusy(sc2::UNIT_TYPEID type, sc2::UNIT_TYPEID addon, int delta);

    void addUnits(sc2::UNIT_TYPEID type, int delta);

    /** Adds a number of given unit type (with optional addon) to the state.
     * Delta may be negative to indicate that units should be removed.
     * 
     * If this action could not be performed (e.g. there were no units that could be removed) then this function will panic.
     */
    void addUnits(sc2::UNIT_TYPEID type, sc2::UNIT_TYPEID addon, int delta);
    void killUnits(sc2::UNIT_TYPEID type, sc2::UNIT_TYPEID addon, int count);

    /** Returns the current mining speed of (minerals,vespene gas) per second (at normal game speed) */
    MiningSpeed miningSpeed(bool debug=false) const;

    /** Returns the time it will take to get the specified resources using the given mining speed */
    float timeToGetResources(MiningSpeed miningSpeed, float mineralCost, float vespeneCost) const;

    /** Adds a new future event to the state */
    void addEvent(BuildEvent event);

    /** Simulate the state until a given point in time.
     * All actions up to and including the end time will have been completed after the function has been called.
     * This will update the current resources using the simulated mining speed.
     */
    void simulate(float endTime, const std::function<void(const BuildEvent&)>* eventCallback = nullptr);

    /** Simulates the execution of a given build order.
     * The function will return false if the build order could not be performed.
     * The resulting state is the state right after the final item in the build order has been completed.
     * 
     * The optional callback will be called once exactly when build order item number N is executed, with N as the parameter.
     * Note that the this is when the item starts to be executed, not when the item is finished.
     * The callback is called right after the action has been executed, but not necessarily completed.
     */
    bool simulateBuildOrder(const BuildOrder& buildOrder, const std::function<void(int)> = nullptr, bool waitUntilItemsFinished = true);
    bool simulateBuildOrder(BuildOrderState& buildOrder, const std::function<void(int)> callback, bool waitUntilItemsFinished, float maxTime = std::numeric_limits<float>::infinity(), const std::function<void(const BuildEvent&)>* eventCallback = nullptr);

    float foodCap() const;

    /** Food that is currently available.
     * Positive if there is a surplus of food.
     * Note that food is a floating point number, zerglings in particular use 0.5 food.
     * It is still safe to work with floating point numbers because they can exactly represent whole numbers and whole numbers + 0.5 exactly up to very large values.
     */
    float foodAvailable() const;

    /** Food that will be available when following the currents event into the future */
    float foodAvailableInFuture() const;

    /** True if the state contains the given unit type or which is equivalent to the given unit type for tech purposes */
    bool hasEquivalentTech(sc2::UNIT_TYPEID type) const;
};

struct BuildOptimizerParams {
    int genePoolSize = 25;
    int iterations = 512;
    float mutationRateAddRemove = 0.05f;
    float mutationRateMove = 0.025f;
    float varianceBias = 0;
    bool allowChronoBoost = true;
};

std::pair<BuildOrder, std::vector<bool>> expandBuildOrderWithImplicitSteps (const BuildState& startState, BuildOrder buildOrder);

BuildOrder findBestBuildOrderGenetic(const std::vector<std::pair<sc2::UNIT_TYPEID, int>>& startingUnits, const std::vector<std::pair<sc2::UNIT_TYPEID, int>>& target);
BuildOrder findBestBuildOrderGenetic(const BuildState& startState, const std::vector<std::pair<sc2::UNIT_TYPEID, int>>& target, const BuildOrder* seed = nullptr, BuildOptimizerParams params = BuildOptimizerParams());
std::pair<BuildOrder, BuildOrderFitness> findBestBuildOrderGenetic(const BuildState& startState, const std::vector<std::pair<BuildOrderItem, int>>& target, const BuildOrder* seed = nullptr, BuildOptimizerParams params = BuildOptimizerParams());
void unitTestBuildOptimizer();
void printBuildOrderDetailed(const BuildState& startState, const BuildOrder& buildOrder, const std::vector<bool>* highlight = nullptr);
void optimizeExistingBuildOrder(const sc2::ObservationInterface* observation, const std::vector<const sc2::Unit*>& ourUnits, const BuildState& buildOrderStartingState, BuildOrderTracker& buildOrder, bool serialize);
BuildOrderFitness calculateFitness(const BuildState& startState, const BuildOrder& buildOrder);