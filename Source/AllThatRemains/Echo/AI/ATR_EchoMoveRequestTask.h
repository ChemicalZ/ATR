// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "../ATR_EchoRuntimeTypes.h"
#include "ATR_EchoMoveRequestTask.generated.h"

// Instance data — bind MoveRequest (and bHasValidMoveRequest) from the Echo Intent
// evaluator outputs in the StateTree editor.
USTRUCT(BlueprintType)
struct FATR_EchoMoveRequestTaskInstanceData
{
	GENERATED_BODY()

	// The explicit, typed move order produced by the subsystem intent evaluator.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	FATR_EchoMoveRequest MoveRequest;

	// Gate flag — false for orient-only / idle intents that must not path-move.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	bool bHasValidMoveRequest = false;
};

// StateTree task that executes a typed FATR_EchoMoveRequest through the Echo controller.
//
// Replaces FATR_EchoMoveToTask. Differences:
//   - Move target is explicit (Actor vs Location) — never the old "null actor → ZeroVector".
//   - None / invalid requests are rejected instead of silently moving to world origin.
//   - The controller tracks the FAIRequestID and reports a classified success/failure to
//     the subsystem; this task only drives StateTree flow (Running until path following ends).
//
// Context owner must be AATR_EchoAIController. Set Context Actor = AIController in the asset.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Move Request"))
struct FATR_EchoMoveRequestTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoMoveRequestTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	virtual void                ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};
