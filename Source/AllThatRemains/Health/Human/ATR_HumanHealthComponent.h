// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "../ATR_HealthTypes.h"
#include "ATR_HumanHealthTypes.h"
#include "ATR_HumanStats.h"
#include "ATR_HumanHealthComponent.generated.h"

class UATR_HealthSettings;
class UATR_BodyRegionDefinition;
class UATR_ConditionDefinition;
class UATR_DamageTypeDefinition;
class UATR_DiseaseDefinition;
class UATR_SubstanceDefinition;
class UATR_TreatmentDefinition;
struct FATR_BodyRegionRow;
struct FATR_DamageTypeRow;

// Environmental inputs pushed by world/weather/shelter systems (linked later).
// The component only reads this; it never queries the world itself.
USTRUCT(BlueprintType)
struct FATR_EnvironmentState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Environment")
	float AirTemperatureC = 24.f;

	// Extra effective warmth from fires/heaters, degrees C.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Environment", meta = (ClampMin = "0.0"))
	float RadiantHeatC = 0.f;

	// 0 = fully exposed, 1 = fully sheltered (halts wind/rain effects and
	// dampens temperature drift).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Environment", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Shelter01 = 0.f;

	// Clothing insulation 0..1, composed by the clothing system from
	// UATR_ArmorMaterialDefinition::Insulation across worn layers.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Environment", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Insulation01 = 0.f;
};

// An administered substance: runtime curve state + the definition driving it.
USTRUCT()
struct FATR_ActiveSubstance
{
	GENERATED_BODY()

	UPROPERTY() FATR_SubstanceEffect State;
	UPROPERTY() TObjectPtr<const UATR_SubstanceDefinition> Definition = nullptr;
};

// A contracted disease: runtime state + the definition driving it.
USTRUCT()
struct FATR_ActiveDisease
{
	GENERATED_BODY()

	UPROPERTY() FATR_DiseaseState State;
	UPROPERTY() TObjectPtr<const UATR_DiseaseDefinition> Definition = nullptr;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FATR_OnDeathSignature, EATR_DeathCause, Cause);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FATR_OnConsciousnessChangedSignature, bool, bConscious);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FATR_OnWoundCreatedSignature, FATR_Wound, Wound);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FATR_OnConditionChangedSignature, EATR_ConditionType, Type, float, Severity01, bool, bActive);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FATR_OnDerivedStatsChangedSignature);

