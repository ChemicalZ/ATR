// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoMeleeTask.h"
#include "ATR_EchoAIController.h"
#include "ATR_EchoAILog.h"
#include "../ATR_ActiveEcho.h"
#include "../ATR_EchoSettings.h"
#include "../../Player/ATR_Player.h"
#include "StateTreeExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

EStateTreeRunStatus FATR_EchoMeleeTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	Data.bGrabbed      = false;
	Data.LastGrabTime  = -1000.f;
	Data.LastBiteTime  = -1000.f;
	Data.GrabStartTime = -1.f;
	Data.Grip          = EATR_EchoGripType::None;

	// Safety net: a prior task instance may have leaked bBlockDemotion=true if StateTree
	// force-exited without ExitState firing cleanly. Clear it on fresh enter.
	if (AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner()))
		if (AATR_ActiveEcho* Pawn = Cast<AATR_ActiveEcho>(AIC->GetPawn()))
			Pawn->bBlockDemotion = false;

	UE_LOG(LogATR_EchoAI, Verbose, TEXT("EchoMelee: enter attack state"));
	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FATR_EchoMeleeTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_MeleeTask_Tick);

	FInstanceDataType& Data = Context.GetInstanceData(*this);

	AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	AATR_ActiveEcho* Pawn = AIC ? Cast<AATR_ActiveEcho>(AIC->GetPawn()) : nullptr;
	if (!Pawn)
	{
		// Designed concurrent behavior: the move task drives even when this task
		// can't act yet (waiting on possess). Visible at VeryVerbose so a stuck
		// echo "in Melee but unpossessed" doesn't have to be guessed at.
		UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("EchoMelee[%d]: tick without pawn — skipping"),
			AIC ? AIC->GetEchoId() : INDEX_NONE);
		return EStateTreeRunStatus::Running;
	}

	const int32 EchoId = AIC ? AIC->GetEchoId() : INDEX_NONE;

	auto ReleaseGrab = [&](const TCHAR* Why)
	{
		if (Data.bGrabbed)
		{
			Data.bGrabbed = false;
			Data.Grip     = EATR_EchoGripType::None;
			Pawn->bBlockDemotion = false;
			Pawn->NotifyGrabReleased();
			if (AATR_Player* GrabbedPlayer = Cast<AATR_Player>(Data.Target))
				GrabbedPlayer->UnregisterGrab(Pawn);
			UE_LOG(LogATR_EchoAI, Verbose, TEXT("EchoMelee[%d]: RELEASE grab (%s)"), EchoId, Why);
		}
	};

	AActor* Target = Data.Target;
	if (!IsValid(Target))
	{
		ReleaseGrab(TEXT("target invalid"));
		return EStateTreeRunStatus::Running;
	}

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	if (!S || !S->bEnableMeleeAttack)
	{
		ReleaseGrab(TEXT("melee disabled"));
		return EStateTreeRunStatus::Running;
	}

	const UWorld* W = Pawn->GetWorld();
	const float Now  = W ? W->GetTimeSeconds() : 0.f;
	const float Dist = FVector::Dist2D(Pawn->GetActorLocation(), Target->GetActorLocation());

	// Barged: a shoulder-charge knocked this echo aside (resolved in AATR_ActiveEcho::NotifyHit).
	// The grip is physically broken and the echo is too staggered to grab until it recovers.
	if (Pawn->IsBargeStaggered(Now))
	{
		ReleaseGrab(TEXT("barged"));
		return EStateTreeRunStatus::Running; // recover, then resume grabbing
	}

	// Grab attempt - within arm's length, off cooldown. The echo keeps moving forward regardless.
	// Outcome (incl. failed-grab scratches) is resolved + logged by the pawn in LogATR_EchoCombat.
	if (!Data.bGrabbed && Dist <= S->GrabRange && (Now - Data.LastGrabTime) >= S->GrabCooldownSeconds)
	{
		Data.LastGrabTime = Now;
		if (Pawn->TryGrabTarget(Target))
		{
			Data.bGrabbed      = true;
			Data.GrabStartTime = Now;
			Data.Grip          = Pawn->CurrentGrip; // resolved by TryGrabTarget_Implementation
			Pawn->bBlockDemotion = true; // don't demote mid-grab
			if (AATR_Player* GrabbedPlayer = Cast<AATR_Player>(Target))
				GrabbedPlayer->RegisterGrab(Pawn, Pawn->CurrentGrip); // player applies drag/pull + owns the struggle
			UE_LOG(LogATR_EchoAI, Verbose, TEXT("EchoMelee[%d]: GRAB %s (dist %.0f cm)"),
				EchoId, *GetNameSafe(Target), Dist);
		}
	}

	// While grabbed — pull the target in and bite on a cadence; release if it slips away,
	// the grip was lost externally (structural damage), or the grab has held too long (anti-strand).
	if (Data.bGrabbed)
	{
		// External grip loss (capability change cleared CurrentGrip): bail out cleanly.
		if (Pawn->CurrentGrip == EATR_EchoGripType::None)
		{
			Data.Grip = EATR_EchoGripType::None;
			ReleaseGrab(TEXT("grip lost externally"));
			return EStateTreeRunStatus::Running;
		}

		// Anti-strand: cap any single grab so bBlockDemotion can't strand forever.
		if (Data.GrabStartTime > 0.f && (Now - Data.GrabStartTime) > S->MaxGrabHoldSeconds)
		{
			ReleaseGrab(TEXT("max hold exceeded"));
			return EStateTreeRunStatus::Running;
		}

		Pawn->PullTarget(Target, S->MeleePullStrength);

		if (Dist <= S->BiteRange && (Now - Data.LastBiteTime) >= S->BiteCooldownSeconds)
		{
			Data.LastBiteTime = Now;
			Pawn->TryBiteTarget(Target); // grip read from Pawn->CurrentGrip internally
		}

		if (Dist > S->GrabRange * S->GrabReleaseMultiplier)
			ReleaseGrab(TEXT("target escaped"));
	}

	return EStateTreeRunStatus::Running; // never completes - chase + attack run together
}

void FATR_EchoMeleeTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& Data = Context.GetInstanceData(*this);
	if (Data.bGrabbed)
	{
		if (AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner()))
			if (AATR_ActiveEcho* Pawn = Cast<AATR_ActiveEcho>(AIC->GetPawn()))
			{
				Pawn->bBlockDemotion = false;
				Pawn->NotifyGrabReleased();
				if (AATR_Player* GrabbedPlayer = Cast<AATR_Player>(Data.Target))
					GrabbedPlayer->UnregisterGrab(Pawn);
			}
		Data.bGrabbed = false;
		Data.Grip     = EATR_EchoGripType::None;
	}
}
