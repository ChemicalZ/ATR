// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoAIController.h"
#include "ATR_EchoAILog.h"
#include "../ATR_EchoSubsystem.h"
#include "../ATR_EchoSettings.h"
#include "../ATR_ActiveEcho.h"
#include "../ATR_EchoBarrierInterface.h"
#include "../ATR_EchoAcoustics.h"
#include "../Data/ATR_EchoBarrierDataAsset.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "Components/CapsuleComponent.h"

DEFINE_LOG_CATEGORY(LogATR_EchoAI);

namespace
{
	// Technical constants — NOT behavior tuning (designer knobs live in UATR_EchoSettings).
	constexpr float GFallbackAgentRadiusCm   = 34.f;  // capsule radius if a pawn somehow has no capsule
	constexpr float GStepProbeMinDistanceCm  = 50.f;  // minimum ground-projection probe distance
	constexpr float GStepProbeRadiusFactor   = 2.f;   // probe distance = capsule radius × this (min above)
	constexpr float GStuckSweepRadiusFactor  = 3.f;   // stuck-classification sweep length = capsule radius × this (min: sweep distance)
	constexpr float GMinNoiseLoudnessFactor  = 0.05f; // floor for AIPerception loudness in the dB conversion (avoids log(0))
}

// ─── Construction ─────────────────────────────────────────────────────────────

AATR_EchoAIController::AATR_EchoAIController()
{
	// Perception tuning comes from Project Settings (Echo|Sight, Echo|Hearing) — no hardcoded
	// behavior numbers. The settings CDO is created on demand and is available during CDO construction.
	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	HearingRange          = Settings->ActiveHearingRange;
	ObstacleTraceDistance = Settings->ObstacleForwardTraceLength;

	AIPerception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));

	UAISenseConfig_Sight* SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius                              = Settings->ActiveSightRadius;
	SightConfig->LoseSightRadius                          = Settings->ActiveLoseSightRadius;
	SightConfig->PeripheralVisionAngleDegrees             = Settings->ActivePeripheralVisionAngleDegrees;
	SightConfig->SetMaxAge(Settings->ActiveSightMaxAgeSeconds);
	SightConfig->DetectionByAffiliation.bDetectEnemies    = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals   = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*SightConfig);
	AIPerception->SetDominantSense(SightConfig->GetSenseImplementation());

	UAISenseConfig_Hearing* HearingConfig = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("HearingConfig"));
	HearingConfig->HearingRange                             = HearingRange;
	HearingConfig->SetMaxAge(Settings->ActiveHearingMaxAgeSeconds);
	HearingConfig->DetectionByAffiliation.bDetectEnemies    = true;
	HearingConfig->DetectionByAffiliation.bDetectNeutrals   = true;
	HearingConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*HearingConfig);

	StateTreeComp = CreateDefaultSubobject<UStateTreeComponent>(TEXT("StateTree"));
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void AATR_EchoAIController::OnPossess(APawn* InPawn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_OnPossess);
	Super::OnPossess(InPawn);

	// Resolve the canonical-state bridge once. EchoId is stable for the Echo's lifetime,
	// so caching it here is safe even though the underlying SoA index can move.
	CachedSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	CachedEchoId    = INDEX_NONE;
	if (CachedSubsystem)
	{
		if (const AATR_ActiveEcho* Echo = Cast<AATR_ActiveEcho>(InPawn))
			CachedEchoId = CachedSubsystem->GetEchoIdForIndex(Echo->SourceIndex);
	}

	if (AIPerception)
	{
		SetSensesEnabled(true); // explicit re-enable on possess (paired with pool disable)
		AIPerception->SetComponentTickEnabled(true);
		// Guard against double-binding if a lifecycle bug ever possesses without an
		// intervening unpossess. AddUnique is a no-op when already bound.
		AIPerception->OnPerceptionUpdated.AddUniqueDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
	}

	// Classified move results flow back to the subsystem through this callback.
	ReceiveMoveCompleted.AddUniqueDynamic(this, &AATR_EchoAIController::HandleMoveCompleted);

	if (StateTreeComp) StateTreeComp->StartLogic();

	UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("OnPossess — %s possessed %s (EchoId %d)"),
		*GetName(), InPawn ? *InPawn->GetName() : TEXT("null"), CachedEchoId);
}

