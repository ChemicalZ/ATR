// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "ATR_EchoMoveToTask.generated.h"

// Instance data — one copy per active StateTree execution. Bindings in the editor link here.
USTRUCT(BlueprintType)
struct FATR_EchoMoveToTaskInstanceData
{
	GENERATED_BODY()

	// Move toward this actor if set; otherwise use TargetLocation.
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<AActor> TargetActor = nullptr;

	UPROPERTY(EditAnywhere, Category = "Input")
	FVector TargetLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Parameter", meta = (ClampMin = 0.f, ForceUnits = "cm"))
	float AcceptanceRadius = 50.f;
};

// StateTree task that drives an AAIController to move to an actor or location.
//
// EnterState  — issues the move request; returns Succeeded if already at goal, Failed if no path.
// Tick        — polls path following status; returns Succeeded when movement ends.
// ExitState   — always stops movement so interrupted states don't keep the controller walking.
//
// Context owner must be AAIController (or a subclass). Set Context Actor = AIController in the
// StateTree asset.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Move To"))
struct FATR_EchoMoveToTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoMoveToTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	virtual void                ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};
