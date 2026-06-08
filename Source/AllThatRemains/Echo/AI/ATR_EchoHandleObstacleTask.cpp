// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoHandleObstacleTask.h"
#include "ATR_EchoAIController.h"
#include "ATR_EchoAILog.h"
#include "../ATR_EchoSubsystem.h"
#include "Navigation/PathFollowingComponent.h"
#include "StateTreeExecutionContext.h"

EStateTreeRunStatus FATR_EchoHandleObstacleTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandleObstacleTask_EnterState);

	const FInstanceDataType& Data = Context.GetInstanceData(*this);
	AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	// Debug: surface which obstacle triggered this so designers can see the hook firing.
	if (const UWorld* W = AIC->GetWorld())
	{
		if (const UATR_EchoSubsystem* Sub = W->GetSubsystem<UATR_EchoSubsystem>())
		{
			if (const FATR_EchoRuntimeState* State = Sub->GetEchoState(AIC->GetEchoId()))
			{
				UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("HandleObstacle — EchoId %d reason %d at %s"),
					AIC->GetEchoId(), static_cast<int32>(State->Obstacle.Reason),
					*State->Obstacle.ObstacleLocation.ToCompactString());
			}
		}
	}

	// Placeholder fallback: execute the sidestep/repath the subsystem produced. If there is no
	// valid fallback move, fail gracefully so the StateTree returns to search/idle.
	if (!Data.bHasValidMoveRequest || Data.MoveRequest.Type == EATR_EchoMoveTargetType::None)
		return EStateTreeRunStatus::Failed;

	switch (AIC->IssueMoveRequest(Data.MoveRequest))
	{
		case EPathFollowingRequestResult::AlreadyAtGoal: return EStateTreeRunStatus::Succeeded;
		case EPathFollowingRequestResult::Failed:        return EStateTreeRunStatus::Failed;
		default:                                         return EStateTreeRunStatus::Running;
	}
}

EStateTreeRunStatus FATR_EchoHandleObstacleTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandleObstacleTask_Tick);

	const AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	switch (AIC->GetMoveStatus())
	{
		case EPathFollowingStatus::Moving:
		case EPathFollowingStatus::Waiting:
		case EPathFollowingStatus::Paused:
			return EStateTreeRunStatus::Running;

		case EPathFollowingStatus::Idle:
		default:
			return EStateTreeRunStatus::Succeeded; // resolved-or-not, hand control back to the tree
	}
}

void FATR_EchoHandleObstacleTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandleObstacleTask_ExitState);

	if (AAIController* AIC = Cast<AAIController>(Context.GetOwner()))
		AIC->StopMovement();
}