void AATR_EchoAIController::OnUnPossess()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_OnUnPossess);
	StopMovement(); // must be before Super — Super clears the pawn reference (also clears direct pursuit)
	PrevVisibleActor.Reset();
	PrevVisibleTime = -1.f;

	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));

	if (AIPerception)
	{
		AIPerception->OnPerceptionUpdated.RemoveDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
		SetSensesEnabled(false); // disable senses + forget memory so no stale stimuli carry over
		AIPerception->SetComponentTickEnabled(false);
	}

	ReceiveMoveCompleted.RemoveDynamic(this, &AATR_EchoAIController::HandleMoveCompleted);
	ActiveMoveRequestId = FAIRequestID::InvalidRequest;
	CachedEchoId    = INDEX_NONE;
	CachedSubsystem = nullptr;

	Super::OnUnPossess();
}

void AATR_EchoAIController::EnterPool()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_EnterPool);

	// Safety net — normally OnUnPossess already cleaned up.
	// RemoveDynamic on an unbound delegate is a no-op.
	bDirectPursuitActive = false;
	DirectGoalActor.Reset();
	PrevVisibleActor.Reset();
	PrevVisibleTime = -1.f;

	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));

	if (AIPerception)
	{
		AIPerception->OnPerceptionUpdated.RemoveDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
		SetSensesEnabled(false); // disable senses + forget perception memory
		AIPerception->SetComponentTickEnabled(false);
	}

	ReceiveMoveCompleted.RemoveDynamic(this, &AATR_EchoAIController::HandleMoveCompleted);
	ActiveMoveRequestId = FAIRequestID::InvalidRequest;
	CachedEchoId    = INDEX_NONE;
	CachedSubsystem = nullptr;
}

// ─── Perception ───────────────────────────────────────────────────────────────

void AATR_EchoAIController::HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandlePerceptionUpdated);
	if (!HasAuthority()) return;

	// Perception only FEEDS the subsystem's canonical awareness; the subsystem owns "what this
	// Echo knows" and selects intent. The controller no longer holds its own target.
	ReportPerceptionFacts(UpdatedActors);
}

