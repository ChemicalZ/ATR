// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoMoveRequestTask.h"
#include "ATR_EchoAIController.h"
#include "ATR_EchoAILog.h"
#include "Navigation/PathFollowingComponent.h"
#include "StateTreeExecutionContext.h"

EStateTreeRunStatus FATR_EchoMoveRequestTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveRequestTask_EnterState);

	const FInstanceDataType& Data = Context.GetInstanceData(*this);
	AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC)
	{
		UE_LOG(LogATR_EchoAI, Warning, TEXT("MoveRequestTask::EnterState — owner is not AATR_EchoAIController"));
		return EStateTreeRunStatus::Failed;
	}

	// Reject anything that isn't an explicit, valid move — no implicit origin moves.
	if (!Data.bHasValidMoveRequest || Data.MoveRequest.Type == EATR_EchoMoveTargetType::None)
		return EStateTreeRunStatus::Failed;

	switch (AIC->IssueMoveRequest(Data.MoveRequest))
	{
		case EPathFollowingRequestResult::AlreadyAtGoal:
			return EStateTreeRunStatus::Succeeded;

		case EPathFollowingRequestResult::Failed:
			return EStateTreeRunStatus::Failed;

		default:
			return EStateTreeRunStatus::Running;
	}
}

EStateTreeRunStatus FATR_EchoMoveRequestTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveRequestTask_Tick);

	const AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	// Path following still owns the truth of completion; the controller's HandleMoveCompleted
	// has already reported the classified result to the subsystem by the time we go Idle.
	switch (AIC->GetMoveStatus())
	{
		case EPathFollowingStatus::Moving:
		case EPathFollowingStatus::Waiting:
		case EPathFollowingStatus::Paused:
			return EStateTreeRunStatus::Running;

		case EPathFollowingStatus::Idle:
		default:
			return EStateTreeRunStatus::Succeeded;
	}
}

void FATR_EchoMoveRequestTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveRequestTask_ExitState);

	if (AAIController* AIC = Cast<AAIController>(Context.GetOwner()))
		AIC->StopMovement();
}
