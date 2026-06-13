// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoMeleeTask.h"
#include "ATR_EchoAIController.h"
#include "../ATR_ActiveEcho.h"
#include "../ATR_EchoSettings.h"
#include "StateTreeExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

EStateTreeRunStatus FATR_EchoMeleeTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.bGrabbed     = false;
	Data.LastGrabTime = -1000.f;
	Data.LastBiteTime = -1000.f;
	Data.GrabStartTime = -1.f;

	// Safety net: a prior task instance may have leaked bBlockDemotion=true
	// (e.g. StateTree force-exited without ExitState firing cleanly). Clear it
	// on every fresh enter so the demotion guard never strands across attempts.
	if (AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner()))
		if (AATR_ActiveEcho* Pawn = Cast<AATR_ActiveEcho>(AIC->GetPawn()))
			Pawn->bBlockDemotion = false;

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FATR_EchoMeleeTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MeleeTask_Tick);

	FInstanceDataType& Data = Context.GetInstanceData(*this);

	AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	AATR_ActiveEcho* Pawn = AIC ? Cast<AATR_ActiveEcho>(AIC->GetPawn()) : nullptr;
	if (!Pawn)
		return EStateTreeRunStatus::Running; // can't act this frame; keep the move task driving

	auto ReleaseGrab = [&]()
	{
		if (Data.bGrabbed)
		{
			Data.bGrabbed = false;
			Pawn->bBlockDemotion = false;
			Pawn->NotifyGrabReleased(); // clears CurrentGrip (replicated)
		}
	};

	AActor* Target = Data.Target;
	if (!IsValid(Target))
	{
		ReleaseGrab();
		return EStateTreeRunStatus::Running;
	}

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	if (!S || !S->bEnableMeleeAttack)
	{
		ReleaseGrab();
		return EStateTreeRunStatus::Running;
	}

	const UWorld* W = Pawn->GetWorld();
	const float Now  = W ? W->GetTimeSeconds() : 0.f;
	const float Dist = FVector::Dist2D(Pawn->GetActorLocation(), Target->GetActorLocation());

	// Grab attempt — within arm's length, off cooldown. The echo keeps moving forward regardless.
	if (!Data.bGrabbed && Dist <= S->GrabRange && (Now - Data.LastGrabTime) >= S->GrabCooldownSeconds)
	{
		Data.LastGrabTime = Now;
		if (Pawn->TryGrabTarget(Target))
		{
			Data.bGrabbed = true;
			Data.GrabStartTime = Now;
			Pawn->bBlockDemotion = true; // don't demote mid-grab
		}
	}

	// While grabbed — pull the target in and bite on a cadence; release if it slips away,
	// the grip got cleared externally (e.g. structural damage broke both arms), or the
	// grab has been held too long (anti-strand: max grab duration).
	if (Data.bGrabbed)
	{
		// External grip loss (capability change cleared CurrentGrip): bail out cleanly.
		if (Pawn->CurrentGrip == EATR_EchoGripType::None)
		{
			ReleaseGrab();
			return EStateTreeRunStatus::Running;
		}

		// Anti-strand: cap any single grab to MaxGrabHoldSeconds so a stuck grab can't
		// hold bBlockDemotion forever (e.g. target stays in BadAngle/cone-fail loop).
		if (Data.GrabStartTime > 0.f && (Now - Data.GrabStartTime) > S->MaxGrabHoldSeconds)
		{
			ReleaseGrab();
			return EStateTreeRunStatus::Running;
		}

		Pawn->PullTarget(Target, S->MeleePullStrength);

		if (Dist <= S->BiteRange && (Now - Data.LastBiteTime) >= S->BiteCooldownSeconds)
		{
			Data.LastBiteTime = Now;
			Pawn->TryBiteTarget(Target);
		}

		if (Dist > S->GrabRange * S->GrabReleaseMultiplier)
			ReleaseGrab();
	}

	return EStateTreeRunStatus::Running; // never completes — chase + attack run together
}

void FATR_EchoMeleeTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	if (Data.bGrabbed)
	{
		if (AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner()))
			if (AATR_ActiveEcho* Pawn = Cast<AATR_ActiveEcho>(AIC->GetPawn()))
			{
				Pawn->bBlockDemotion = false; // always release the demotion block on exit
				Pawn->NotifyGrabReleased();   // clears CurrentGrip (replicated)
			}
		Data.bGrabbed = false;
	}
}