void AATR_EchoAIController::ReportPerceptionFacts(const TArray<AActor*>& UpdatedActors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_ReportPerceptionFacts);

	if (!AIPerception || !CachedSubsystem || CachedEchoId == INDEX_NONE) return;

	const float       Now       = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const FAISenseID  SightID   = UAISense::GetSenseID<UAISense_Sight>();
	const FAISenseID  HearingID = UAISense::GetSenseID<UAISense_Hearing>();
	const APawn*      MyPawn    = GetPawn();

	for (AActor* Actor : UpdatedActors)
	{
		if (!IsValid(Actor) || Actor == MyPawn) continue;

		FActorPerceptionBlueprintInfo Info;
		AIPerception->GetActorsPerception(Actor, Info);

		for (const FAIStimulus& Stim : Info.LastSensedStimuli)
		{
			if (Stim.Type == SightID)
			{
				if (Stim.WasSuccessfullySensed())
				{
					// Currently visible. Derive observed velocity from the delta of successive
					// VISIBLE positions rather than Actor->GetVelocity(), so the Echo never reads
					// motion it could not have seen. First sighting (or a different actor) yields
					// zero velocity until a second visible sample arrives.
					const FVector CurLoc = Actor->GetActorLocation();
					FVector ObservedVelocity = FVector::ZeroVector;
					if (PrevVisibleActor.Get() == Actor && PrevVisibleTime >= 0.f)
					{
						const float Dt = Now - PrevVisibleTime;
						if (Dt > KINDA_SMALL_NUMBER)
							ObservedVelocity = (CurLoc - PrevVisibleLocation) / Dt;
					}
					PrevVisibleActor    = Actor;
					PrevVisibleLocation = CurLoc;
					PrevVisibleTime     = Now;

					CachedSubsystem->ReportEchoSawActor(CachedEchoId, Actor, CurLoc, ObservedVelocity, Now);
				}
				else
				{
					// Sight just lost — hand off to memory. No location/velocity is sampled here:
					// the subsystem keeps the values it observed while the target was visible.
					CachedSubsystem->ReportEchoLostSight(CachedEchoId, Actor, Now);

					if (PrevVisibleActor.Get() == Actor)
					{
						PrevVisibleActor.Reset();
						PrevVisibleTime = -1.f;
					}
				}
			}
			else if (Stim.Type == HearingID && Stim.WasSuccessfullySensed())
			{
				// Location-only. Model the noise as a Noise stimulus with REAL acoustic
				// attenuation: the AIPerception loudness multiplier maps onto a dB SPL source
				// level (1.0 → DefaultPerceivedNoiseLoudnessDb; each ×2 = +6 dB), and the
				// received level at this Echo falls off with inverse-square spreading + air
				// absorption (ATR_EchoAcoustics — the future ray-tracing seam). The source
				// actor is carried ONLY as debug metadata, never as target knowledge.
				const UATR_EchoSettings* AS = GetDefault<UATR_EchoSettings>();
				const float RefDistCm    = AS->AcousticReferenceDistanceCm;
				const float ThresholdDb  = AS->EchoHearingThresholdDb;
				const float SaturationDb = AS->EchoHearingSaturationDb;
				const float AirAbsorb    = AS->AirAbsorptionDbPer100m;
				const float DefaultDb    = AS->DefaultPerceivedNoiseLoudnessDb;

				const FVector PawnLoc  = MyPawn ? MyPawn->GetActorLocation() : Stim.StimulusLocation;
				const float   Dist     = FVector::Dist(PawnLoc, Stim.StimulusLocation);
				const float   SourceDb = DefaultDb + 20.f * FMath::LogX(10.f, FMath::Max(Stim.Strength, GMinNoiseLoudnessFactor));

				const float ReceivedDb = ATR_EchoAcoustics::ComputeReceivedDb(SourceDb, Dist, RefDistCm, AirAbsorb);
				const float Perceived  = ATR_EchoAcoustics::DbToNormalizedStrength(ReceivedDb, ThresholdDb, SaturationDb);
				if (Perceived <= 0.f) continue; // arrived below the hearing threshold — masked

				// Imperfect hearing: localize with an error that grows with distance and shrinks
				// with received level — quiet, distant noises produce rough estimates.
				FVector PerceivedLoc = Stim.StimulusLocation;
				const float ErrFraction = AS->HearingMaxLocationErrorFraction * (1.f - Perceived);
				if (ErrFraction > 0.f && Dist > 1.f)
				{
					const float Ang = FMath::FRandRange(0.f, 2.f * PI);
					const float Mag = Dist * ErrFraction * FMath::FRand();
					PerceivedLoc += FVector(FMath::Cos(Ang), FMath::Sin(Ang), 0.f) * Mag;
				}

				FATR_StimulusEvent Noise;
				Noise.Type                 = EATR_StimulusType::Noise;
				Noise.Location             = PerceivedLoc;
				Noise.Direction            = (Stim.StimulusLocation - PawnLoc).GetSafeNormal();
				Noise.LoudnessDb           = SourceDb;
				Noise.Strength             = Perceived; // received intensity at THIS Echo's ear
				Noise.Radius               = 0.f;
				Noise.TimeSeconds          = Now;
				Noise.SourceActor_DebugOnly = Actor; // metadata only — not a target

				CachedSubsystem->ReportEchoStimulus(CachedEchoId, Noise);
			}
		}
	}
}

void AATR_EchoAIController::SetSensesEnabled(bool bEnabled)
{
	if (!AIPerception) return;

	AIPerception->SetSenseEnabled(UAISense_Sight::StaticClass(),   bEnabled);
	AIPerception->SetSenseEnabled(UAISense_Hearing::StaticClass(), bEnabled);

	if (!bEnabled)
		AIPerception->ForgetAll(); // drop stale stimuli so a recycled controller starts clean
}

// ─── Movement Execution ───────────────────────────────────────────────