// ─────────────────────────────────────────────────────────────────────────────
// Full human physiological simulation — player and important living NPCs
// (the top two simulation tiers; lower NPC tiers can run this component with
// the slow systems disabled later).
//
// NO monolithic HP. Death only via EATR_DeathCause paths. Combat is
// event-driven through ApplyDamageEvent (pipeline below); biology runs in
// three low-frequency buckets (fast/medium/slow) — never per-frame, never
// per-wound-per-frame.
//
// Combat pipeline (one call, no ticking):
//   hit → region → weapon profile → clothing/armor mitigation (interface) →
//   severity/depth/penetration → wound(s) → immediate conditions/organ damage
//   → pain/shock spike → derived stats recalc → log.
//
// Server-authoritative; simulation only runs with authority. The linking pass
// replicates the OBSERVABLE state (vitals, survival, derived stats, blood,
// wounds, conditions, impairments, death/consciousness) so client HUD/anim can
// read the same getters as the server. Diseases and substances stay
// server-only (they carry definition pointers and drive simulation, not
// presentation). All mutation APIs remain authority-gated.
//
// All tunables: UATR_HealthSettings + DataAssets. Nothing hardcoded except
// code-default data rows used when no DataAsset is assigned.
// ─────────────────────────────────────────────────────────────────────────────
UCLASS(ClassGroup=(ATR), meta=(BlueprintSpawnableComponent))
class ALLTHATREMAINS_API UATR_HumanHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UATR_HumanHealthComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ── Combat entry point ─────────────────────────────────────────────────

	// The single way trauma enters a human. Returns ids of wounds created
	// (empty if fully mitigated or already dead).
	UFUNCTION(BlueprintCallable, Category = "Health|Damage")
	TArray<int32> ApplyDamageEvent(const FATR_DamageEvent& Event);

	// ── Treatment / medical API ────────────────────────────────────────────

	// Apply a wound-level treatment (bandage/tourniquet/disinfect/suture/
	// splint). ApplierSkill01 combines with item quality. False if the wound
	// doesn't exist, is internal, or the treatment doesn't apply.
	UFUNCTION(BlueprintCallable, Category = "Health|Treatment")
	bool ApplyTreatmentToWound(int32 WoundId, const UATR_TreatmentDefinition* Treatment, float ApplierSkill01 = 0.5f);

	// Remove a tourniquet (stops accruing tissue damage; bleeding may resume).
	UFUNCTION(BlueprintCallable, Category = "Health|Treatment")
	bool RemoveTourniquet(int32 WoundId);

	// Administer any substance: medicine, stimulant, sedative, alcohol, poison.
	UFUNCTION(BlueprintCallable, Category = "Health|Treatment")
	void AdministerSubstance(const UATR_SubstanceDefinition* Substance, float Dose = 1.f);

	// Transfuse blood. Compatibility per ATR_Health::IsBloodCompatible;
	// incompatible blood starts a transfusion reaction (fever/shock/toxicity).
	UFUNCTION(BlueprintCallable, Category = "Health|Treatment")
	void ApplyTransfusion(EATR_BloodType DonorType, float Volume01);

	// ── Disease / survival inputs ──────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Health|Disease")
	void ContractDisease(const UATR_DiseaseDefinition* Disease);

	// Eat/drink. All parameters are 01-units added (quality blends into
	// NutritionQuality trend).
	UFUNCTION(BlueprintCallable, Category = "Health|Survival")
	void Consume(float Hydration01, float Calories01, float Protein01, float Quality01);

	UFUNCTION(BlueprintCallable, Category = "Health|Survival")
	void SetSleeping(bool bAsleep);

	UFUNCTION(BlueprintCallable, Category = "Health|Survival")
	void SetEnvironment(const FATR_EnvironmentState& NewEnvironment);

	UFUNCTION(BlueprintCallable, Category = "Health|Survival")
	void AddWetness(float Amount01);

	// Movement/combat systems report current exertion 0..1 (decays on its
	// own). Scales fatigue, hydration/calorie drain, and body heat.
	UFUNCTION(BlueprintCallable, Category = "Health|Survival")
	void NotifyExertion(float Intensity01);

	// ── Queries ────────────────────────────────────────────────────────────

	// UFUNCTIONs cannot return references — BP getters copy. C++ callers that
	// care should use the *Ref accessors below.
	UFUNCTION(BlueprintPure, Category = "Health") FATR_HumanVitals GetVitals() const { return Vitals; }
	UFUNCTION(BlueprintPure, Category = "Health") FATR_HumanSurvivalStats GetSurvivalStats() const { return Survival; }
	UFUNCTION(BlueprintPure, Category = "Health") FATR_DerivedCombatStats GetDerivedStats() const { return Derived; }
	UFUNCTION(BlueprintPure, Category = "Health") FATR_BloodState GetBloodState() const { return Blood; }
	UFUNCTION(BlueprintPure, Category = "Health") TArray<FATR_Wound> GetWounds() const { return Wounds; }
	UFUNCTION(BlueprintPure, Category = "Health") TArray<FATR_Condition> GetConditions() const { return Conditions; }
	UFUNCTION(BlueprintPure, Category = "Health") TArray<FATR_PermanentImpairment> GetImpairments() const { return Impairments; }
	UFUNCTION(BlueprintPure, Category = "Health") bool IsAlive() const { return DeathCause == EATR_DeathCause::None; }
	UFUNCTION(BlueprintPure, Category = "Health") bool IsConscious() const { return !bUnconscious && IsAlive(); }
	UFUNCTION(BlueprintPure, Category = "Health") EATR_DeathCause GetDeathCause() const { return DeathCause; }

	const FATR_HumanVitals& GetVitalsRef() const { return Vitals; }
	const FATR_HumanSurvivalStats& GetSurvivalStatsRef() const { return Survival; }
	const FATR_DerivedCombatStats& GetDerivedStatsRef() const { return Derived; }
	const TArray<FATR_Wound>& GetWoundsRef() const { return Wounds; }
	const TArray<FATR_Condition>& GetConditionsRef() const { return Conditions; }

	UFUNCTION(BlueprintPure, Category = "Health")
	bool HasCondition(EATR_ConditionType Type) const;

	// ── Character setup ────────────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Setup")
	FATR_HumanBaseStats BaseStats;

	UPROPERTY(EditAnywhere, Category = "Health|Setup")
	EATR_BloodType BloodType = EATR_BloodType::OPos;

	// ── Events ─────────────────────────────────────────────────────────────

	UPROPERTY(BlueprintAssignable, Category = "Health|Events") FATR_OnDeathSignature OnDeath;
	UPROPERTY(BlueprintAssignable, Category = "Health|Events") FATR_OnConsciousnessChangedSignature OnConsciousnessChanged;
	UPROPERTY(BlueprintAssignable, Category = "Health|Events") FATR_OnWoundCreatedSignature OnWoundCreated;
	UPROPERTY(BlueprintAssignable, Category = "Health|Events") FATR_OnConditionChangedSignature OnConditionChanged;
	UPROPERTY(BlueprintAssignable, Category = "Health|Events") FATR_OnDerivedStatsChangedSignature OnDerivedStatsChanged;

	// ── Debug ──────────────────────────────────────────────────────────────

	// Multi-line dump of vitals, survival, wounds, conditions, derived stats,
	// last damage event, and death cause — for the debug UI / logging.
	UFUNCTION(BlueprintCallable, Category = "Health|Debug")
	FString GetDebugString() const;

