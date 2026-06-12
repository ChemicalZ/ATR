// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GenericTeamAgentInterface.h"
#include "GameFramework/Character.h"
#include "ATR_EchoRuntimeTypes.h"
#include "ATR_ActiveEcho.generated.h"

class UATR_EchoSubsystem;

// Fully-realized Echo actor. Pooled — spawned only near players or for scripted sequences.
// Owns only movement (CMC) and anim state. All AI lives on AATR_EchoAIController.
//
// Lifecycle:
//   Spawn → EnterPool()                           (dormant: hidden, no collision, no tick)
//   PromoteEcho: InitFromSoA()                    (teleport to SoA position, seed velocity)
//              → Controller->Possess(this)        (OnPossess → AI wakes at correct location)
//   DemoteEcho: WriteBackToSoA → EnterPool()      (actor returns to pool)
//             → Controller->UnPossess()           (OnUnPossess → AI stops, controller returns to pool)
//
// SourceIndex always mirrors the SoA row. INDEX_NONE when pooled.
// Server owns simulation. Clients drive visuals via CMC replication.
UCLASS()
class ALLTHATREMAINS_API AATR_ActiveEcho : public ACharacter, public IGenericTeamAgentInterface
{
	GENERATED_BODY()

public:
	AATR_ActiveEcho();
	
	virtual FGenericTeamId GetGenericTeamId() const override { return FGenericTeamId(TeamNumber); }
	virtual void SetGenericTeamId(const FGenericTeamId& NewId) override { TeamNumber = NewId.GetId(); }

	// --- State ---

	// SoA row this Actor was promoted from. INDEX_NONE when pooled.
	// Replicated so clients can register in their local IndexToActor and suppress ISM.
	UPROPERTY(ReplicatedUsing = OnRep_SourceIndex)
	int32 SourceIndex = INDEX_NONE;

	// Local cache of Sub->AnimState[SourceIndex]. Flushed to SoA on demotion.
	// Drive your AnimBlueprint from this.
	UPROPERTY(ReplicatedUsing = OnRep_AnimStateCache, BlueprintReadOnly, Category = "Echo")
	uint8 AnimStateCache = 0;

	// PRESENTATION-ONLY canonical intent, replicated so clients can drive animation/FX (chase
	// vs search vs investigate vs obstacle). This is NOT AI memory — clients never make
	// decisions from it; the server owns intent selection. Server-written each intent tick.
	UPROPERTY(ReplicatedUsing = OnRep_EchoIntent, BlueprintReadOnly, Category = "Echo")
	EATR_EchoIntent ReplicatedIntent = EATR_EchoIntent::Idle;

	// Server-only setter — updates ReplicatedIntent (replicates to clients on change).
	void SetEchoIntentForPresentation(EATR_EchoIntent NewIntent);

	// Set true by StateTree tasks that must not be interrupted (e.g., grab, death sequence).
	// RunPromotionPass skips demotion while this is true.
	// StateTree is responsible for clearing it in ExitState; EnterPool resets it as a safety net.
	UPROPERTY(BlueprintReadWrite, Category = "Echo")
	bool bBlockDemotion = false;

	// --- Combat hooks ---
	// Called by the melee StateTree task (FATR_EchoMeleeTask). BlueprintNativeEvent so designers can
	// override the real effect (anim montage, attach, damage, root-motion pull). The native defaults
	// are intentionally minimal so the grab→bite→pull flow works before art/gameplay is wired.

	// Attempt to grab Target. Return true if the grab "takes" (the task then holds the grab, bites,
	// and pulls). Default: succeeds. Override to gate on facing/animation/anti-spam.
	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Combat")
	bool TryGrabTarget(AActor* Target);

	// Attempt a bite on Target (already grabbed, within bite range). Return true if it landed.
	// Default: succeeds. Override to apply damage / play the bite montage.
	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Combat")
	bool TryBiteTarget(AActor* Target);

	// Pull Target toward this echo while grabbed. Default: no-op. Override to apply your pull /
	// root motion / physics constraint. Strength comes from Echo|Combat.MeleePullStrength.
	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Combat")
	void PullTarget(AActor* Target, float Strength);

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
	UFUNCTION() void OnRep_EchoIntent();
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Team")
	uint8 TeamNumber = 2;

public:
	virtual void Tick(float DeltaTime) override;
};