EPathFollowingRequestResult::Type AATR_EchoAIController::IssueMoveRequest(const FATR_EchoMoveRequest& Request)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_IssueMoveRequest);

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// Any new request supersedes a running direct-pursuit move.
	bDirectPursuitActive = false;
	DirectGoalActor.Reset();

	// Validate up front so movement can never silently fall back to FVector::ZeroVector.
	if (Request.Type == EATR_EchoMoveTargetType::None ||
		(Request.Type == EATR_EchoMoveTargetType::Actor && !Request.Actor.IsValid()))
	{
		ReportMoveResultToSubsystem(false, EATR_MoveFailureReason::InvalidTarget, Now, nullptr);
		return EPathFollowingRequestResult::Failed;
	}

	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	const bool bDirect = Request.bDirectPursuit && (!Settings || Settings->bUseLineOfDesirePursuit);

	// New monotonic serial for this issued request (shared by both movement models). The
	// move/obstacle task records the serial and resolves only when the matching completion
	// is reported — so a path that merely goes Idle is not treated as success.
	if (CachedSubsystem && CachedEchoId != INDEX_NONE)
	{
		if (FATR_EchoRuntimeState* State = CachedSubsystem->GetMutableEchoState(CachedEchoId))
		{
			State->Movement.MoveRequestSerial += 1;
			State->Movement.bMoveInProgress     = true;
			State->Movement.bLastMoveSucceeded  = false;
			LastIssuedMoveSerial = State->Movement.MoveRequestSerial;
		}
	}

	if (bDirect)
	{
		// LINE-OF-DESIRE pursuit (design doc: Active Prey-Driven Pursuit). No MoveTo, no
		// route: TickDirectPursuit moves along the stimulus vector each frame, validates
		// with ground projection, and classifies any meaningful blocker it meets.
		Super::StopMovement(); // kill any path-following move without clearing direct state
		ActiveMoveRequestId = FAIRequestID::InvalidRequest;

		bDirectGoalIsActor     = (Request.Type == EATR_EchoMoveTargetType::Actor);
		DirectGoalActor        = Request.Actor;
		DirectGoalLocation     = Request.Location;
		DirectAcceptanceRadius = Request.AcceptanceRadius;
		DirectStuckTime        = 0.f;
		DirectStuckAnchor      = GetPawn() ? GetPawn()->GetActorLocation() : FVector::ZeroVector;
		bDirectPursuitActive   = true;

		// Already-at-goal short-circuit mirrors path-following behavior.
		if (const APawn* P = GetPawn())
		{
			const FVector Goal = bDirectGoalIsActor && DirectGoalActor.IsValid()
				? DirectGoalActor->GetActorLocation() : DirectGoalLocation;
			float CapsuleR = 0.f;
			if (const ACharacter* C = Cast<ACharacter>(P))
				if (const UCapsuleComponent* Cap = C->GetCapsuleComponent())
					CapsuleR = Cap->GetScaledCapsuleRadius();
			if (FVector::Dist2D(P->GetActorLocation(), Goal) <= DirectAcceptanceRadius + CapsuleR)
			{
				CompleteDirectPursuit(true, EATR_MoveFailureReason::None, nullptr);
				return EPathFollowingRequestResult::AlreadyAtGoal;
			}
		}
		return EPathFollowingRequestResult::RequestSuccessful;
	}

	// PATHFINDING move — ambient/abstract movement only (wander, horde migration, distant
	// investigation). Active pursuit never reaches this branch unless the
	// bUseLineOfDesirePursuit master switch is off (debug).
	FAIMoveRequest MoveReq;
	MoveReq.SetAcceptanceRadius(Request.AcceptanceRadius);
	MoveReq.SetUsePathfinding(true);
	MoveReq.SetReachTestIncludesAgentRadius(true);

	if (Request.Type == EATR_EchoMoveTargetType::Actor)
		MoveReq.SetGoalActor(Request.Actor.Get());
	else
		MoveReq.SetGoalLocation(Request.Location);

	const FPathFollowingRequestResult Result = MoveTo(MoveReq);
	ActiveMoveRequestId = Result.MoveId;

	// Immediate terminal codes are reported now; Running results are reported later via
	// HandleMoveCompleted when path following finishes.
	if (Result.Code == EPathFollowingRequestResult::AlreadyAtGoal)
		ReportMoveResultToSubsystem(true, EATR_MoveFailureReason::None, Now, nullptr);
	else if (Result.Code == EPathFollowingRequestResult::Failed)
		ReportMoveResultToSubsystem(false, EATR_MoveFailureReason::NoPath, Now, nullptr);

	return Result.Code;
}

void AATR_EchoAIController::StopMovement()
{
	// Tasks call this from ExitState — must clear BOTH movement models. No completion is
	// reported here: an aborted move is superseded by whatever the new state issues.
	bDirectPursuitActive = false;
	DirectGoalActor.Reset();
	Super::StopMovement();
}

void AATR_EchoAIController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bDirectPursuitActive && HasAuthority())
		TickDirectPursuit(DeltaTime);
}

