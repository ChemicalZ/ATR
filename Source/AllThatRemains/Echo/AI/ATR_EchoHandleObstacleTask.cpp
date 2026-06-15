// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoHandleObstacleTask.h"
#include "ATR_EchoAIController.h"
#include "ATR_EchoAILog.h"
#include "../ATR_EchoSubsystem.h"
#include "../ATR_EchoSettings.h"
#include "../Data/ATR_EchoObstacleBehaviorDataAsset.h"
#include "Navigation/PathFollowingComponent.h"
#include "StateTreeExecutionContext.h"

namespace
{
	// Deterministic per-Echo [0,1) hash for shuffle variation (mirrors the subsystem's).
	float TaskHash01(int32 EchoId, uint32 Salt)
	{
		uint32 H = static_cast<uint32>(EchoId) * 2654435761u + Salt * 40503u;
		H ^= H >> 13; H *= 0x85ebca6bu; H ^= H >> 16;
		return static_cast<float>(H & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
	}

	// Technical hash-salt mixers for shuffle variation — NOT behavior tuning.
	constexpr float  GShuffleAngleSaltScale = 7.f;
	constexpr uint32 GShuffleAngleSaltAdd   = 11u;
	constexpr float  GShuffleDistSaltScale  = 13.f;
	constexpr uint32 GShuffleDistSaltAdd    = 29u;
}

EStateTreeRunStatus FATR_EchoHandleObstacleTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_EngageBarrierTask_EnterState);

	FInstanceDataType& Data = Context.GetInstanceData(*this);
	AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	const UWorld* W = AIC->GetWorld();
	const UATR_EchoSubsystem* Sub = W ? W->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	const FATR_EchoRuntimeState* State = Sub ? Sub->GetEchoState(AIC->GetEchoId()) : nullptr;
	if (!State) return EStateTreeRunStatus::Failed;

	// Nothing to engage (cleared between intent selection and task entry) → let the tree
	// fall through to pursuit/search.
	if (!State->Obstacle.bHasObstacle) return EStateTreeRunStatus::Succeeded;

	Data.EnterTime     = W->GetTimeSeconds();
	Data.LastHitTime   = -1.f;
	Data.ShuffleTarget = FVector::ZeroVector;
	Data.WaitMoveSerial = 0;

	// DEBUG-ONLY legacy reroute: execute the subsystem's sidestep move and resolve by serial.
	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	if (Settings && Settings->bAllowActivePursuitTacticalReroute)
	{
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

	UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("EngageBarrier — EchoId %d type %d phase %d at %s"),
		AIC->GetEchoId(), static_cast<int32>(State->Obstacle.BarrierType),
		static_cast<int32>(State->Obstacle.Phase),
		*State->Obstacle.ObstacleLocation.ToCompactString());

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FATR_EchoHandleObstacleTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_EngageBarrierTask_Tick);

	FInstanceDataType& Data = Context.GetInstanceData(*this);
	AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	// No pawn = can't measure contact distance and a frustrated tap would fire
	// from the barrier itself (zero-distance fallback). Suspend until possessed.
	const APawn* PawnNow = AIC->GetPawn();
	if (!PawnNow) return EStateTreeRunStatus::Running;

	UWorld* W = AIC->GetWorld();
	UATR_EchoSubsystem* Sub = W ? W->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	const FATR_EchoRuntimeState* State = Sub ? Sub->GetEchoState(AIC->GetEchoId()) : nullptr;
	if (!State) return EStateTreeRunStatus::Failed;

	// DEBUG-ONLY legacy reroute resolution.
	if (Data.WaitMoveSerial != 0)
	{
		switch (AIC->GetMoveOutcomeForSerial(static_cast<uint32>(Data.WaitMoveSerial)))
		{
			case AATR_EchoAIController::EEchoMoveOutcome::Succeeded: return EStateTreeRunStatus::Succeeded;
			case AATR_EchoAIController::EEchoMoveOutcome::Failed:    return EStateTreeRunStatus::Failed;
			default:                                                return EStateTreeRunStatus::Running;
		}
	}

	// Barrier opened / broke / engagement expired — the subsystem cleared it. Succeed so the
	// tree re-evaluates: pursuit resumes if the stimulus survived, search/idle otherwise.
	const FATR_EchoObstacleIntent& O = State->Obstacle;
	if (!O.bHasObstacle) return EStateTreeRunStatus::Succeeded;

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	const float Now          = W->GetTimeSeconds();
	const float EngageDist   = S->BarrierEngageDistanceCm;
	const float AttackEvery  = S->BarrierAttackIntervalSeconds;
	const float FrustEvery   = S->FrustratedSearchHitIntervalSeconds;
	const float FrustRadius  = S->FrustratedSearchRadiusCm;

	// Always face the barrier.
	AIC->SetFocalPoint(O.ObstacleLocation, EAIFocusPriority::Gameplay);

	if (O.Phase == EATR_EchoBarrierPhase::FrustratedSearch)
	{
		// Linger: small random shuffles around the barrier, an occasional hit. No reroute —
		// the subsystem decays this into idle/wander when the frustration window ends.
		if (Data.ShuffleTarget.IsZero() ||
			AIC->DirectApproach(Data.ShuffleTarget, S->FrustratedShuffleAcceptRadiusCm))
		{
			const float MinFrac = S->FrustratedShuffleMinRadiusFraction;
			const float Ang  = TaskHash01(AIC->GetEchoId(), static_cast<uint32>(Now * GShuffleAngleSaltScale) + GShuffleAngleSaltAdd) * 2.f * PI;
			const float Dist = FrustRadius * (MinFrac + (1.f - MinFrac)
				* TaskHash01(AIC->GetEchoId(), static_cast<uint32>(Now * GShuffleDistSaltScale) + GShuffleDistSaltAdd));
			Data.ShuffleTarget = O.ObstacleLocation + FVector(FMath::Cos(Ang), FMath::Sin(Ang), 0.f) * Dist;
		}

		if (Data.LastHitTime < 0.f || (Now - Data.LastHitTime) >= FrustEvery)
		{
			// Occasional frustrated tap — only when actually at the barrier.
			if (FVector::Dist2D(PawnNow->GetActorLocation(), O.ObstacleLocation)
				<= EngageDist * S->FrustratedSearchHitRangeMultiplier)
			{
				Sub->ReportBarrierImpact(AIC->GetEchoId());
				Data.LastHitTime = Now;
			}
		}
		return EStateTreeRunStatus::Running;
	}

	// Engage / ReachThrough: close into contact, then press and attack on the interval.
	const bool bInContact = AIC->DirectApproach(O.ObstacleLocation, EngageDist);
	if (!bInContact) return EStateTreeRunStatus::Running;

	// First-attack delay (wind-up): per-Echo obstacle behavior asset, else project setting.
	const UATR_EchoObstacleBehaviorDataAsset* Behavior = Sub->ResolvedDefaultObstacleBehavior;
	const float FirstDelay = Behavior ? Behavior->AttackObstacleDelaySeconds : S->BarrierFirstAttackDelaySeconds;
	if ((Now - Data.EnterTime) < FirstDelay) return EStateTreeRunStatus::Running;

	if (Data.LastHitTime < 0.f || (Now - Data.LastHitTime) >= AttackEvery)
	{
		// Press/attack/claw/reach — damage, group pressure, pounding agitation, and the
		// impact-noise stimulus all resolve in the subsystem. ReachThrough currently attacks
		// from contact as well; target grabs through the opening layer on via the melee task
		// when the target enters reach.
		Sub->ReportBarrierImpact(AIC->GetEchoId());
		Data.LastHitTime = Now;
	}

	return EStateTreeRunStatus::Running;
}

void FATR_EchoHandleObstacleTask::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_EngageBarrierTask_ExitState);

	if (AAIController* AIC = Cast<AAIController>(Context.GetOwner()))
	{
		AIC->ClearFocus(EAIFocusPriority::Gameplay);
		AIC->StopMovement();
	}
}
