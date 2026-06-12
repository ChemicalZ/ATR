// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "../ATR_EchoRuntimeTypes.h"
#include "ATR_EchoHandleObstacleTask.generated.h"

// Instance data — bind MoveRequest/bHasValidMoveRequest from the intent evaluator. When the
// subsystem chose EngageBarrier, MoveRequest is the direct move to the barrier contact point.
USTRUCT(BlueprintType)
struct FATR_EchoHandleObstacleTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	FATR_EchoMoveRequest MoveRequest;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	bool bHasValidMoveRequest = false;

	// Internal — time of this Echo's last barrier hit (engage or frustrated tap).
	UPROPERTY(Transient)
	float LastHitTime = -1.f;

	// Internal — serial of the legacy sidestep move when bAllowActivePursuitTacticalReroute
	// (debug) is enabled. 0 when barrier engagement is active (normal mode).
	UPROPERTY(Transient)
	int32 WaitMoveSerial = 0;

	// Internal — when this engagement entered the state (drives the first-attack delay).
	UPROPERTY(Transient)
	float EnterTime = -1.f;

	// Internal — current frustrated-shuffle target point (zero = pick a new one).
	UPROPERTY(Transient)
	FVector ShuffleTarget = FVector::ZeroVector;
};

// Barrier engagement task (design doc: Engage Barrier / Reach Through Barrier / Frustrated
// Search). While the subsystem keeps Intent == EngageBarrier this task:
//   - faces the barrier and closes into contact range (direct approach — no pathfinding);
//   - presses and attacks on the configured interval (ReportBarrierImpact: damage scaled by
//     group pressure, pounding agitation, impact-noise stimulus that attracts the horde);
//   - in the ReachThrough phase, keeps attacking from contact (the melee task layers
//     grab/claw-through on top when the target enters reach);
//   - in the FrustratedSearch phase, lingers near the barrier with small shuffles and
//     occasional hits.
// It NEVER sidesteps, repaths, or picks an alternate entrance. It resolves Succeeded when
// the subsystem clears the barrier (opened/broken/expired) so the tree re-evaluates into
// pursuit/search/idle.
//
// Context owner must be AATR_EchoAIController.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Engage Barrier"))
struct FATR_EchoHandleObstacleTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoHandleObstacleTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	virtual void                ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};
