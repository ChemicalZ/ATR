#include "ZombieMoveToTask.h"
#include "AIController.h"
#include "VisualLogger/VisualLogger.h"
#include "StateTreeExecutionContext.h"

EStateTreeRunStatus FZombieMoveToTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());

	if (!AIC) return EStateTreeRunStatus::Failed;

	// Trigger the move command immediately on entry
	if (InstanceData.TargetActor)
	{
		AIC->MoveToActor(InstanceData.TargetActor, InstanceData.AcceptanceRadius);
	}
	else
	{
		AIC->MoveToLocation(InstanceData.TargetLocation, InstanceData.AcceptanceRadius);
	}

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FZombieMoveToTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	// Check the AI's pathfollowing status
	EPathFollowingStatus::Type Status = AIC->GetMoveStatus();

	if (Status == EPathFollowingStatus::Idle)
	{
		// We reached the destination or the path failed
		return EStateTreeRunStatus::Succeeded;
	}

	return EStateTreeRunStatus::Running;
}