void AATR_EchoAIController::TickDirectPursuit(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_TickDirectPursuit);

	APawn* P = GetPawn();
	UWorld* W = GetWorld();
	if (!P || !W)
	{
		CompleteDirectPursuit(false, EATR_MoveFailureReason::InvalidTarget, nullptr);
		return;
	}

	// Resolve the goal. Actor goals are only ever issued for CURRENTLY VISIBLE targets (the
	// intent evaluator gates on line of sight); when sight is lost the subsystem swaps in a
	// location request, which supersedes this move. No hidden-actor tracking happens here.
	AActor* GoalActor = bDirectGoalIsActor ? DirectGoalActor.Get() : nullptr;
	if (bDirectGoalIsActor && !GoalActor)
	{
		CompleteDirectPursuit(false, EATR_MoveFailureReason::InvalidTarget, nullptr);
		return;
	}
	const FVector Goal = GoalActor ? GoalActor->GetActorLocation() : DirectGoalLocation;

	const FVector PawnLoc = P->GetActorLocation();
	float CapsuleR = GFallbackAgentRadiusCm;
	if (const ACharacter* C = Cast<ACharacter>(P))
		if (const UCapsuleComponent* Cap = C->GetCapsuleComponent())
			CapsuleR = Cap->GetScaledCapsuleRadius();

	// Arrival?
	const float DistToGoal2D = FVector::Dist2D(PawnLoc, Goal);
	if (DistToGoal2D <= DirectAcceptanceRadius + CapsuleR)
	{
		CompleteDirectPursuit(true, EATR_MoveFailureReason::None, nullptr);
		return;
	}

	FVector DesiredDir = (Goal - PawnLoc);
	DesiredDir.Z = 0.f;
	if (!DesiredDir.Normalize())
	{
		CompleteDirectPursuit(true, EATR_MoveFailureReason::None, nullptr);
		return;
	}

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	const float SweepDist   = S->ActivePursuitForwardSweepDistanceCm;
	const float SweepRadius = S->ActivePursuitSweepRadiusCm;
	const float HeadOnDot   = S->ActivePursuitBlockerHeadOnDot;
	const float StuckTime   = S->ActivePursuitStuckTimeSeconds;
	const float StuckDist   = S->ActivePursuitStuckProgressCm;
	const float ProjRadius  = S->ActivePursuitGroundProjectionRadiusCm;

	// Forward sweep along the line of desire — blocker DETECTION, not path planning.
	FVector MoveDir = DesiredDir;
	FHitResult Hit;
	FCollisionQueryParams Params(TEXT("EchoDesireSweep"), /*bTraceComplex=*/false, P);
	if (GoalActor) Params.AddIgnoredActor(GoalActor);

	const FVector Start = PawnLoc;
	const FVector End   = Start + DesiredDir * FMath::Min(SweepDist, DistToGoal2D);
	const bool bHit = (SweepRadius > 0.f)
		? W->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(SweepRadius), Params)
		: W->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

	if (bHit && Hit.GetActor())
	{
		bool bMeaningful = false;
		const EATR_EchoBarrierType Type = ClassifyBarrierActor(Hit.GetActor(), bMeaningful);
		const float FacingDot = FVector::DotProduct(DesiredDir, -Hit.ImpactNormal.GetSafeNormal2D());

		// Tagged/interface barriers engage when met head-on. Untagged statics and crowd/props
		// wall-slide first — the stuck timer below still converts persistent blockage into
		// engagement, which is what produces "press briefly → frustrate" on plain walls.
		const bool bExplicitBarrier =
			Type != EATR_EchoBarrierType::NonInteractableWall &&
			Type != EATR_EchoBarrierType::Crowd &&
			Type != EATR_EchoBarrierType::SmallProp &&
			Type != EATR_EchoBarrierType::Unknown;

		if (bMeaningful && bExplicitBarrier && FacingDot >= HeadOnDot)
		{
			ReportBlockedByBarrier(Hit.GetActor(), Type, Hit.ImpactPoint, Hit.ImpactNormal);
			return;
		}

		// Glancing/soft contact → small wall slide along the surface (LOCAL avoidance — looks
		// like dumb physical pushing, never like problem solving).
		const FVector N2D = Hit.ImpactNormal.GetSafeNormal2D();
		FVector Slide = DesiredDir - N2D * FVector::DotProduct(DesiredDir, N2D);
		Slide.Z = 0.f;
		if (Slide.Normalize())
			MoveDir = Slide;
	}

	// Ground projection — navmesh as movement VALIDATOR only. If the immediate step ahead
	// has no walkable ground (cliff/void), don't take it; the stuck timer will classify and
	// frustrate. This is the only navmesh query active pursuit is allowed.
	if (const UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W))
	{
		const FVector StepProbe = PawnLoc + MoveDir * FMath::Max(CapsuleR * GStepProbeRadiusFactor, GStepProbeMinDistanceCm);
		FNavLocation Projected;
		if (!Nav->ProjectPointToNavigation(StepProbe, Projected, FVector(ProjRadius, ProjRadius, ProjRadius)))
			MoveDir = FVector::ZeroVector;
	}

	if (!MoveDir.IsNearlyZero())
		P->AddMovementInput(MoveDir, 1.f);

	SetFocalPoint(Goal);

	// Stuck detection — pushing without progress means SOMETHING physically blocks the line
	// of desire (often untagged static geometry). Classify whatever is ahead and engage it
	// rather than ever asking for an alternate route.
	if (FVector::Dist2D(PawnLoc, DirectStuckAnchor) >= StuckDist)
	{
		DirectStuckAnchor = PawnLoc;
		DirectStuckTime   = 0.f;
	}
	else
	{
		DirectStuckTime += DeltaTime;
		if (DirectStuckTime >= StuckTime)
		{
			FHitResult AheadHit;
			const FVector AheadEnd = PawnLoc + DesiredDir * FMath::Max(SweepDist, CapsuleR * GStuckSweepRadiusFactor);
			const bool bAhead = W->SweepSingleByChannel(AheadHit, PawnLoc, AheadEnd, FQuat::Identity,
				ECC_Visibility, FCollisionShape::MakeSphere(FMath::Max(SweepRadius, 1.f)), Params);

			if (bAhead && AheadHit.GetActor())
			{
				bool bMeaningful = false;
				const EATR_EchoBarrierType Type = ClassifyBarrierActor(AheadHit.GetActor(), bMeaningful);
				if (bMeaningful)
				{
					ReportBlockedByBarrier(AheadHit.GetActor(), Type, AheadHit.ImpactPoint, AheadHit.ImpactNormal);
					return;
				}
				if (Type == EATR_EchoBarrierType::Crowd)
				{
					// Crowd compression — keep pushing into the mass (shoving/piling is correct
					// horde behavior, not a failure). Reset and let separation sort it out.
					DirectStuckTime   = 0.f;
					DirectStuckAnchor = PawnLoc;
					return;
				}
			}
			// Nothing classifiable ahead (void/no walkable step) — fail as unreachable so the
			// subsystem decays into search rather than spinning here forever.
			CompleteDirectPursuit(false, EATR_MoveFailureReason::TargetUnreachable, nullptr);
		}
	}
}