private:
	// ── Simulation buckets (see Update Frequencies in the design doc) ──────

	// 1-4 Hz: bleeding, blood pressure, oxygenation, shock, consciousness,
	// death checks.
	void FastTick(float Dt);

	// 0.5-1 Hz: pain aggregation, fatigue, body temperature, status markers,
	// derived stats refresh.
	void MediumTick(float Dt);

	// In-game-minutes cadence: wound lifecycle, infection, disease, substance
	// metabolism, healing, survival drains, scarring/impairment outcomes.
	void SlowTick(float Dt);

	// ── Combat pipeline helpers ────────────────────────────────────────────

	struct FResolvedDamage
	{
		EATR_DamageType Type = EATR_DamageType::None;
		float Severity = 0.f;
		float Penetration = 0.f;
		float Contamination = 0.f;
	};

	// Steps 3-5: profile/explicit fields → mitigation → resolved numbers.
	void ResolveDamageComponents(const FATR_DamageEvent& Event, TArray<FResolvedDamage>& Out) const;

	// Step 6: create one wound from resolved damage. Returns wound id.
	int32 CreateWound(const FATR_DamageEvent& Event, const FResolvedDamage& Damage);

	// Step 7: immediate organ damage, fractures, concussion, pneumothorax,
	// catastrophic-trauma check, pain/shock spike.
	// Wound is BY VALUE on purpose: this function can add internal-bleeding
	// wounds, which may reallocate the Wounds array under a reference.
	void ApplyImmediateEffects(const FATR_DamageEvent& Event, const FResolvedDamage& Damage, FATR_Wound Wound);

	// ── Condition management ───────────────────────────────────────────────

	FATR_Condition* FindCondition(EATR_ConditionType Type, EATR_BodyRegion Region = EATR_BodyRegion::None);
	// bOverwriteSeverity=false (wound/condition path): existing severity rises via Max, never drops.
	// bOverwriteSeverity=true (status-marker path): severity tracks current source value both ways.
	void SetCondition(EATR_ConditionType Type, float Severity01, EATR_BodyRegion Region = EATR_BodyRegion::None,
	                  int32 SourceWoundId = INDEX_NONE, float ProgressionRate = 0.f, float Duration = -1.f,
	                  bool bOverwriteSeverity = false);
	void RemoveCondition(EATR_ConditionType Type, EATR_BodyRegion Region = EATR_BodyRegion::None);

	// Sync derived status markers (Pain/Fever/Dehydration/.../Limping) from
	// their source values. Markers never apply penalties (amendment #3).
	void UpdateStatusMarkers();

	// ── Internals ──────────────────────────────────────────────────────────

	void RecalculateDerivedStats();
	void RecalculateSubstanceTotals();
	void Die(EATR_DeathCause Cause);
	FATR_DamageMitigation QueryMitigation(EATR_BodyRegion Region, EATR_DamageType Type) const;

	// DataAsset rows with code-default fallback (system runs content-free).
	FATR_BodyRegionRow GetRegionRow(EATR_BodyRegion Region) const;
	FATR_DamageTypeRow GetDamageTypeRow(EATR_DamageType Type) const;

	float WorstWoundSeverity(EATR_BodyRegion Region) const;
	bool HasImpairment(EATR_ImpairmentType Type, EATR_BodyRegion Region) const;

	// ── State ──────────────────────────────────────────────────────────────
	// Observable state replicates (server writes, clients read via the public
	// getters). Property replication only ships deltas on change, and the slow
	// containers (wounds/conditions) mutate at tick-bucket cadence, not per frame.

	UPROPERTY(Replicated) FATR_HumanVitals Vitals;
	UPROPERTY(Replicated) FATR_HumanSurvivalStats Survival;
	UPROPERTY(Replicated) FATR_BloodState Blood;
	UPROPERTY(Replicated) FATR_DerivedCombatStats Derived;
	UPROPERTY(Replicated) TArray<FATR_Wound> Wounds;
	UPROPERTY(Replicated) TArray<FATR_Condition> Conditions;
	UPROPERTY() TArray<FATR_ActiveDisease> Diseases;       // server-only (definition ptrs)
	UPROPERTY() TArray<FATR_ActiveSubstance> Substances;   // server-only (definition ptrs)
	UPROPERTY(Replicated) TArray<FATR_PermanentImpairment> Impairments;

	UPROPERTY() FATR_EnvironmentState Environment;

	// Aggregated substance effects, recomputed on the slow tick and on
	// administration; read by fast/medium ticks and derived stats.
	struct FSubstanceTotals
	{
		float PainRelief = 0.f;
		float FeverReduction = 0.f;
		float InfectionSuppression = 0.f;
		float Alertness = 0.f;
		float Sedation = 0.f;
		float ReactionPenalty = 0.f;
		float PerceptionPenalty = 0.f;
		float BalancePenalty = 0.f;
		float HydrationDrainPerSecond = 0.f;
	};
	FSubstanceTotals SubstanceTotals;

	// Death/consciousness replicate with RepNotify so the client-side delegates
	// (OnDeath / OnConsciousnessChanged) fire for HUD/anim, mirroring the server.
	UPROPERTY(ReplicatedUsing = OnRep_DeathCause)
	EATR_DeathCause DeathCause = EATR_DeathCause::None;

	UPROPERTY(ReplicatedUsing = OnRep_Unconscious)
	bool bUnconscious = false;

	UFUNCTION() void OnRep_DeathCause();
	UFUNCTION() void OnRep_Unconscious();

	bool bSleeping = false;
	bool bShivering = false;
	float Exertion01 = 0.f;

	// Sustained-collapse death timers (grace windows from settings).
	float CirculatoryCollapseSeconds = 0.f;
	float HypoxiaSeconds = 0.f;
	float TempOutOfRangeSeconds = 0.f;

	// Bucket accumulators.
	float FastAcc = 0.f;
	float MediumAcc = 0.f;
	float SlowAcc = 0.f;

	int32 NextWoundId = 0;

	// Cached settings (read once at BeginPlay; settings are DefaultConfig).
	UPROPERTY() TObjectPtr<const UATR_HealthSettings> Settings;

	// Data tables resolved once at BeginPlay (may stay null — code defaults).
	UPROPERTY() TObjectPtr<const UATR_BodyRegionDefinition> RegionDef;
	UPROPERTY() TObjectPtr<const UATR_DamageTypeDefinition> DamageTypeDef;
	UPROPERTY() TObjectPtr<const UATR_ConditionDefinition> ConditionDef;

	// Last damage event summary for the debug requirements.
	FString LastDamageDebug;
};
