#pragma once
#include "sc2api/sc2_interfaces.h"
#include <libvoxelbot/buildorder/build_time_estimator.h>
#include <libvoxelbot/utilities/stdutils.h>
#include <limits>
#include <vector>

struct AvailableUnitTypes;
struct BuildState;

inline bool canBeAttackedByAirWeapons(sc2::UNIT_TYPEID type) {
    return isFlying(type) || type == sc2::UNIT_TYPEID::PROTOSS_COLOSSUS;
}

struct CombatUnit {
	int owner;
	sc2::UNIT_TYPEID type;
	float health;
	float health_max;
	float shield;
	float shield_max;
	float energy;
	bool is_flying;
	float buffTimer = 0;
	void modifyHealth(float delta);

	CombatUnit() {}
	CombatUnit (int owner, sc2::UNIT_TYPEID type, int health, bool flying) : owner(owner), type(type), health(health), health_max(health), shield(0), shield_max(0), energy(50), is_flying(flying) {}
	CombatUnit(const sc2::Unit& unit) : owner(unit.owner), type(unit.unit_type), health(unit.health), health_max(unit.health_max), shield(unit.shield), shield_max(unit.shield_max), energy(unit.energy), is_flying(unit.is_flying) {}
};

struct CombatUpgrades {
	std::bitset<90> upgrades;

	struct iterator {
		const CombatUpgrades& parent;
		size_t index = 0;
		iterator(const CombatUpgrades& parent, size_t index): parent(parent), index(index) {
			while(this->index < parent.upgrades.size() && !parent.upgrades[this->index]) this->index++;
		}
        iterator operator++() {
			index++;
			while(index < parent.upgrades.size() && !parent.upgrades[index]) index++;
			return *this;
		}
        bool operator!=(const iterator & other) const {
			return index != other.index;
		}
        sc2::UPGRADE_ID operator*() const;
	};

	CombatUpgrades();

	bool hasUpgrade(sc2::UPGRADE_ID upgrade) const;

	uint64_t hash() const {
		return std::hash<std::bitset<90>>()(upgrades);
		// uint64_t h = 0;
		// for (auto u : upgrades) {
		// 	h = h * 31 ^ (uint64_t)u;
		// }
		// return h;
	}

	void add(sc2::UPGRADE_ID upgrade);

	void combine(const CombatUpgrades& other) {
		upgrades |= other.upgrades;
	}

	void remove(const CombatUpgrades& other) {
		upgrades &= ~other.upgrades;
	}

	iterator begin() const { return iterator(*this, 0 ); }
	iterator end() const { return iterator(*this, upgrades.size() ); }
};

struct CombatEnvironment;

struct CombatState {
	std::vector<CombatUnit> units;
	const CombatEnvironment* environment = nullptr;

	// Owner with the highest total health summed over all units
	int owner_with_best_outcome() const;
	std::string str();
};

struct CombatResult {
	float time = 0;
	std::array<float, 2> averageHealthTime = {{ 0, 0 }};
	CombatState state;
};

struct CombatRecordingFrame {
    int tick;
    std::vector<std::tuple<sc2::UNIT_TYPEID, int, float>> healths;
    void add(sc2::UNIT_TYPEID type, int owner, float health, float shield);
};

struct CombatRecording {
	std::vector<CombatRecordingFrame> frames;
	void writeCSV(std::string filename);
};

struct WeaponInfo {
   private:
    mutable std::vector<float> dpsCache;
    float baseDPS;

   public:
    bool available;
    float splash;
    const sc2::Weapon* weapon;

    float getDPS() const {
        return baseDPS;
    }

    float getDPS(sc2::UNIT_TYPEID target, float modifier = 0) const;

    float range() const;

    WeaponInfo()
        : baseDPS(0), available(false), splash(0), weapon(nullptr) {
    }

    WeaponInfo(const sc2::Weapon* weapon, sc2::UNIT_TYPEID type, const CombatUpgrades& upgrades, const CombatUpgrades& targetUpgrades);
};

struct CombatSettings {
    bool badMicro = false;
	bool debug = false;
    bool enableSplash = true;
    bool enableTimingAdjustment = true;
    bool enableSurroundLimits = true;
    bool enableMeleeBlocking = true;
	bool workersDoNoDamage = false;
	bool assumeReasonablePositioning = true;
	float maxTime = std::numeric_limits<float>::infinity();
	float startTime = 0;
};

struct UnitCombatInfo {
    WeaponInfo groundWeapon;
    WeaponInfo airWeapon;

	// In case the unit has multiple weapons this is the fastest of the two weapons
	float attackInterval() const;
	
    UnitCombatInfo(sc2::UNIT_TYPEID type, const CombatUpgrades& upgrades, const CombatUpgrades& targetUpgrades);
};

