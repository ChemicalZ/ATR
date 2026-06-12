// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoIntentEvaluator.h"
#include "ATR_EchoAIController.h"
#include "../ATR_EchoSubsystem.h"
#include "StateTreeExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

void FATR_EchoIntentEvaluator::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_IntentEvaluator_Tick);

	FInstanceDataType& Data = Context.GetInstanceData(*this);

	// Reset to a safe "no move" every tick so a missing subsystem/state can never leave
	// stale outputs driving the StateTree.
	Data.Intent               = EATR_EchoIntent::Idle;
	Data.MoveRequest          = FATR_EchoMoveRequest{};
	Data.ConfirmedVisibleActor = nullptr;
	Data.TargetLocation       = FVector::ZeroVector;
	Data.AcceptanceRadius     = 50.f;
	Data.bHasValidMoveRequest = false;

	const AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC) return;

	const UWorld* W = AIC->GetWorld();
	const UATR_EchoSubsystem* Sub = W ? W->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	if (!Sub) return;

	const FATR_EchoRuntimeState* State = Sub->GetEchoState(AIC->GetEchoId());
	if (!State) return;

	Data.Intent           = State->Intent;
	Data.MoveRequest      = State->Movement.Request;
	Data.AcceptanceRadius = State->Movement.Request.AcceptanceRadius;

	// Always mirror the request location so orient-only intents (TurnTowardStimulus) can bind it as
	// the point to turn toward, even though they don't path-move.
	Data.TargetLocation = State->Movement.Request.Location;

	switch (State->Movement.Request.Type)
	{
		case EATR_EchoMoveTargetType::Actor:
			// Surface the actor only when sight is genuinely current — never chase stale memory.
			// Both ChaseVisibleActor and Attack keep moving toward the live target.
			if ((State->Intent == EATR_EchoIntent::ChaseVisibleActor || State->Intent == EATR_EchoIntent::Attack)
				&& State->Awareness.bHasCurrentLineOfSight)
			{
				Data.ConfirmedVisibleActor = State->Movement.Request.Actor.Get();
				Data.bHasValidMoveRequest  = (Data.ConfirmedVisibleActor != nullptr);
			}
			break;

		case EATR_EchoMoveTargetType::Location:
			Data.bHasValidMoveRequest = true;
			break;

		case EATR_EchoMoveTargetType::None:
		default:
			Data.bHasValidMoveRequest = false; // orient-only / idle intents do not path-move
			break;
	}
}
