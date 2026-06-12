// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "../ATR_HealthTypes.h"
#include "ATR_WeaponDamageProfile.generated.h"

// One damage component of a weapon. Most weapons deal several at once —
// an axe is slash + blunt; a firearm is ballistic with fracture pressure.
USTRUCT(BlueprintType)
struct FATR_WeaponDamageComponent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Damage")
	EATR_DamageType DamageType = EATR_DamageType::Blunt;

	// Nominal trauma at EventScale 1, before armor/clothing mitigation.
	UPROPERTY(EditAnywhere, Category = "Damage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Severity01 = 0.3f;

	// Ability to reach deep tissue/organs (knives high, bats low).
	UPROPERTY(EditAnywhere, Category = "Damage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Penetration01 = 0.f;

	// Contamination introduced (bites/dirty blades high, clean rounds low).
	UPROPERTY(EditAnywhere, Category = "Damage", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Contamination01 = 0.f;
};

// Data-driven description of what a weapon (or natural attack) does to a body.
// Examples: axe = slash/blunt + high dismember; knife = puncture/slash + high
// contamination when dirty; bat = blunt/crush + fracture/stun; firearm =
// ballistic + penetration; Echo bite = bite + contamination + disease.
//
// Combat code resolves hits THROUGH these assets — never hardcoded damage rules.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_WeaponDamageProfile : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// Damage dealt per hit. Each component becomes a wound candidate.
	UPROPERTY(EditAnywhere, Category = "Damage")
	TArray<FATR_WeaponDamageComponent> DamageComponents;

	// Chance a severe limb/digit hit severs (axes high, bats zero). Also used
	// by the Echo model for structural part removal.
	UPROPERTY(EditAnywhere, Category = "Damage|Trauma", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DismemberChance01 = 0.f;

	// Chance pressure toward bone fracture on blunt/crush components.
	UPROPERTY(EditAnywhere, Category = "Damage|Trauma", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FractureChance01 = 0.f;

	// Chance of concussion/stun on head/blunt impacts.
	UPROPERTY(EditAnywhere, Category = "Damage|Trauma", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StunChance01 = 0.f;

	// Scales the bleed rate of wounds this weapon creates.
	UPROPERTY(EditAnywhere, Category = "Damage|Trauma", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float BleedScale = 1.f;

	// Scales the pain of wounds this weapon creates.
	UPROPERTY(EditAnywhere, Category = "Damage|Trauma", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float PainScale = 1.f;

	// Ballistic only: chance the round exits, creating a second (exit) wound.
	UPROPERTY(EditAnywhere, Category = "Damage|Trauma", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ExitWoundChance01 = 0.f;
};