struct CombatEnvironment {
	std::array<std::vector<UnitCombatInfo>, 2> combatInfo;
	std::array<CombatUpgrades, 2> upgrades;

	CombatEnvironment(const CombatUpgrades& upgrades, const CombatUpgrades& targetUpgrades);

	float attackRange(int owner, sc2::UNIT_TYPEID type) const;
	float attackRange(const CombatUnit& unit) const;
	const UnitCombatInfo& getCombatInfo(const CombatUnit& unit) const;
	float calculateDPS(int owner, sc2::UNIT_TYPEID type, bool air) const;
	float calculateDPS(const std::vector<CombatUnit>& units, bool air) const;
	float calculateDPS(const CombatUnit& unit, bool air) const;
	float calculateDPS(const CombatUnit& unit1, const CombatUnit& unit2) const;
};

struct CombatPredictor {
private:
	mutable std::map<uint64_t, CombatEnvironment> combatEnvironments;
public:
	CombatEnvironment defaultCombatEnvironment;
	CombatPredictor();
	void init();
	CombatResult predict_engage(const CombatState& state, bool debug=false, bool badMicro=false, CombatRecording* recording=nullptr, int defenderPlayer = 1) const;
	CombatResult predict_engage(const CombatState& state, CombatSettings settings, CombatRecording* recording=nullptr, int defenderPlayer = 1) const;
	void unitTest(const BuildOptimizerNN& buildTimePredictor) const;

	const CombatEnvironment& getCombatEnvironment(const CombatUpgrades& upgrades, const CombatUpgrades& targetUpgrades) const;

	const CombatEnvironment& combineCombatEnvironment(const CombatEnvironment* env, const CombatUpgrades& upgrades, int upgradesOwner) const;

	float targetScore(const CombatUnit& unit, bool hasGround, bool hasAir) const;

	float mineralScore(const CombatState& initialState, const CombatResult& combatResult, int player, const std::vector<float>& timeToProduceUnits, const CombatUpgrades upgrades) const;
	float mineralScoreFixedTime(const CombatState& initialState, const CombatResult& combatResult, int player, const std::vector<float>& timeToProduceUnits, const CombatUpgrades upgrades) const;
};

CombatUnit makeUnit(int owner, sc2::UNIT_TYPEID type);

extern const std::vector<sc2::UNIT_TYPEID> availableUnitTypesTerran;
extern const std::vector<sc2::UNIT_TYPEID> availableUnitTypesProtoss;

struct ArmyComposition {
    std::vector<std::pair<sc2::UNIT_TYPEID,int>> unitCounts;
    CombatUpgrades upgrades;

	void combine(const ArmyComposition& other) {
		for (auto u : other.unitCounts) {
			bool found = false;
			for (auto& u2 : unitCounts) {
				if (u2.first == u.first) {
					u2 = { u2.first, u2.second + u.second };
					found = true;
					break;
				}
			}

			if (!found) unitCounts.push_back(u);
		}

		upgrades.combine(other.upgrades);
	}
};

struct CompositionSearchSettings {
	const CombatPredictor& combatPredictor;
	const AvailableUnitTypes& availableUnitTypes;
	const BuildOptimizerNN* buildTimePredictor = nullptr;
	float availableTime = 4 * 60;

	CompositionSearchSettings(const CombatPredictor& combatPredictor, const AvailableUnitTypes& availableUnitTypes, const BuildOptimizerNN* buildTimePredictor = nullptr) : combatPredictor(combatPredictor), availableUnitTypes(availableUnitTypes), buildTimePredictor(buildTimePredictor) {}
};

ArmyComposition findBestCompositionGenetic(const CombatPredictor& predictor, const AvailableUnitTypes& availableUnitTypes, const CombatState& opponent, const BuildOptimizerNN* buildTimePredictor = nullptr, const BuildState* startingBuildState = nullptr, std::vector<std::pair<sc2::UNIT_TYPEID,int>>* seedComposition = nullptr);
ArmyComposition findBestCompositionGenetic(const CombatState& opponent, CompositionSearchSettings settings, const BuildState* startingBuildState = nullptr, std::vector<std::pair<sc2::UNIT_TYPEID,int>>* seedComposition = nullptr);

struct CombatRecorder {
private:
	std::vector<std::pair<float, std::vector<sc2::Unit>>> frames;
public:
	void tick(const sc2::ObservationInterface* observation);
	void finalize(std::string filename="recording.csv");
};


void logRecordings(CombatState& state, const CombatPredictor& predictor, float spawnOffset = 0, std::string msg = "recording");
float timeToBeAbleToAttack (const CombatEnvironment& env, CombatUnit& unit, float distanceToEnemy);