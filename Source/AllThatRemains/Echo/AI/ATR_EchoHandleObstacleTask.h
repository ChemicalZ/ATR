// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "../ATR_EchoRuntimeTypes.h"
#include "ATR_EchoHandleObstacleTask.generated.h"

// Instance data — bind MoveRequest/bHasValidMoveRequest from the intent evaluator. When the
// subsystem chose HandleObstacle, MoveRequest is the placeholder sidestep around the obstacle.
USTRUCT(BlueprintType)
struct FATR_EchoHandleObstacleTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	FATR_EchoMoveRequest MoveRequest;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	bool bHasValidMoveRequest = false;
};

// Placeholder obstacle-handling task (Phase 9). Logs the classified obstacle, executes the
// subsystem's sidestep/repath fallback, and returns gracefully so the StateTree falls back to
// search/idle if the obstacle is not resolved. This is the seam where real door/window/fence
// breaking, group pounding, and obstacle audio/FX attach in a later pass — without reworking
// the movement/intent architecture.
//
// Context owner must be AATR_EchoAIController.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Handle Obstacle"))
struct FATR_EchoHandleObstacleTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoHandleObstacleTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	virtual void                ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};
