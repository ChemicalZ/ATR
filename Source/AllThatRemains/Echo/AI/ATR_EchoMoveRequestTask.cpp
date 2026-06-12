// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoMoveRequestTask.h"
#include "ATR_EchoAIController.h"
#include "ATR_EchoAILog.h"
#include "Navigation/PathFollowingComponent.h"
#include "StateTreeExecutionContext.h"

EStateTreeRunStatus FATR_EchoMoveRequestTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveRequestTask_EnterState);

	FInstanceDataType& Data = Context.GetInstanceData(*this);
	AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC)
	{
		UE_LOG(LogATR_EchoAI, Warning, TEXT("MoveRequestTask::EnterState — owner is not AATR_EchoAIController"));
		return EStateTreeRunStatus::Failed;
	}

	// Reject anything that isn't an explicit, valid move — no implicit origin moves.
	if (!Data.bHasValidMoveRequest || Data.MoveRequest.Type == EATR_EchoMoveTargetType::None)
		return EStateTreeRunStatus::Failed;

	const EPathFollowingRequestResult::Type Code = AIC->IssueMoveRequest(Data.MoveRequest);

	// Record the serial of the move we just issued so Tick resolves on the matching classified
	// result rather than on path-following status.
	Data.WaitMoveSerial = static_cast<int32>(AIC->GetLastIssuedMoveSerial());

	switch (Code)
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

	const FInstanceDataType& Data = Context.GetInstanceData(*this);
	const AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	// Resolve from the subsystem's classified move result, keyed by the serial we issued. A path
	// that merely went Idle (aborted/off-path) reports a failure and fails this task instead of
	// falsely succeeding; a stale result from a superseded request can never resolve this state.
	switch (AIC->GetMoveOutcomeForSerial(static_cast<uint32>(Data.WaitMoveSerial)))
	{
		case AATR_EchoAIController::EEchoMoveOutcome::Succeeded: return EStateTreeRunStatus::Succeeded;
		case AATR_EchoAIController::EEchoMoveOutcome::Failed:    return EStateTreeRunStatus::Failed;
		case AATR_EchoAIController::EEchoMoveOutcome::Pending:
		default:                                                return EStateTreeRunStatus::Running;
	}
}

void FATR_EchoMoveRequestTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MoveRequestTask_ExitState);

	if (AAIController* AIC = Cast<AAIController>(Context.GetOwner()))
		AIC->StopMovement();
}
