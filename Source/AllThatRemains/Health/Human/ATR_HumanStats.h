// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ATR_HumanStats.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
// Grounded human stats.
//
// Base stats are innate/trained attributes (slow-changing character data).
// Derived stats are NEVER manually maintained meters — they are recalculated
// from base stats + injuries + survival state + equipment + skills whenever an
// input changes (dirty-flag recalc in UATR_HumanHealthComponent), then cached.
// Other systems (movement, weapons, stealth) read the cached struct.
// ─────────────────────────────────────────────────────────────────────────────

// Innate attributes, 0..1 where 0.5 = average adult and 1 = peak human.
USTRUCT(BlueprintType)
struct FATR_HumanBaseStats
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float Strength = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float Endurance = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float Dexterity = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float Balance = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float Reaction = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float Perception = 0.5f;

	// Scales how much aggregate pain penalizes capability and consciousness.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float PainTolerance = 0.5f;

	// Innate immune quality. Current ImmuneStrength01 (survival stats) is this
	// modified by sleep/nutrition/disease/substances.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health|Stats", meta = (ClampMin = "0.0", ClampMax = "1.0")) float ImmuneResilience = 0.5f;
};

// Cached derived capability. 01 values are absolute capability (0 = unable);
// *Mult values are multipliers around 1 for systems that own their own base
// numbers (movement speed, reload anims, noise).
USTRUCT(BlueprintType)
struct FATR_DerivedCombatStats
{
	GENERATED_BODY()

	// Worst-hand grip. Gates weapon usability; hand/finger wounds crush this.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float GripStrengthLeft01 = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float GripStrengthRight01 = 1.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float MeleePower01 = 1.f;

	// One/two-handed weapon handling (arm wounds, fractures, pain).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float WeaponControl01 = 1.f;

	// Steadiness of aim (pain, fatigue, sleep debt, cold shivering, tremors).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float AimStability01 = 1.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float ReloadSpeedMult = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float MoveSpeedMult = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float SprintDurationMult = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float StaminaRecoveryMult = 1.f;

	// Movement noise multiplier (limping is loud; injured sneaking is worse).
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float NoiseMult = 1.f;

	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float ClimbAbility01 = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float ShovePower01 = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") float GrappleResistance01 = 1.f;

	// Hard gates other systems should respect outright.
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") bool bCanSprint = true;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") bool bCanClimb = true;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") bool bCanUseTwoHandedWeapons = true;
	UPROPERTY(BlueprintReadOnly, Category = "Health|Derived") bool bIsLimping = false;
};
