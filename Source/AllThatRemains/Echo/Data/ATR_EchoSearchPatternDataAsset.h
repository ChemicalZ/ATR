// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ATR_EchoSearchPatternDataAsset.generated.h"

// Defines a lost-sight search behavior variant without changing code. Referenced from
// UATR_EchoSettings (the default pattern) and optionally overridden per archetype.
// The subsystem reads these values when beginning/advancing a search; nothing here is
// behavior-critical state, only tuning.
UCLASS(BlueprintType, meta = (DisplayName = "Echo Search Pattern"))
class ALLTHATREMAINS_API UATR_EchoSearchPatternDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	// Per-step {forward, side} multipliers of the Echo's search radius. Index 0 is the
	// projected-direction probe; later entries widen the fan. Empty falls back to the
	// built-in standard fan so a misconfigured asset never stops search entirely.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Search")
	TArray<FVector2D> SearchOffsets;

	// Scales the per-Echo search radius derived from the clamped velocity projection.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Search", meta = (ClampMin = "0.0"))
	float SearchRadiusScale = 1.f;

	// Scales the per-Echo maximum search duration.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Search", meta = (ClampMin = "0.0"))
	float MaxDurationScale = 1.f;

	// Per-step angular jitter applied to the primary direction so a group fans out.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Search", meta = (ClampMin = "0.0", ForceUnits = "deg"))
	float RandomAngleDegrees = 20.f;

	// If true, the primary direction comes from the target's last observed travel; if false,
	// the search uses the Echo's facing direction only (confused/local pattern).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Search")
	bool bUseProjectedDirection = true;

	// If true, an exhausted/failed projection may fall back to a local random point instead of
	// immediately ending the search.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Search")
	bool bAllowLocalRandomFallback = true;
};
