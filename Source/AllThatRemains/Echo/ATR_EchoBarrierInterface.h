// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ATR_EchoBarrierInterface.generated.h"

class UATR_EchoBarrierDataAsset;

UINTERFACE(BlueprintType, MinimalAPI)
class UATR_EchoBarrier : public UInterface
{
	GENERATED_BODY()
};

// Implemented by world actors that act as Echo pursuit barriers (doors, windows,
// fences, gates, barricades, destructible walls). The Echo system uses this to:
//   - classify the barrier and read its interaction rules (GetEchoBarrierData);
//   - know when engagement should end because the barrier opened/broke (IsEchoPassable);
//   - deliver press/attack impacts so the actor applies damage, plays FX, and
//     replicates its own damage/break state (OnEchoBarrierImpact).
//
// Engagement NEVER asks how to get around the barrier. A blocked Echo presses,
// attacks, reaches through, frustrates, or decays — it does not reroute.
//
// Actors without this interface can still be classified via actor tags
// (Echo.Obstacle.Door / Window / Fence / Gate / Barricade / Vehicle /
// DestructibleWall); untagged static geometry is treated as a non-interactable
// wall (press briefly → frustrated search → decay).
class ALLTHATREMAINS_API IATR_EchoBarrier
{
	GENERATED_BODY()

public:
	// Interaction rules for this barrier. May return null → per-type defaults apply.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Echo|Barrier")
	UATR_EchoBarrierDataAsset* GetEchoBarrierData() const;

	// True once the barrier no longer blocks movement (door opened, window broken
	// passable, barricade destroyed). Engagement ends and pursuit resumes.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Echo|Barrier")
	bool IsEchoPassable() const;

	// One Echo impact landed. Damage is the pressure-scaled per-hit damage; Pressure is
	// the current accumulated group pressure on this barrier. The actor owns applying
	// damage to itself, breaking, FX/audio, and replication of its damage state.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Echo|Barrier")
	void OnEchoBarrierImpact(AActor* EchoInstigator, float Damage, float Pressure);

	// Native default implementations (see .cpp) — C++ implementers override these.
	virtual UATR_EchoBarrierDataAsset* GetEchoBarrierData_Implementation() const;
	virtual bool IsEchoPassable_Implementation() const;
	virtual void OnEchoBarrierImpact_Implementation(AActor* EchoInstigator, float Damage, float Pressure);
};