bool AATR_EchoAIController::DirectApproach(const FVector& TargetLocation, float AcceptRadius)
{
	APawn* P = GetPawn();
	if (!P) return false;

	float CapsuleR = GFallbackAgentRadiusCm;
	if (const ACharacter* C = Cast<ACharacter>(P))
		if (const UCapsuleComponent* Cap = C->GetCapsuleComponent())
			CapsuleR = Cap->GetScaledCapsuleRadius();

	if (FVector::Dist2D(P->GetActorLocation(), TargetLocation) <= AcceptRadius + CapsuleR)
		return true;

	FVector Dir = TargetLocation - P->GetActorLocation();
	Dir.Z = 0.f;
	if (Dir.Normalize())
		P->AddMovementInput(Dir, 1.f);
	return false;
}

EATR_EchoBarrierType AATR_EchoAIController::ClassifyBarrierActor(const AActor* Actor, bool& bOutMeaningful) const
{
	bOutMeaningful = false;
	if (!Actor) return EATR_EchoBarrierType::None;

	// Other Echoes are crowd, not barriers — separation handles them locally.
	if (Actor->IsA<AATR_ActiveEcho>())
	{
		return EATR_EchoBarrierType::Crowd;
	}

	// 1) Interface — the actor states its own barrier rules.
	if (Actor->Implements<UATR_EchoBarrier>())
	{
		bOutMeaningful = true;
		if (const UATR_EchoBarrierDataAsset* Data = IATR_EchoBarrier::Execute_GetEchoBarrierData(const_cast<AActor*>(Actor)))
			return Data->BarrierType;
		return EATR_EchoBarrierType::Unknown;
	}

	// 2) Tag contract — designers tag barrier actors so the AI classifies them without
	//    hard class dependencies.
	static const FName TagDoor       (TEXT("Echo.Obstacle.Door"));
	static const FName TagWindow     (TEXT("Echo.Obstacle.Window"));
	static const FName TagFence      (TEXT("Echo.Obstacle.Fence"));
	static const FName TagGate       (TEXT("Echo.Obstacle.Gate"));
	static const FName TagBarricade  (TEXT("Echo.Obstacle.Barricade"));
	static const FName TagVehicle    (TEXT("Echo.Obstacle.Vehicle"));
	static const FName TagDestructibleWall(TEXT("Echo.Obstacle.DestructibleWall"));

	bOutMeaningful = true;
	if (Actor->ActorHasTag(TagDoor))             return EATR_EchoBarrierType::Door;
	if (Actor->ActorHasTag(TagWindow))           return EATR_EchoBarrierType::Window;
	if (Actor->ActorHasTag(TagFence))            return EATR_EchoBarrierType::Fence;
	if (Actor->ActorHasTag(TagGate))             return EATR_EchoBarrierType::Gate;
	if (Actor->ActorHasTag(TagBarricade))        return EATR_EchoBarrierType::Barricade;
	if (Actor->ActorHasTag(TagVehicle))          return EATR_EchoBarrierType::Vehicle;
	if (Actor->ActorHasTag(TagDestructibleWall)) return EATR_EchoBarrierType::DestructibleWall;

	// 3) Mobility fallback. Static untagged geometry = non-interactable wall: meaningful
	//    (press briefly → frustrated search → decay — never reroute). Untagged dynamic
	//    actors are props/debris: not meaningful, slide/push past them.
	if (Actor->GetRootComponent() && Actor->GetRootComponent()->Mobility == EComponentMobility::Static)
		return EATR_EchoBarrierType::NonInteractableWall;

	bOutMeaningful = false;
	return EATR_EchoBarrierType::SmallProp;
}

