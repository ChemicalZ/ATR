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

	FInstanceDataType& Data = Context.GetInstanceData(*this);
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

	// Execute the sidestep/repath fallback the subsystem produced. If there is no valid fallback
	// move, fail so the StateTree returns to search/idle.
	if (!Data.bHasValidMoveRequest || Data.MoveRequest.Type == EATR_EchoMoveTargetType::None)
		return EStateTreeRunStatus::Failed;

	const EPathFollowingRequestResult::Type Code = AIC->IssueMoveRequest(Data.MoveRequest);
	Data.WaitMoveSerial = static_cast<int32>(AIC->GetLastIssuedMoveSerial());

	switch (Code)
	{
		case EPathFollowingRequestResult::AlreadyAtGoal: return EStateTreeRunStatus::Succeeded;
		case EPathFollowingRequestResult::Failed:        return EStateTreeRunStatus::Failed;
		default:                                         return EStateTreeRunStatus::Running;
	}
}

EStateTreeRunStatus FATR_EchoHandleObstacleTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandleObstacleTask_Tick);

	const FInstanceDataType& Data = Context.GetInstanceData(*this);
	const AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	// Resolve from the classified result of the sidestep/repath, keyed by the issued serial.
	// Either outcome hands control back to the tree via transitions (success → resume pursuit/
	// search; failure → fall back to search/idle); it is never inferred from path-Idle alone.
	switch (AIC->GetMoveOutcomeForSerial(static_cast<uint32>(Data.WaitMoveSerial)))
	{
		case AATR_EchoAIController::EEchoMoveOutcome::Succeeded: return EStateTreeRunStatus::Succeeded;
		case AATR_EchoAIController::EEchoMoveOutcome::Failed:    return EStateTreeRunStatus::Failed;
		case AATR_EchoAIController::EEchoMoveOutcome::Pending:
		default:                                                return EStateTreeRunStatus::Running;
	}
}

void FATR_EchoHandleObstacleTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandleObstacleTask_ExitState);

	if (AAIController* AIC = Cast<AAIController>(Context.GetOwner()))
		AIC->StopMovement();
}
