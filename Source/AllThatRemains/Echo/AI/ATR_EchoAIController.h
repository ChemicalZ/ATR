// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Components/StateTreeComponent.h"
#include "ATR_EchoAIController.generated.h"

class UATR_EchoSubsystem;

// Pooled AI controller for AATR_ActiveEcho.
//
// Owns all AI — perception and behavior. The character owns only movement.
//
// Lifecycle mirrors the actor pool:
//   Spawn → EnterPool()          (dormant: perception off, StateTree stopped, delegate unbound)
//   Possess(Echo) → OnPossess()  (active: perception on, delegate bound, StateTree running)
//   UnPossess()  → OnUnPossess() (stops movement, unbinds delegate, dormant)
//   Return to ControllerPool
//
// Team: Echoes are Team 2 (see TeamNumber default below). Sight/hearing ignore friendlies,
// so echoes never perceive each other. Players have no team (NoTeam) and are treated as
// neutral → detected. The controller and the possessed AATR_ActiveEcho must share this team
// id for friendly-filtering to work; both default to 2.
UCLASS()
class ALLTHATREMAINS_API AATR_EchoAIController : public AAIController
{
	GENERATED_BODY()

public:
	AATR_EchoAIController();

	virtual FGenericTeamId GetGenericTeamId() const override { return FGenericTeamId(TeamNumber); }
	virtual void SetGenericTeamId(const FGenericTeamId& NewId) override { TeamNumber = NewId.GetId(); }

	// --- Components ---

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UAIPerceptionComponent> AIPerception;

	// Assign a StateTree asset to this component in your Blueprint subclass.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UStateTreeComponent> StateTreeComp;

	// --- Config ---

	// Multiplier applied to a target's score when it is already CurrentTarget.
	// Prevents flickering when two targets score nearly equal.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Echo|AI", meta = (ClampMin = 1.f))
	float LoyaltyBonusMultiplier = 1.2f;

	// Returns the current best target selected by perception scoring. Used by StateTree evaluators.
	// TRANSITIONAL (Phases 2–4): superseded by subsystem intent; removed in final cleanup.
	AActor* GetCurrentTarget() const { return CurrentTarget.Get(); }

	// Stable EchoId of the possessed Echo, resolved at OnPossess. INDEX_NONE when pooled.
	int32 GetEchoId() const { return CachedEchoId; }

	// --- Pool ---

	// Stop all AI logic and clear state. Called after UnPossess when returning to pool.
	void EnterPool();

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Team")
	uint8 TeamNumber = 2;
private:
	// Batch perception callback — fires once per perception tick after all stimuli are processed.
	UFUNCTION()
	void HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors);

	// Walk this update's perceived actors and report sight/hearing facts into the
	// subsystem's canonical awareness state. Hearing is reported as location-only.
	void ReportPerceptionFacts(const TArray<AActor*>& UpdatedActors);

	// Enable/disable the perception senses and (on disable) forget all perception memory.
	// Keeps a pooled controller from carrying stale stimuli into its next possession.
	void SetSensesEnabled(bool bEnabled);

	// DEPRECATED (transitional): local target scoring. Retained only so the legacy
	// FATR_EchoTargetEvaluator keeps working until Phase 3 makes subsystem intent
	// authoritative. Removed in the final cleanup pass.
	// Score all currently known (sight) actors and return the highest-scoring one.
	// Returns nullptr if nothing is perceived.
	AActor* SelectBestTarget() const;

	// Resolved once at OnPossess from the possessed AATR_ActiveEcho's SoA row.
	// Stable for the Echo's lifetime (EchoId never changes; SoA index can).
	int32 CachedEchoId = INDEX_NONE;

	// World subsystem cache — world subsystems outlive controllers, so a raw observer
	// pointer is safe. Cleared on unpossess/pool to avoid use across worlds.
	UATR_EchoSubsystem* CachedSubsystem = nullptr;

	// Last selected target. Weak so destroyed actors clear automatically.
	// TRANSITIONAL (Phase 1): this is controller-owned canonical target state. It will be
	// superseded by subsystem-owned FATR_EchoRuntimeState awareness/intent in Phases 2–4
	// and removed in the final cleanup pass. Do not build new behavior on it.
	TWeakObjectPtr<AActor> CurrentTarget;
};
