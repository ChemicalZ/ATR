// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "../ATR_HealthTypes.h"
#include "ATR_HumanHealthTypes.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
// Human physiological model — data only. Simulation lives in
// UATR_HumanHealthComponent; tunables in UATR_HealthSettings.
//
// Humans fail physiologically, metabolically, and environmentally — never via
// a single HP pool. Every struct here is a piece of that failure model:
//   vitals      — what the body is doing right now
//   survival    — the metabolic inputs that decide recovery vs. decline
//   wounds      — discrete trauma with their own lifecycle + treatment state
//   conditions  — bridges from wounds/stats to failure and combat penalties
//   disease     — data-driven illness framework (no hardcoded illnesses)
//   substances  — one timed framework for medicine/drugs/poison
//   blood       — type + transfusion reaction state (volume lives in vitals)
//   impairments — permanent outcomes that never resolve
// ─────────────────────────────────────────────────────────────────────────────

// Current physiological state. All 01 values are normalized 0..1 where 1 is
// healthy; CoreTemperatureC is the single canonical body temperature
// (design amendment #1 — it was duplicated in survival stats).
USTRUCT(BlueprintType)
struct FATR_HumanVitals
{
	GENERATED_BODY()

	// Circulating blood volume. Single source of truth (amendment #2).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float BloodVolume01 = 1.f;

	// Effective perfusion. Derived each fast tick from volume, heart, shock.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float BloodPressure01 = 1.f;

	// Tissue oxygen delivery. Derived from lungs, airway, pressure.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float Oxygenation01 = 1.f;

	// Brain health/function. <= fatal threshold -> death (BrainFailure).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float BrainFunction01 = 1.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float HeartFunction01 = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float LeftLungFunction01 = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float RightLungFunction01 = 1.f;

	// Awareness level. 0 = unconscious. Derived from brain, oxygen, pressure,
	// pain, shock, sleep debt, sedation, temperature.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float Consciousness01 = 1.f;

	// AGGREGATED pain (wounds + conditions + disease - painkillers), 0..1.
	// The 'Pain' condition is a threshold marker derived from this value.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float Pain01 = 0.f;

	// Circulatory shock, 0..1. Rises with blood loss, severe pain, transfusion
	// reactions; decays slowly when stable.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float Shock01 = 0.f;

	// Canonical core body temperature in Celsius.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float CoreTemperatureC = 37.f;

	// Systemic infection load, 0..1. At sepsis threshold spawns Sepsis; at
	// fatal threshold kills (Sepsis cause).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float Infection01 = 0.f;

	// AGGREGATED febrile drive from infection/disease/transfusion reaction,
	// 0..1. Offsets the temperature target upward; the 'Fever' condition is a
	// threshold marker derived from this value.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float Fever01 = 0.f;

	// Systemic toxin load (poison, overdose, organ stress), 0..1.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Vitals") float Toxicity01 = 0.f;
};

// Metabolic/environmental inputs. Not arcade meters — these feed vitals,
// healing, disease resistance, stamina, and combat capability.
// Note: no temperature here (amendment #1); Wetness is the temperature INPUT.
USTRUCT(BlueprintType)
struct FATR_HumanSurvivalStats
{
	GENERATED_BODY()

	// 1 = fully hydrated. Affects blood recovery, stamina, heat tolerance,
	// consciousness, healing, substance processing.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float Hydration01 = 1.f;

	// 1 = well fed. Energy for activity and healing.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float Calories01 = 1.f;

	// Protein/repair reserve drawn down by healing wounds and rebuilding.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float ProteinRepairReserve01 = 1.f;

	// Diet quality trend. Scales healing and immune effectiveness.
	// First pass does NOT simulate individual vitamins.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float NutritionQuality01 = 1.f;

	// 0 = fully rested, 1 = collapse imminent. Affects reaction, perception,
	// aim, pain tolerance, immune response, healing, decision speed.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float SleepDebt01 = 0.f;

	// Short-term exertion tiredness (distinct from sleep debt).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float Fatigue01 = 0.f;

	// How wet skin/clothing is. Input to temperature simulation.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float Wetness01 = 0.f;

	// Current immune effectiveness: base ImmuneResilience modified by sleep,
	// nutrition, disease suppression, and substances.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Survival") float ImmuneStrength01 = 1.f;
};

// Per-wound treatment state (embedded in FATR_Wound — amendment #4).
// Treatment modifies wound progression; it never instantly erases injuries.
USTRUCT(BlueprintType)
struct FATR_WoundTreatmentState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") bool bBandaged = false;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") bool bTourniquet = false;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") bool bDisinfected = false;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") bool bSutured = false;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") bool bSplinted = false;

	// Dressing cleanliness, 1 = fresh. Degrades over time; dirty dressings
	// stop protecting against contamination.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") float Cleanliness01 = 1.f;

	// Skill/supply quality of the best treatment applied. Scales benefits.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") float TreatmentQuality01 = 0.f;

	// Seconds a tourniquet has been on. Long application risks tissue death
	// (escalates the wound's StructuralDamage on the slow tick).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Treatment") float TourniquetSeconds = 0.f;
};

// One discrete injury. Created by the combat pipeline; progressed on the slow
// biological tick. Never ticked per frame.
USTRUCT(BlueprintType)
struct FATR_Wound
{
	GENERATED_BODY()

	// Stable id for treatment targeting and debug (amendment #4).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") int32 WoundId = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") EATR_BodyRegion Region = EATR_BodyRegion::None;

	// Optional fine-detail target (amendment #6). None for ordinary wounds.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") EATR_BodyDetailKind DetailKind = EATR_BodyDetailKind::None;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") uint8 DetailIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") EATR_DamageType DamageType = EATR_DamageType::None;

