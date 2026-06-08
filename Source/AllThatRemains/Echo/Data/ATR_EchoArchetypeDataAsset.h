// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ATR_EchoArchetypeDataAsset.generated.h"

class UATR_EchoSearchPatternDataAsset;
class UATR_EchoObstacleBehaviorDataAsset;

// Per-Echo behavior modifiers. An Echo may reference an archetype to scale its base tuning
// (sight/hearing/smell ranges, aggression, search persistence, movement speed, obstacle
// aggression) and to override its search/obstacle behavior assets. All values are multipliers
// applied on top of the global UATR_EchoSettings base, so an Echo with no archetype behaves at
// the global defaults.
UCLASS(BlueprintType, meta = (DisplayName = "Echo Archetype"))
class ALLTHATREMAINS_API UATR_EchoArchetypeDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float Aggression = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float SightScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float HearingScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float SmellScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float SearchPersistenceScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float AgitationSensitivityScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float MovementSpeedScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype", meta = (ClampMin = "0.0"))
	float ObstacleAggressionScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype")
	TSoftObjectPtr<UATR_EchoSearchPatternDataAsset> SearchPatternOverride;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Archetype")
	TSoftObjectPtr<UATR_EchoObstacleBehaviorDataAsset> ObstacleBehaviorOverride;
};
