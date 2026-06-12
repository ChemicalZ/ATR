// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ATR_EchoObstacleBehaviorDataAsset.generated.h"

// Configures obstacle interaction without replacing the intent/task architecture. The current
// runtime only performs sidestep/repath; the break/climb/call fields define the future seam so
// door/window/fence interactions can attach through FATR_EchoObstacleIntent + HandleObstacle
// later with no architectural change. Referenced from UATR_EchoSettings (default) and overridable
// per archetype.
UCLASS(BlueprintType, meta = (DisplayName = "Echo Obstacle Behavior"))
class ALLTHATREMAINS_API UATR_EchoObstacleBehaviorDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle")
	bool bCanAttackDoors = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle")
	bool bCanBreakWindows = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle")
	bool bCanClimbFences = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle")
	bool bCanCallNearbyEchoes = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float AttackObstacleDelaySeconds = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle", meta = (ClampMin = "0.0"))
	float ObstacleDamagePerHit = 10.f;

	// Agitation deposited when a group is pounding the same obstacle — draws more Echoes in.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle", meta = (ClampMin = "0.0"))
	float GroupPoundingAgitationAmount = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Obstacle", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float RepathAfterFailedObstacleSeconds = 2.f;
};
