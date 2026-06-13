// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ATR_HealthTypes.generated.h"

class UATR_WeaponDamageProfile;

// ─────────────────────────────────────────────────────────────────────────────
// Shared health/damage vocabulary used by both the Human model (player and
// important NPCs) and the Echo structural model.
//
// Core rule (design doc): no monolithic HP. Humans die from failure of brain,
// circulation, oxygenation, temperature, sepsis, toxicity, or catastrophic
// trauma. Echoes die only from brain destruction. Combat creates trauma;
// survival stats decide whether trauma stabilizes, worsens, heals, or kills.
//
// Tunables live in UATR_HealthSettings / DataAssets, never hardcoded.
// ─────────────────────────────────────────────────────────────────────────────

// Dedicated log category for health simulation (wounds, vitals, conditions,
// treatment, Echo structural damage). Defined in ATR_HealthTypes.cpp.
DECLARE_LOG_CATEGORY_EXTERN(LogATR_Health, Log, All);

// Coarse body regions tracked for every simulated human. Fine detail
// (fingers, eyes, ...) is NOT a region — see EATR_BodyDetailKind. Detail is
// only referenced by wounds/impairments when actually damaged, per the
// "do not tick unused fine-detail parts" rule.
UENUM(BlueprintType)
enum class EATR_BodyRegion : uint8
{
	None,
	Head,
	Neck,
	Chest,
	Abdomen,
	Pelvis,
	LeftUpperArm,
	LeftForearm,
	LeftHand,
	RightUpperArm,
	RightForearm,
	RightHand,
	LeftThigh,
	LeftShin,
	LeftFoot,
	RightThigh,
	RightShin,
	RightFoot,
	COUNT UMETA(Hidden)
};

// Optional fine-detail target inside a region. Spawned on wounds/impairments
// only when damaged or gameplay-relevant; there are no per-digit arrays.
UENUM(BlueprintType)
enum class EATR_BodyDetailKind : uint8
{
	None,
	Finger, // DetailIndex 0-4, thumb = 0, on the hand of the wound's region
	Toe,    // DetailIndex 0-4 on the foot of the wound's region
	Eye,    // DetailIndex 0 = left, 1 = right (region = Head)
	Ear,    // DetailIndex 0 = left, 1 = right (region = Head)
	Jaw,    // region = Head
	Teeth   // region = Head
};

// What kind of trauma a hit (or hazard) inflicts. Drives wound behavior:
// slash bleeds, puncture reaches organs, blunt fractures, bite contaminates...
UENUM(BlueprintType)
enum class EATR_DamageType : uint8
{
	None,
	Blunt,
	Slash,
	Puncture,
	Crush,
	Bite,
	Burn,
	Ballistic,
	Fall,
	Explosion,
	Toxic,
	Disease
};

// Lifecycle of a wound. Progression is driven by the slow biological tick,
// modified by treatment, contamination, and survival stats.
UENUM(BlueprintType)
enum class EATR_WoundStage : uint8
{
	Open,
	Clotted,
	Inflamed,
	Infected,
	Healing,
	Scarred,
	Chronic,
	Resolved
};

// Conditions bridge wounds/survival stats to body failure and combat penalties.
//
// Two families share this enum (see design amendment #3):
//  - Pathologies (Bleeding, Fracture, Sepsis, ...) — simulated, progress over time.
//  - Derived status markers (Pain, Shock, Fever, WeakGrip, Limping, Vision/
//    HearingImpaired, Unconsciousness, Dehydration, Starvation, SleepDeprivation,
//    Hypo/Hyperthermia) — discrete mirrors of vital/stat thresholds for UI/AI
//    queries. Markers never apply penalties themselves; the owning vital or
//    derived stat is the single source of truth.
UENUM(BlueprintType)
enum class EATR_ConditionType : uint8
{
	None,
	Bleeding,
	InternalBleeding,
	Pain,
	Fracture,
	Sprain,
	Concussion,
	Shock,
	Hypoxia,
	Pneumothorax,
	Infection,
	Fever,
	Sepsis,
	Toxicity,
	Dehydration,
	Starvation,
	SleepDeprivation,
	Hypothermia,
	Hyperthermia,
	Unconsciousness,
	WeakGrip,
	Limping,
	VisionImpaired,
	HearingImpaired
};

// ABO/Rh blood types for the transfusion system.
UENUM(BlueprintType)
enum class EATR_BloodType : uint8
{
	ONeg,
	OPos,
	ANeg,
	APos,
	BNeg,
	BPos,
	ABNeg,
	ABPos
};

// Permanent, non-healing outcomes of destroyed/untreated severe injury.
UENUM(BlueprintType)
enum class EATR_ImpairmentType : uint8
{
	None,
	LostFinger,
	LostToe,
	LostHand,
	LostFoot,
	LostLimb,
	EyeDestroyed,
	HearingLoss,
	NerveDamage,
	ChronicPain,
	ReducedGrip,
	ReducedMobility,
	ReducedLungCapacity
};

