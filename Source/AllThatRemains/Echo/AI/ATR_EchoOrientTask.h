// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "ATR_EchoOrientTask.generated.h"

// Instance data. Bind TargetLocation to the Echo Intent evaluator's TargetLocation output so the
// echo orients toward the stimulus (heard location / horde-pressure point) without moving.
USTRUCT(BlueprintType)
struct FATR_EchoOrientTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
	FVector TargetLocation = FVector::ZeroVector;
};

// Orient-only task for TurnTowardStimulus: rotates the pawn's yaw toward TargetLocation at a fixed
// rate (Echo|Combat.OrientTurnRateDegPerSec) and never moves it. Returns Running each tick — wire the
// TurnTowardStimulus state's transition as OnTick → Root (like Idle) so the tree re-evaluates while
// the echo turns. Context owner must be AATR_EchoAIController.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Orient (Turn Toward)"))
struct FATR_EchoOrientTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoOrientTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};
