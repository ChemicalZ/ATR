// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Components/StateTreeComponent.h"
#include "ATR_EchoAIController.generated.h"

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
	AActor* GetCurrentTarget() const { return CurrentTarget.Get(); }

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

	// Score all currently known (sight) actors and return the highest-scoring one.
	// Returns nullptr if nothing is perceived.
	AActor* SelectBestTarget() const;

	// Last selected target. Weak so destroyed actors clear automatically.
	TWeakObjectPtr<AActor> CurrentTarget;
};
