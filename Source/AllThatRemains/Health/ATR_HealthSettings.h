// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ATR_HealthTypes.h"
#include "ATR_HealthSettings.generated.h"

class UATR_BodyRegionDefinition;
class UATR_DamageTypeDefinition;
class UATR_ConditionDefinition;
class UATR_WeaponDamageProfile;

// Project Settings > AllThatRemains > Health & Survival
//
// All health/survival tunables live here — global rates, fatal thresholds,
// tick frequencies, Echo structural rules, replication limits, debug toggles.
// C++ reads via GetDefault<UATR_HealthSettings>(); nothing is hardcoded in
// simulation code.
//
// Content-shaped data (weapons, diseases, substances, treatments, armor,
// per-region/per-damage-type behavior) lives in DataAssets; the Health|Data
// category points at the table assets.
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Health & Survival"))
class ALLTHATREMAINS_API UATR_HealthSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName("AllThatRemains"); }

	// Defensive clamp of all settings to required invariants. Called from
	// PostInitProperties at CDO load (once) and from PostEditChangeProperty
	// on in-editor edits. Runtime callers do not need to re-clamp.
	void ValidateAndClamp();

	virtual void PostInitProperties() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ── Health|Data ────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Health|Data", meta=(
		ToolTip="Per-body-region tuning table (bleed/pain scales, organ damage transfer, catastrophic thresholds). Leave empty to use code defaults."))
	TSoftObjectPtr<UATR_BodyRegionDefinition> BodyRegionDefinition;

	UPROPERTY(Config, EditAnywhere, Category="Health|Data", meta=(
		ToolTip="Per-damage-type wound behavior table (bleed/pain/structural/depth/infection factors). Leave empty to use code defaults."))
	TSoftObjectPtr<UATR_DamageTypeDefinition> DamageTypeDefinition;

	UPROPERTY(Config, EditAnywhere, Category="Health|Data", meta=(
		ToolTip="Per-condition tuning table (display, progression, pain). Leave empty to use code defaults."))
	TSoftObjectPtr<UATR_ConditionDefinition> ConditionDefinition;

	// ── Health|Ticks ───────────────────────────────────────────────────────
	// Never tick every wound every frame. Three biological buckets:

	UPROPERTY(Config, EditAnywhere, Category="Health|Ticks", meta=(ClampMin="0.5", ClampMax="10.0",
		ToolTip="Fast bucket Hz: bleeding, blood pressure, oxygenation, shock, consciousness, death checks."))
	float FastTickHz = 2.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Ticks", meta=(ClampMin="0.1", ClampMax="2.0",
		ToolTip="Medium bucket Hz: pain aggregation, fatigue, body temperature during exposure."))
	float MediumTickHz = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Ticks", meta=(ClampMin="1.0", ClampMax="300.0",
		ToolTip="Slow bucket period in seconds: wound lifecycle, infection, disease, substances, healing, survival drains."))
	float SlowTickSeconds = 30.f;

	// ── Health|Fatal Thresholds ────────────────────────────────────────────
	// Death is cause-based. Each path has a threshold; sustained paths also
	// have a grace period the vital must stay collapsed for before death.

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="0.0", ClampMax="0.5",
		ToolTip="BrainFunction at/below this is immediately fatal (BrainFailure)."))
	float FatalBrainFunction = 0.05f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="0.0", ClampMax="0.9",
		ToolTip="BloodPressure at/below this starts the circulatory-collapse death timer."))
	float CollapseBloodPressure = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="1.0", ClampMax="600.0",
		ToolTip="Seconds of sustained circulatory collapse before death (CirculatoryCollapse)."))
	float CirculatoryGraceSeconds = 90.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="0.0", ClampMax="0.9",
		ToolTip="Oxygenation at/below this starts the hypoxia death timer."))
	float CollapseOxygenation = 0.2f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="1.0", ClampMax="600.0",
		ToolTip="Seconds of sustained oxygen collapse before death (Hypoxia). Brain damage accrues during it."))
	float HypoxiaGraceSeconds = 180.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="0.0", ClampMax="0.5",
		ToolTip="HeartFunction at/below this is immediately fatal (CardiacArrest)."))
	float FatalHeartFunction = 0.05f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="15.0", ClampMax="35.0",
		ToolTip="Core temperature (C) at/below this starts the hypothermia death timer."))
	float FatalCoreTempLowC = 26.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="38.0", ClampMax="46.0",
		ToolTip="Core temperature (C) at/above this starts the hyperthermia death timer."))
	float FatalCoreTempHighC = 43.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="1.0", ClampMax="1800.0",
		ToolTip="Seconds outside the fatal temperature range before death."))
	float TemperatureGraceSeconds = 300.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="0.5", ClampMax="1.0",
		ToolTip="Systemic Infection01 at/above this is fatal (Sepsis)."))
	float FatalInfection = 0.95f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Fatal", meta=(ClampMin="0.5", ClampMax="1.0",
		ToolTip="Toxicity01 at/above this is fatal (Toxicity)."))
	float FatalToxicity = 0.95f;

	// ── Health|Bleeding ────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Health|Bleeding", meta=(ClampMin="0.0",
		ToolTip="Natural clotting: fraction of a wound's bleed rate removed per second for shallow wounds (deep wounds clot slower)."))
	float ClotRatePerSecond = 0.002f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Bleeding", meta=(ClampMin="0.0", ClampMax="3600.0",
		ToolTip="Seconds a tourniquet can stay on before tissue damage starts accruing on the wound."))
	float TourniquetSafeSeconds = 600.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Bleeding", meta=(ClampMin="0.0",
		ToolTip="StructuralDamage01 per second added to a tourniquet wound past the safe window."))
	float TourniquetTissueDamageRate = 0.0005f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Bleeding", meta=(ClampMin="0.0",
		ToolTip="BloodVolume01 regenerated per second when hydrated and fed (scaled down by poor survival stats)."))
	float BloodRegenPerSecond = 0.00002f;

	// ── Health|Pain & Shock ────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Health|PainShock", meta=(ClampMin="0.0",
		ToolTip="Aggregate pain decay per second toward the wound/condition pain floor."))
	float PainDecayPerSecond = 0.01f;

	UPROPERTY(Config, EditAnywhere, Category="Health|PainShock", meta=(ClampMin="0.0",
		ToolTip="Shock01 rise per second per unit of blood-volume deficit."))
	float ShockFromBloodLossRate = 0.02f;

	UPROPERTY(Config, EditAnywhere, Category="Health|PainShock", meta=(ClampMin="0.0",
		ToolTip="Shock01 rise per second per unit of pain above tolerance."))
	float ShockFromPainRate = 0.005f;

	UPROPERTY(Config, EditAnywhere, Category="Health|PainShock", meta=(ClampMin="0.0",
		ToolTip="Shock01 decay per second while stable (no bleeding, pain controlled)."))
	float ShockDecayPerSecond = 0.002f;

	// ── Health|Oxygen & Consciousness ──────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Health|Oxygen", meta=(ClampMin="0.0",
		ToolTip="Oxygenation recovery per second toward the lung/pressure-derived ceiling."))
	float OxygenRecoveryPerSecond = 0.05f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Oxygen", meta=(ClampMin="0.0",
		ToolTip="BrainFunction01 lost per second while oxygenation is collapsed (hypoxic brain damage)."))
	float HypoxiaBrainDamagePerSecond = 0.001f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Oxygen", meta=(ClampMin="0.0", ClampMax="0.5",
		ToolTip="Consciousness01 at/below this the human is unconscious (condition marker + capability zeroed)."))
	float UnconsciousThreshold = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Oxygen", meta=(ClampMin="0.0", ClampMax="0.9",
		ToolTip="Consciousness01 must recover to this before waking (hysteresis so victims don't flicker awake)."))
	float WakeThreshold = 0.35f;

	// ── Health|Temperature ─────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Health|Temperature", meta=(ClampMin="-50.0", ClampMax="50.0",
		ToolTip="Air temperature (C) at which an unclothed, dry, resting body holds 37C."))
	float NeutralAirTempC = 24.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Temperature", meta=(ClampMin="0.0",
		ToolTip="Core temp drift (C per second) per degree of effective environment difference. Insulation/shelter scale it down."))
	float TempDriftPerDegreePerSecond = 0.0004f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Temperature", meta=(ClampMin="0.0", ClampMax="5.0",
		ToolTip="How strongly wetness amplifies cold (1 = wet doubles effective cold difference)."))
	float WetnessChillScale = 1.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Temperature", meta=(ClampMin="0.0", ClampMax="6.0",
		ToolTip="Max core-temp offset (C) added by full Fever01."))
	float FeverMaxOffsetC = 4.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Temperature", meta=(ClampMin="28.0", ClampMax="37.0",
		ToolTip="Core temp (C) below which hypothermia condition/penalties begin."))
	float HypothermiaOnsetC = 35.f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Temperature", meta=(ClampMin="37.0", ClampMax="43.0",
		ToolTip="Core temp (C) above which hyperthermia condition/penalties begin."))
	float HyperthermiaOnsetC = 38.5f;

	// ── Health|Survival ────────────────────────────────────────────────────
	// Drains are per real-time hour at rest; exertion/heat scale them in code.

	UPROPERTY(Config, EditAnywhere, Category="Health|Survival", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Hydration01 lost per hour at rest in neutral temperature."))
	float HydrationDrainPerHour = 0.04f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Survival", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Calories01 lost per hour at rest."))
	float CalorieDrainPerHour = 0.02f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Survival", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="SleepDebt01 gained per hour awake."))
	float SleepDebtPerHourAwake = 0.05f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Survival", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="SleepDebt01 removed per hour asleep."))
	float SleepRecoveryPerHourAsleep = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Survival", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Wetness01 dried per hour in neutral conditions (heat/wind handled by callers of AddWetness)."))
	float WetnessDryPerHour = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Survival", meta=(ClampMin="0.0",
		ToolTip="Fatigue01 gained per second at full exertion."))
	float FatigueGainPerSecond = 0.002f;

	UPROPERTY(Config, EditAnywhere, Category="Health|Survival", meta=(ClampMin="0.0",
		ToolTip="Fatigue01 recovered per second at rest (scaled by calories/sleep)."))
	float FatigueRecoveryPerSecond = 0.001f;

	// ── Health|Infection & Healing ─────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Health|InfectionHealing", meta=(ClampMin="0.0",
		ToolTip="Wound InfectionRisk01 growth per second per unit contamination (immune strength fights it)."))
	float InfectionRiskGrowthRate = 0.0003f;

	UPROPERTY(Config, EditAnywhere, Category="Health|InfectionHealing", meta=(ClampMin="0.0",
		ToolTip="Infection/risk reduction per second at full immune strength."))
	float ImmuneFightRate = 0.0004f;

	UPROPERTY(Config, EditAnywhere, Category="Health|InfectionHealing", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Systemic Infection01 at/above this spawns the Sepsis condition."))
	float SepsisThreshold = 0.7f;

	UPROPERTY(Config, EditAnywhere, Category="Health|InfectionHealing", meta=(ClampMin="0.0",
		ToolTip="Base wound severity healed per second under ideal survival stats and treatment."))
	float BaseHealRatePerSecond = 0.00015f;

	UPROPERTY(Config, EditAnywhere, Category="Health|InfectionHealing", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Wound severity at/above which a healed wound scars (visual persistence; optional functional penalty)."))
	float ScarSeverityThreshold = 0.4f;

	UPROPERTY(Config, EditAnywhere, Category="Health|InfectionHealing", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="StructuralDamage01 at/above which an untreated healed wound leaves a permanent impairment."))
	float ImpairmentStructuralThreshold = 0.7f;

	// ── Echo|Structure ─────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Structure", meta=(
		ToolTip="If true a spine-destroyed Echo can still crawl with its arms; if false it goes immobile."))
	bool bSpineDestroyedAllowsCrawl = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Structure", meta=(ClampMin="0", ClampMax="5",
		ToolTip="Minimum fingers remaining on a hand for that hand to make strong grabs (fewer = weak grab only)."))
	int32 MinFingersForStrongGrab = 3;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Structure", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Severity scale converting human-weapon severity into Echo brain integrity loss on head hits."))
	float EchoBrainDamageScale = 1.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Structure", meta=(ClampMin="0.0", ClampMax="4.0",
		ToolTip="Global scale on weapon DismemberChance when resolving severing hits against Echo parts."))
	float EchoDismemberScale = 1.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Structure", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Resolved severity at/above which a Ballistic/Crush/Explosion torso or pelvis hit destroys the Echo's spine function."))
	float EchoSpineDestroySeverity = 0.85f;

	// ── Debug|Melee ────────────────────────────────────────────────────────
	// Player debug melee ray (development tool). DAMAGE values are authoritative
	// on the profile asset — assign one or the swing does no damage. Only behavior
	// knobs (range, debug draw) live here so there is exactly one place per concern.

	UPROPERTY(Config, EditAnywhere, Category="Debug|Melee", meta=(
		ToolTip="Weapon profile used by the player's debug melee ray. REQUIRED: no profile = trace-only (no damage applied)."))
	TSoftObjectPtr<UATR_WeaponDamageProfile> DebugMeleeProfile;

	UPROPERTY(Config, EditAnywhere, Category="Debug|Melee", meta=(ClampMin="10.0", ClampMax="100000.0", ForceUnits="cm",
		ToolTip="Trace length of the player's debug melee ray."))
	float DebugMeleeRange = 250.f;

	UPROPERTY(Config, EditAnywhere, Category="Debug|Melee", meta=(
		ToolTip="Draw a debug line/point for each debug melee trace."))
	bool bDrawDebugMeleeTrace = true;

	// ── Echo|Replication ───────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Replication", meta=(ClampMin="1", ClampMax="1024",
		ToolTip="Max structural deltas drained from the Echo health model per network update (bandwidth cap)."))
	int32 MaxStructuralDeltasPerUpdate = 64;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Replication", meta=(ClampMin="16", ClampMax="65536",
		ToolTip="Pending-delta queue cap; beyond this the model collapses an Echo's deltas into one FullStructuralRefresh."))
	int32 MaxPendingDeltas = 4096;

	// ── Debug ──────────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Debug",
		meta=(ToolTip="Log every damage event: region, type, mitigation, wounds created."))
	bool bLogDamageEvents = true;

	UPROPERTY(Config, EditAnywhere, Category="Debug",
		meta=(ToolTip="Log condition add/remove/stage transitions."))
	bool bLogConditionChanges = true;

	UPROPERTY(Config, EditAnywhere, Category="Debug",
		meta=(ToolTip="Verbose: log vital deltas every fast tick (spammy — VeryVerbose channel)."))
	bool bLogVitalDeltas = false;

	UPROPERTY(Config, EditAnywhere, Category="Debug",
		meta=(ToolTip="Log Echo structural mask/capability changes and delta queue stats."))
	bool bLogEchoStructuralChanges = false;
};