// Why a human died. Preserved on the component for UI/debugging ("DeathCause").
UENUM(BlueprintType)
enum class EATR_DeathCause : uint8
{
	None,
	BrainFailure,
	CirculatoryCollapse, // blood volume / pressure below survivable for too long
	Hypoxia,             // oxygenation collapse for too long
	CardiacArrest,       // heart function collapse
	Hypothermia,
	Hyperthermia,
	Sepsis,
	Toxicity,
	CatastrophicTrauma   // single event destroyed required function outright
};

// ─────────────────────────────────────────────────────────────────────────────
// Damage events — the single entry point of the combat pipeline.
// Event-driven: hits create one of these; nothing about combat damage ticks
// per frame.
// ─────────────────────────────────────────────────────────────────────────────

// One resolved hit against a human (or, via the Echo model's own entry point,
// an Echo). Producers (melee traces, ballistics, falls, hazards) fill this in;
// UATR_HumanHealthComponent::ApplyDamageEvent consumes it.
//
// Either set WeaponProfile (preferred — data-driven) or fill the explicit
// fields below for profile-less damage like falls and environmental hazards.
USTRUCT(BlueprintType)
struct FATR_DamageEvent
{
	GENERATED_BODY()

	// Who/what caused this (for logging/aggro). May be null (environment).
	// Not Blueprint-exposed: weak pointers make poor BP pins; C++ producers set it.
	UPROPERTY()
	TWeakObjectPtr<AActor> Instigator;

	// Where the hit landed.
	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage")
	EATR_BodyRegion Region = EATR_BodyRegion::None;

	// Optional fine-detail target (finger/eye/...). None for normal hits.
	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage")
	EATR_BodyDetailKind DetailKind = EATR_BodyDetailKind::None;

	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage")
	uint8 DetailIndex = 0;

	// Data-driven damage description. When set, the profile's components are
	// applied (each becomes a wound candidate) scaled by EventScale01.
	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage")
	TObjectPtr<UATR_WeaponDamageProfile> WeaponProfile = nullptr;

	// Overall scale of this event vs. the profile's nominal values
	// (light swing vs. full swing, grazing ballistic, fall height factor...).
	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float EventScale01 = 1.f;

	// ── Explicit fields — used only when WeaponProfile is null ──

	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage")
	EATR_DamageType DamageType = EATR_DamageType::None;

	// Nominal trauma magnitude before mitigation, 0..1.
	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Severity01 = 0.f;

	// Ability to reach deep tissue/organs through layers, 0..1.
	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Penetration01 = 0.f;

	// Dirt/saliva/debris introduced, 0..1. Drives infection risk.
	UPROPERTY(BlueprintReadWrite, Category = "Health|Damage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Contamination01 = 0.f;
};

// ─────────────────────────────────────────────────────────────────────────────
// Clothing/armor mitigation hook.
// Damage code never hardcodes armor logic; it queries the victim through this
// interface per region per damage type. The clothing/equipment system (a
// separate system, linked later) implements it using
// UATR_ArmorMaterialDefinition data.
// ─────────────────────────────────────────────────────────────────────────────

// Result of asking protection layers about one (region, damage type) pair.
USTRUCT(BlueprintType)
struct FATR_DamageMitigation
{
	GENERATED_BODY()

	// Multiplier applied to incoming severity. 1 = unprotected, 0 = fully stopped.
	UPROPERTY(BlueprintReadWrite, Category = "Health|Armor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SeverityScale = 1.f;

	// Multiplier applied to penetration (plates stop knives, leather slows bites).
	UPROPERTY(BlueprintReadWrite, Category = "Health|Armor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PenetrationScale = 1.f;

	// Multiplier applied to contamination (covered skin stays cleaner).
	UPROPERTY(BlueprintReadWrite, Category = "Health|Armor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ContaminationScale = 1.f;
};

UINTERFACE(MinimalAPI, Blueprintable)
class UATR_DamageMitigationProvider : public UInterface
{
	GENERATED_BODY()
};

// Implemented by whatever owns protection layers (clothing/armor system).
// UATR_HumanHealthComponent queries the owning actor for this interface on
// every damage event; absent implementation means unprotected.
class ALLTHATREMAINS_API IATR_DamageMitigationProvider
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Health|Armor")
	FATR_DamageMitigation GetMitigationForHit(EATR_BodyRegion Region, EATR_DamageType DamageType);
};

// ─────────────────────────────────────────────────────────────────────────────
// Small shared helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace ATR_Health
{
	// Standard ABO/Rh compatibility: O- universal donor, AB+ universal recipient.
	// First pass uses the basic matrix only (design doc: keep simple initially).
	ALLTHATREMAINS_API bool IsBloodCompatible(EATR_BloodType Donor, EATR_BloodType Recipient);

	// True for the limb regions whose bleeding a tourniquet can stop.
	ALLTHATREMAINS_API bool IsLimbRegion(EATR_BodyRegion Region);

	// Coarse hit-location → body-region resolution for actors without per-bone
	// hit zones (capsule hits, debug traces). Uses the victim's actor space:
	// normalized height picks the band (legs/pelvis/abdomen/chest/neck/head),
	// lateral sign picks left/right for limbs. Good enough for the linking pass;
	// physics-asset hit zones can replace it per-actor later.
	ALLTHATREMAINS_API EATR_BodyRegion RegionFromHitLocation(const AActor* Victim, const FVector& WorldHitLocation);
}
