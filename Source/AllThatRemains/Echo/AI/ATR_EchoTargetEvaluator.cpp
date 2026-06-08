// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoTargetEvaluator.h"
#include "ATR_EchoAIController.h"
#include "StateTreeExecutionContext.h"

void FATR_EchoTargetEvaluator::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_TargetEvaluator_Tick);
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	const AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	InstanceData.TargetActor = AIC ? AIC->GetCurrentTarget() : nullptr;
}
