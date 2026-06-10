// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "../ATR_EchoCombatTypes.h"
#include "ATR_EchoMeleeTask.generated.h"

// Instance data. Bind Target to the Echo Intent evaluator's ConfirmedVisibleActor so the melee task
// always acts on the genuinely-visible target. The grab/cooldown bookkeeping is internal.
USTRUCT(BlueprintType)
struct FATR_EchoMeleeTaskInstanceData
{
	GENERATED_BODY()

	// The confirmed-visible target to grab/bite. Bind from the intent evaluator.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	TObjectPtr<AActor> Target = nullptr;

	// Internal grab state + cooldown clocks (not designer-bound).
	UPROPERTY(Transient) bool  bGrabbed      = false;
	UPROPERTY(Transient) float LastGrabTime  = -1000.f;
	UPROPERTY(Transient) float LastBiteTime  = -1000.f;
	UPROPERTY(Transient) float GrabStartTime = -1.f;
	UPROPERTY(Transient) EATR_EchoGripType Grip = EATR_EchoGripType::None;
};

// Melee grab → bite → pull, layered ON TOP of the chase. This task NEVER stops (always Running) so
// the state's move task keeps driving the echo forward — momentum is preserved. Each tick:
//   - within GrabRange  → attempt a grab (TryGrabTarget); on success, hold it and block demotion;
//   - while grabbed     → pull the target in (PullTarget) and bite (TryBiteTarget) on a cadence;
//   - target slips away → release the grab.
//
// This task only drives CADENCE (when to attempt). All combat RESOLUTION — success/failure,
// grip strength, wounds, failed-grab scratches, pull physics — is C++-owned by AATR_ActiveEcho
// and logged to LogATR_EchoCombat. Blueprint only receives cosmetic BP_On* events from the pawn.
//
// Context owner must be AATR_EchoAIController; its pawn must be an AATR_ActiveEcho. Place this task
// on the Attack state alongside ATR_EchoMoveRequestTask.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Melee (Grab/Bite/Pull)"))
struct FATR_EchoMeleeTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoMeleeTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	virtual void                ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};