EATR_MoveFailureReason AATR_EchoAIController::BarrierTypeToFailureReason(EATR_EchoBarrierType Type)
{
	switch (Type)
	{
		case EATR_EchoBarrierType::Door:   return EATR_MoveFailureReason::BlockedByDoor;
		case EATR_EchoBarrierType::Window: return EATR_MoveFailureReason::BlockedByWindow;
		case EATR_EchoBarrierType::Fence:
		case EATR_EchoBarrierType::Gate:   return EATR_MoveFailureReason::BlockedByFence;
		default:                           return EATR_MoveFailureReason::BlockedByDynamicActor;
	}
}

void AATR_EchoAIController::CompleteDirectPursuit(bool bSuccess, EATR_MoveFailureReason Reason, AActor* Blocker)
{
	bDirectPursuitActive = false;
	DirectGoalActor.Reset();
	ClearFocus(EAIFocusPriority::Gameplay);

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	ReportMoveResultToSubsystem(bSuccess, Reason, Now, Blocker);
}

void AATR_EchoAIController::ReportBlockedByBarrier(AActor* Blocker, EATR_EchoBarrierType Type, const FVector& HitLocation, const FVector& HitNormal)
{
	bDirectPursuitActive = false;
	DirectGoalActor.Reset();
	ClearFocus(EAIFocusPriority::Gameplay);

	if (!CachedSubsystem || CachedEchoId == INDEX_NONE) return;

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("Line-of-desire blocked — EchoId %d barrier %s type %d"),
		CachedEchoId, Blocker ? *Blocker->GetName() : TEXT("none"), static_cast<int32>(Type));

	// Single subsystem entry point: stamps the failed move completion (so the move task
	// resolves) AND seeds barrier memory (so intent flips to EngageBarrier).
	CachedSubsystem->ReportEchoBlockedByBarrier(CachedEchoId, Blocker, Type, HitLocation, HitNormal, Now);
}

