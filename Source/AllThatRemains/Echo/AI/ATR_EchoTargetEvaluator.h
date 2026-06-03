// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeEvaluatorBase.h"
#include "ATR_EchoTargetEvaluator.generated.h"

// Evaluator instance data. TargetActor is the bindable output — wire it to
// FATR_EchoMoveToTask.TargetActor in the StateTree editor.
USTRUCT(BlueprintType)
struct FATR_EchoTargetEvaluatorInstanceData
{
	GENERATED_BODY()

	// Output — bind this to EchoMoveToTask.TargetActor.
	UPROPERTY(EditAnywhere, Category = "Output")
	TObjectPtr<AActor> TargetActor = nullptr;
};

// Reads CurrentTarget from AATR_EchoAIController each StateTree tick and exposes it
// as a bindable property. Requires Context Actor = AIController in the StateTree asset.
USTRUCT(BlueprintType, meta = (DisplayName = "Echo Target"))
struct FATR_EchoTargetEvaluator : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FATR_EchoTargetEvaluatorInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	// Writes the controller's CurrentTarget into TargetActor every tick.
	virtual void Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};
