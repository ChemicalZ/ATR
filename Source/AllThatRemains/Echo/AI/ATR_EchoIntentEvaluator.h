// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeEvaluatorBase.h"
#include "../ATR_EchoRuntimeTypes.h"
#include "ATR_EchoIntentEvaluator.generated.h"

// Bindable outputs of the intent evaluator. The StateTree binds these to its
// transitions and to FATR_EchoMoveRequestTask:
//   - Intent               → drive state selection (chase / search / investigate / obstacle / idle).
//   - MoveRequest          → the whole typed move order (preferred single binding).
//   - ConfirmedVisibleActor→ set ONLY when Intent == ChaseVisibleActor and LOS is current.
//   - TargetLocation       → set for location-based intents (lost sight, search, hearing, horde, obstacle).
//   - AcceptanceRadius     → mirrored from the move request for convenience.
//   - bHasValidMoveRequest → false when the Echo should not path-move (e.g. TurnTowardStimulus/Idle).
USTRUCT(BlueprintType)
struct FATR_EchoIntentEvaluatorInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	EATR_EchoIntent Intent = EATR_EchoIntent::Idle;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FATR_EchoMoveRequest MoveRequest;

	// Non-null only while a target is genuinely visible. Never reflects stale sight memory.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	TObjectPtr<AActor> ConfirmedVisibleActor = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FVector TargetLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	float AcceptanceRadius = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bHasValidMoveRequest = false;
};

// Reads the subsystem-owned canonical intent/move-request for the possessed Echo and
// exposes it as bindable StateTree outputs. Replaces FATR_EchoTargetEvaluator, which only
// surfaced a raw (possibly stale) TargetActor.
//
// Requires Context Actor = AIController in the StateTree asset (owner must be
// AATR_EchoAIController). Pure read — never mutates subsystem state.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Intent"))
struct FATR_EchoIntentEvaluator : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoIntentEvaluatorInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual void Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};