void AATR_EchoAIController::HandleMoveCompleted(FAIRequestID RequestID, EPathFollowingResult::Type Result)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandleMoveCompleted);
	if (!HasAuthority()) return;

	// Direct pursuit never uses path following — a stray path completion (e.g. the abort of
	// a superseded path move) must not resolve a line-of-desire request.
	if (bDirectPursuitActive) return;

	// Ignore completions for superseded requests.
	if (ActiveMoveRequestId.IsValid() && !RequestID.IsEquivalent(ActiveMoveRequestId)) return;

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	bool                   bSuccess = false;
	EATR_MoveFailureReason Reason   = EATR_MoveFailureReason::None;

	AActor* Blocker = nullptr;

	switch (Result)
	{
		case EPathFollowingResult::Success: bSuccess = true; break;
		// Blocked → trace forward to identify and classify the obstacle (door/window/fence).
		case EPathFollowingResult::Blocked: Reason = ClassifyBlockingObstacle(Blocker);         break;
		case EPathFollowingResult::OffPath: Reason = EATR_MoveFailureReason::TargetUnreachable;  break;
		case EPathFollowingResult::Aborted: Reason = EATR_MoveFailureReason::AbortedByNewIntent; break;
		case EPathFollowingResult::Invalid: Reason = EATR_MoveFailureReason::InvalidTarget;      break;
		default:                            Reason = EATR_MoveFailureReason::NoPath;             break;
	}

	if (!bSuccess)
	{
		UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("Move blocked — EchoId %d reason %d blocker %s"),
			CachedEchoId, static_cast<int32>(Reason), Blocker ? *Blocker->GetName() : TEXT("none"));
	}

	ReportMoveResultToSubsystem(bSuccess, Reason, Now, Blocker);
}

EATR_MoveFailureReason AATR_EchoAIController::ClassifyBlockingObstacle(AActor*& OutBlocker) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_ClassifyBlockingObstacle);

	// Tag contract — designers tag breakable obstacle actors so the AI can classify them
	// without hard class dependencies. Unknown blockers fall back to a generic dynamic block.
	static const FName TagDoor  (TEXT("Echo.Obstacle.Door"));
	static const FName TagWindow(TEXT("Echo.Obstacle.Window"));
	static const FName TagFence (TEXT("Echo.Obstacle.Fence"));

	OutBlocker = nullptr;

	const APawn* P = GetPawn();
	const UWorld* W = GetWorld();
	if (!P || !W) return EATR_MoveFailureReason::BlockedByDynamicActor;

	const FVector Start = P->GetActorLocation();
	const FVector End   = Start + P->GetActorForwardVector() * ObstacleTraceDistance;

	FHitResult Hit;
	FCollisionQueryParams Params(TEXT("EchoObstacleTrace"), /*bTraceComplex=*/false, P);
	if (!W->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) || !Hit.GetActor())
		return EATR_MoveFailureReason::BlockedByDynamicActor;

	OutBlocker = Hit.GetActor();
	if (OutBlocker->ActorHasTag(TagDoor))   return EATR_MoveFailureReason::BlockedByDoor;
	if (OutBlocker->ActorHasTag(TagWindow)) return EATR_MoveFailureReason::BlockedByWindow;
	if (OutBlocker->ActorHasTag(TagFence))  return EATR_MoveFailureReason::BlockedByFence;
	return EATR_MoveFailureReason::BlockedByDynamicActor;
}

void AATR_EchoAIController::ReportMoveResultToSubsystem(bool bSuccess, EATR_MoveFailureReason Reason, float TimeSeconds, AActor* BlockingActor)
{
	if (!CachedSubsystem || CachedEchoId == INDEX_NONE) return;

	const FVector Loc = GetPawn() ? GetPawn()->GetActorLocation() : FVector::ZeroVector;
	CachedSubsystem->ReportEchoMoveResult(CachedEchoId, bSuccess, Reason, Loc, BlockingActor, TimeSeconds);
}

AATR_EchoAIController::EEchoMoveOutcome AATR_EchoAIController::GetMoveOutcomeForSerial(uint32 Serial) const
{
	if (!CachedSubsystem || CachedEchoId == INDEX_NONE) return EEchoMoveOutcome::Failed;

	const FATR_EchoRuntimeState* State = CachedSubsystem->GetEchoState(CachedEchoId);
	if (!State) return EEchoMoveOutcome::Failed;

	const FATR_EchoMovementIntent& M = State->Movement;

	// A newer request has superseded the one this task issued — abandon this state.
	if (M.MoveRequestSerial != Serial) return EEchoMoveOutcome::Failed;

	// The matching completion has been recorded → resolve by its classified result.
	if (M.LastCompletedMoveRequestSerial == Serial)
		return M.bLastMoveSucceeded ? EEchoMoveOutcome::Succeeded : EEchoMoveOutcome::Failed;

	// Still the outstanding request, not yet completed.
	return EEchoMoveOutcome::Pending;
}