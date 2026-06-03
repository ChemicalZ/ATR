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
//   Spawn → EnterPool()          (dormant: perception off, StateTree stopped)
//   Possess(Echo) → OnPossess()  (active: perception on, StateTree running)
//   UnPossess()  → OnUnPossess() (dormant: perception off, StateTree stopped)
//   Return to ControllerPool
//
// Blueprint subclass: assign the StateTree asset in the Details panel.
// Perception sense parameters are tunable in Blueprint CDO overrides.
UCLASS()
class ALLTHATREMAINS_API AATR_EchoAIController : public AAIController
{
	GENERATED_BODY()

public:
	AATR_EchoAIController();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UAIPerceptionComponent> AIPerception;

	// Assign a StateTree asset to this component in your Blueprint subclass.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UStateTreeComponent> StateTreeComp;

	// Stop all AI logic and prepare controller for pool. Called after UnPossess.
	void EnterPool();

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
};
