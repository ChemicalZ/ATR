// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoMoveToTask.h"
#include "ATR_EchoAILog.h"
#include "AIController.h"
#include "Navigation/PathFollowingComponent.h"
#include "StateTreeExecutionContext.h"
#include "Logging/StructuredLog.h"

EStateTreeRunStatus FATR_EchoMoveToTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveTask_EnterState);
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC)
	{
		UE_LOG(LogATR_EchoAI, Warning, TEXT("MoveTask::EnterState — owner is not AAIController"));
		return EStateTreeRunStatus::Failed;
	}

	EPathFollowingRequestResult::Type Result;
	if (InstanceData.TargetActor)
	{
		UE_LOGFMT(LogATR_EchoAI, VeryVerbose, "MoveTask::EnterState — {Owner} moving to actor {Target}",
			AIC->GetName(), InstanceData.TargetActor->GetName());
		Result = AIC->MoveToActor(InstanceData.TargetActor, InstanceData.AcceptanceRadius);
	}
	else
	{
		UE_LOGFMT(LogATR_EchoAI, VeryVerbose, "MoveTask::EnterState — {Owner} moving to location {X} {Y} {Z}",
			AIC->GetName(),
			InstanceData.TargetLocation.X, InstanceData.TargetLocation.Y, InstanceData.TargetLocation.Z);
		Result = AIC->MoveToLocation(InstanceData.TargetLocation, InstanceData.AcceptanceRadius);
	}

	switch (Result)
	{
		case EPathFollowingRequestResult::AlreadyAtGoal:
			UE_LOGFMT(LogATR_EchoAI, VeryVerbose, "MoveTask::EnterState — {Owner} already at goal", AIC->GetName());
			return EStateTreeRunStatus::Succeeded;

		case EPathFollowingRequestResult::Failed:
			UE_LOGFMT(LogATR_EchoAI, VeryVerbose, "MoveTask::EnterState — {Owner} move request failed (no path?)", AIC->GetName());
			return EStateTreeRunStatus::Failed;

		default:
			return EStateTreeRunStatus::Running;
	}
}

EStateTreeRunStatus FATR_EchoMoveToTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveTask_Tick);
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	switch (AIC->GetMoveStatus())
	{
		case EPathFollowingStatus::Moving:
		case EPathFollowingStatus::Waiting:
		case EPathFollowingStatus::Paused:
			return EStateTreeRunStatus::Running;

		case EPathFollowingStatus::Idle:
		default:
			UE_LOGFMT(LogATR_EchoAI, VeryVerbose, "MoveTask::Tick — {Owner} movement complete (Idle)", AIC->GetName());
			return EStateTreeRunStatus::Succeeded;
	}
}

void FATR_EchoMoveToTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveTask_ExitState);
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC) return;

	UE_LOGFMT(LogATR_EchoAI, VeryVerbose, "MoveTask::ExitState — {Owner} stopping movement", AIC->GetName());
	AIC->StopMovement();
}
