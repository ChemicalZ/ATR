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
	// Replicated so clients can register in their local IndexToActor and suppress ISM.
	UPROPERTY(ReplicatedUsing = OnRep_SourceIndex)
	int32 SourceIndex = INDEX_NONE;

	// Local cache of Sub->AnimState[SourceIndex]. Flushed to SoA on demotion.
	// Drive your AnimBlueprint from this.
	UPROPERTY(ReplicatedUsing = OnRep_AnimStateCache, BlueprintReadOnly, Category = "Echo")
	uint8 AnimStateCache = 0;

	// Set true by StateTree tasks that must not be interrupted (e.g., grab, death sequence).
	// RunPromotionPass skips demotion while this is true.
	// StateTree is responsible for clearing it in ExitState; EnterPool resets it as a safety net.
	UPROPERTY(BlueprintReadWrite, Category = "Echo")
	bool bBlockDemotion = false;

	// --- Mesh Transform Offsets ---
	// Set in Blueprint class defaults to correct pivot and facing mismatches between
	// the ISM representation and the skeletal mesh. Applied once in BeginPlay.

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Echo|Mesh")
	FVector MeshLocationOffset = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Echo|Mesh")
	FRotator MeshRotationOffset = FRotator::ZeroRotator;

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

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void BeginPlay() override;

	// Previous SourceIndex on this machine — lets OnRep unregister the old slot before registering the new one.
	int32 ClientPrevSourceIndex = INDEX_NONE;

	UFUNCTION() void OnRep_SourceIndex();
	UFUNCTION() void OnRep_AnimStateCache();

public:
	virtual void Tick(float DeltaTime) override;
};
