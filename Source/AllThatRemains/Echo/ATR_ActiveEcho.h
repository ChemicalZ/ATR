// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Perception/AIPerceptionComponent.h"
#include "Components/StateTreeComponent.h"
#include "ATR_ActiveEcho.generated.h"

class UATR_EchoSubsystem;

// Fully-realized Echo actor. Pooled — spawned only near players or for scripted sequences.
//
// Lifecycle:
//   Spawn → EnterPool()          (dormant: hidden, no collision, no tick, no AI)
//   EnterPool → InitFromSoA()    (active:  position/velocity seeded, AI running, replicated)
//   InitFromSoA → WriteBackToSoA → EnterPool()   (demoted back to horde)
//
// SourceIndex always mirrors the SoA row. INDEX_NONE when pooled.
// Server owns simulation. Clients drive visuals via CMC replication.
UCLASS()
class ALLTHATREMAINS_API AATR_ActiveEcho : public ACharacter
{
	GENERATED_BODY()

public:
	AATR_ActiveEcho();

	// --- Components ---

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UAIPerceptionComponent> AIPerception;

	// Assign a StateTree asset to this component in your Blueprint subclass.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UStateTreeComponent> StateTreeComp;

	// --- State ---

	// SoA row this Actor was promoted from. INDEX_NONE when pooled.
	int32 SourceIndex = INDEX_NONE;

	// Local cache of Sub->AnimState[SourceIndex]. Flushed to SoA on demotion.
	// Drive your AnimBlueprint from this.
	UPROPERTY(BlueprintReadOnly, Category = "Echo")
	uint8 AnimStateCache = 0;

	// --- Lifecycle ---

	// Put actor in dormant pool state: hidden, collision off, AI off, CMC stopped.
	// Called immediately after pool spawn and after every demotion.
	void EnterPool();

	// Seed position + velocity + animstate from SoA row, then activate all systems.
	// Called by UATR_EchoSubsystem::PromoteEcho — do not call directly.
	void InitFromSoA(const UATR_EchoSubsystem* Sub, int32 Index);

	// Flush live position, velocity, and animstate back into SoA row.
	// Called by UATR_EchoSubsystem::DemoteEcho before returning to pool.
	void WriteBackToSoA(UATR_EchoSubsystem* Sub) const;

protected:
	virtual void BeginPlay() override;

public:
	virtual void Tick(float DeltaTime) override;
};
