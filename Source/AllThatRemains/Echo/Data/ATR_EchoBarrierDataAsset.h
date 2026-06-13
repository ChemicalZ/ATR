// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "AllThatRemains/Echo/ATR_EchoRuntimeTypes.h"
#include "ATR_EchoBarrierDataAsset.generated.h"

// Per-barrier-type interaction description (design doc: Data Asset Guidance). Make one
// asset per obstacle archetype — wooden door, metal door, chain-link fence, privacy
// fence, glass window, boarded window, player-built barricade — and return it from the
// barrier actor's IATR_EchoBarrier::GetEchoBarrierData implementation.
//
// This describes what an Echo can DO to the barrier (damage, reach through, climb
// through, push) and how engaging it sounds/feels. It never describes how to route
// around the barrier — that behavior does not exist during active pursuit.
UCLASS(BlueprintType, meta = (DisplayName = "Echo Barrier"))
class ALLTHATREMAINS_API UATR_EchoBarrierDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier")
	EATR_EchoBarrierType BarrierType = EATR_EchoBarrierType::Unknown;

	// --- What the Echo can do to it ---

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier")
	bool bCanBeDamaged = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier")
	bool bCanBeBroken = true;

	// Permeable — Echoes reach/claw/grab through it (chain-link fence, broken window).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier")
	bool bCanBeReachedThrough = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier")
	bool bCanBeClimbedThrough = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier")
	bool bCanBePushed = false;

	// --- Perception interaction ---

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Perception")
	bool bBlocksSight = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Perception")
	bool bBlocksSound = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Perception")
	bool bBlocksSmell = false;

	// --- Engagement tuning (0 = use the project-wide Echo|Barrier settings value) ---

	// Cumulative damage at which the barrier actor is expected to break/open. The barrier
	// actor owns its own health resolution; this is the design-intent value engagement
	// math may read (e.g., for pressure-driven weak-barrier breaks).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Engagement", meta = (ClampMin = "0.0"))
	float DamageThreshold = 100.f;

	// Group pressure at which a weak barrier may fail outright (0 = pressure never breaks it).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Engagement", meta = (ClampMin = "0.0"))
	float PressureThreshold = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Engagement", meta = (ClampMin = "0.0", ForceUnits = "cm"))
	float ReachThroughDistanceCm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Engagement", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float AttackIntervalSecondsOverride = 0.f;

	// Source loudness of impacts on THIS barrier type, dB SPL @ reference distance
	// (metal door ≈ 90, glass ≈ 100, wooden fence ≈ 75). 0 = use the project-wide
	// BarrierImpactLoudnessDb setting.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Engagement", meta = (ClampMin = "0.0", ClampMax = "194.0", ForceUnits = "dB"))
	float ImpactLoudnessDbOverride = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Barrier|Engagement", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float ClimbThroughDelaySeconds = 1.5f;
};