	// Overall trauma magnitude after mitigation, 0..1. Falls as the wound heals.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float Severity01 = 0.f;

	// Severity at creation (or worst point). Decides scarring/impairment when
	// the wound finally closes.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float PeakSeverity01 = 0.f;

	// How deep it reached (organ involvement), 0..1.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float Depth01 = 0.f;

	// Blood volume loss rate (01-volume units per second) before treatment.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float BleedRate = 0.f;

	// Contribution to aggregate Pain01 while unhealed.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float PainRate = 0.f;

	// Dirt/saliva in the wound, 0..1. Feeds infection on the slow tick.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float Contamination01 = 0.f;

	// Current chance-pressure toward Infected stage, 0..1.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float InfectionRisk01 = 0.f;

	// Mechanical damage (bone/tendon/organ), 0..1. High values create
	// fractures/impairments and gate capability.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float StructuralDamage01 = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") float AgeSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") EATR_WoundStage Stage = EATR_WoundStage::Open;

	// Internal bleeding pseudo-wound (blunt/crush torso trauma). Cannot be
	// bandaged/sutured/disinfected from outside; only clotting and time help.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") bool bInternal = false;

	// Embedded treatment state (amendment #4).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Wounds") FATR_WoundTreatmentState Treatment;

	bool IsActive() const { return Stage != EATR_WoundStage::Resolved && Stage != EATR_WoundStage::Scarred; }
};

// A condition instance. See EATR_ConditionType for the pathology vs.
// derived-status-marker distinction (markers never apply penalties).
USTRUCT(BlueprintType)
struct FATR_Condition
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Health|Conditions") EATR_ConditionType Type = EATR_ConditionType::None;

	// Region-specific conditions (Fracture, Limping); None for systemic ones.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Conditions") EATR_BodyRegion Region = EATR_BodyRegion::None;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Conditions") float Severity01 = 0.f;

	// Severity change per second on the condition's tick (can be negative).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Conditions") float ProgressionRate = 0.f;

	// Seconds until auto-removal; < 0 = indefinite (until cured/resolved).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Conditions") float RemainingDuration = -1.f;

	// Wound that spawned this condition, INDEX_NONE for systemic conditions.
	// Lets fracture/bleeding resolve when their wound heals.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Conditions") int32 SourceWoundId = INDEX_NONE;
};

// Active disease instance. Diseases are DATA (UATR_DiseaseDefinition) driving
// this runtime state — never hardcoded illness logic.
USTRUCT(BlueprintType)
struct FATR_DiseaseState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Health|Disease") FName DiseaseId;

	// Position in the disease's course, 0 = onset, 1 = full progression.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Disease") float Stage01 = 0.f;

	// Symptom intensity at the current stage (immune fight lowers it).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Disease") float Severity01 = 0.f;

	// Seconds of silent incubation left; symptoms start at 0.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Disease") float IncubationRemaining = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Disease") float ProgressionRate = 0.f;

	// 1 = untreated; medication lowers it (scales progression).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Disease") float TreatmentModifier01 = 1.f;
};

// One active dose of any substance — medicine, drug, alcohol, poison. One
// framework for all of them; what it does lives in UATR_SubstanceDefinition.
USTRUCT(BlueprintType)
struct FATR_SubstanceEffect
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") FName SubstanceId;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") float Dose = 1.f;

	// Elapsed time since administration (amendment #5 — required to evaluate
	// the onset/peak/decay curve).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") float TimeActiveSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") float OnsetTime = 30.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") float PeakTime = 120.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") float Duration = 600.f;

	// Personal metabolism scale (hydration/liver state can modify).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") float MetabolismRate = 1.f;

	// Current effect strength 0..1, evaluated from the curve each slow tick.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Substances") float Severity01 = 0.f;

	// Curve: linear ramp 0->1 between onset and peak, hold, linear decay to 0
	// at Duration (metabolism shortens the tail).
	float EvaluateStrength() const
	{
		const float T = TimeActiveSeconds * FMath::Max(MetabolismRate, 0.01f);
		if (T <= OnsetTime) { return 0.f; }
		if (T < PeakTime)   { return (T - OnsetTime) / FMath::Max(PeakTime - OnsetTime, 1.f); }
		if (T >= Duration)  { return 0.f; }
		return 1.f - (T - PeakTime) / FMath::Max(Duration - PeakTime, 1.f);
	}

	bool IsExpired() const
	{
		return TimeActiveSeconds * FMath::Max(MetabolismRate, 0.01f) >= Duration;
	}
};

// Blood typing + transfusion reaction state. Blood VOLUME lives in vitals
// (amendment #2); this struct only knows what type the body is and how it is
// reacting to recent transfusions.
USTRUCT(BlueprintType)
struct FATR_BloodState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Health|Blood") EATR_BloodType BloodType = EATR_BloodType::OPos;

	// 01-volume units transfused recently (decays on the slow tick).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Blood") float RecentTransfusionVolume = 0.f;

	// Hemolytic reaction severity from incompatible blood, 0..1. Drives
	// fever, shock, toxicity, and death risk while elevated.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Blood") float TransfusionReactionSeverity01 = 0.f;
};

// A permanent, non-healing impairment. Created by destroyed/untreated severe
// injury; feeds derived stats forever.
USTRUCT(BlueprintType)
struct FATR_PermanentImpairment
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Health|Impairments") EATR_BodyRegion Region = EATR_BodyRegion::None;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Impairments") EATR_BodyDetailKind DetailKind = EATR_BodyDetailKind::None;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Impairments") uint8 DetailIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Impairments") EATR_ImpairmentType Type = EATR_ImpairmentType::None;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Impairments") float Severity01 = 1.f;
};
