// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoMoveToTask.h"
#include "AIController.h"
#include "Navigation/PathFollowingComponent.h"
#include "StateTreeExecutionContext.h"

EStateTreeRunStatus FATR_EchoMoveToTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	EPathFollowingRequestResult::Type Result;
	if (InstanceData.TargetActor)
		Result = AIC->MoveToActor(InstanceData.TargetActor, InstanceData.AcceptanceRadius);
	else
		Result = AIC->MoveToLocation(InstanceData.TargetLocation, InstanceData.AcceptanceRadius);

	switch (Result)
	{
		case EPathFollowingRequestResult::AlreadyAtGoal: return EStateTreeRunStatus::Succeeded;
		case EPathFollowingRequestResult::Failed:        return EStateTreeRunStatus::Failed;
		default:                                         return EStateTreeRunStatus::Running;
	}
}

EStateTreeRunStatus FATR_EchoMoveToTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
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
			// Idle after a successful EnterState means the move completed (arrived or path ended).
			// StateTree transitions handle what to do next based on game state.
			return EStateTreeRunStatus::Succeeded;
	}
}

void FATR_EchoMoveToTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	// Always abort — covers interruptions. If the move already finished, StopMovement is a no-op.
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (AIC) AIC->StopMovement();
}
