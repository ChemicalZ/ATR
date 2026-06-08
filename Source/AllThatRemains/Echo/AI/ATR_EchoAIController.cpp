// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoAIController.h"
#include "ATR_EchoAILog.h"
#include "../ATR_EchoSubsystem.h"
#include "../ATR_ActiveEcho.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"

DEFINE_LOG_CATEGORY(LogATR_EchoAI);

// ─── Construction ─────────────────────────────────────────────────────────────

AATR_EchoAIController::AATR_EchoAIController()
{
	AIPerception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));

	UAISenseConfig_Sight* SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius                              = 2000.f;
	SightConfig->LoseSightRadius                         = 2500.f;
	SightConfig->PeripheralVisionAngleDegrees             = 90.f;
	SightConfig->SetMaxAge(5.f);
	SightConfig->DetectionByAffiliation.bDetectEnemies    = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals   = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*SightConfig);
	AIPerception->SetDominantSense(SightConfig->GetSenseImplementation());

	UAISenseConfig_Hearing* HearingConfig = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("HearingConfig"));
	HearingConfig->HearingRange                              = 3000.f;
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

	if (StateTreeComp) StateTreeComp->StartLogic();

	UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("OnPossess — %s possessed %s (EchoId %d)"),
		*GetName(), InPawn ? *InPawn->GetName() : TEXT("null"), CachedEchoId);
}

void AATR_EchoAIController::OnUnPossess()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_OnUnPossess);
	StopMovement(); // must be before Super — Super clears the pawn reference
	CurrentTarget.Reset();

	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));

	if (AIPerception)
	{
		AIPerception->OnPerceptionUpdated.RemoveDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
		SetSensesEnabled(false); // disable senses + forget memory so no stale stimuli carry over
		AIPerception->SetComponentTickEnabled(false);
	}

	CachedEchoId    = INDEX_NONE;
	CachedSubsystem = nullptr;

	Super::OnUnPossess();
}

void AATR_EchoAIController::EnterPool()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_EnterPool);

	// Safety net — normally OnUnPossess already cleaned up.
	// RemoveDynamic on an unbound delegate is a no-op.
	CurrentTarget.Reset();

	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));

	if (AIPerception)
	{
		AIPerception->OnPerceptionUpdated.RemoveDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
		SetSensesEnabled(false); // disable senses + forget perception memory
		AIPerception->SetComponentTickEnabled(false);
	}

	CachedEchoId    = INDEX_NONE;
	CachedSubsystem = nullptr;
}

// ─── Perception ───────────────────────────────────────────────────────────────

void AATR_EchoAIController::HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_HandlePerceptionUpdated);
	if (!HasAuthority()) return;

	// Phase 2: perception now FEEDS the subsystem's canonical awareness instead of being
	// the sole owner of "what this Echo knows". The legacy CurrentTarget update below is
	// transitional — it keeps the old FATR_EchoTargetEvaluator working until Phase 3 makes
	// subsystem intent authoritative, after which both are removed.
	ReportPerceptionFacts(UpdatedActors);

	CurrentTarget = SelectBestTarget();
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
					// Currently visible — report live actor position/velocity as confirmed sight.
					CachedSubsystem->ReportEchoSawActor(CachedEchoId, Actor,
						Actor->GetActorLocation(), Actor->GetVelocity(), Now);
				}
				else
				{
					// Sight just lost — hand off to memory using the last sensed location.
					CachedSubsystem->ReportEchoLostSight(CachedEchoId, Actor,
						Stim.StimulusLocation, Actor->GetVelocity(), Now);
				}
			}
			else if (Stim.Type == HearingID && Stim.WasSuccessfullySensed())
			{
				// Location-only — never report the noise's source actor as a target.
				CachedSubsystem->ReportEchoHeardLocation(CachedEchoId, Stim.StimulusLocation, Stim.Strength, Now);
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

AActor* AATR_EchoAIController::SelectBestTarget() const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_SelectBestTarget);
	if (!AIPerception) return nullptr;

	APawn* MyPawn = GetPawn();
	
	if (!MyPawn) return nullptr;


	// Only evaluate sight — hearing is used for alerting, not targeting.
	TArray<AActor*> KnownActors;
	AIPerception->GetKnownPerceivedActors(UAISense_Sight::StaticClass(), KnownActors);
	if (KnownActors.IsEmpty()) return nullptr;

	AActor* BestActor = nullptr;
	float   BestScore = -1.f;

	for (AActor* Actor : KnownActors)
	{

		
		if (!IsValid(Actor) || Actor == MyPawn) continue;

		// Check whether we currently have line of sight or are working from memory.
		FActorPerceptionBlueprintInfo Info;
		AIPerception->GetActorsPerception(Actor, Info);

		bool bCurrentlySensed = false;
		for (const FAIStimulus& Stim : Info.LastSensedStimuli)
		{
			if (Stim.Type == UAISense::GetSenseID<UAISense_Sight>() && Stim.WasSuccessfullySensed())
			{
				bCurrentlySensed = true;
				break;
			}
		}

		// Base score: inverse squared distance (closer = higher).
		const float DistSq = MyPawn->GetSquaredDistanceTo(Actor);
		float Score = 1.f / (DistSq + 1.f);

		// Heavily favour targets we can currently see over stale memory.
		if (bCurrentlySensed)
			Score *= 2.f;

		// Loyalty bonus: current target needs to be significantly beaten before we switch.
		if (Actor == CurrentTarget.Get())
			Score *= LoyaltyBonusMultiplier;

		if (Score > BestScore)
		{
			BestScore = Score;
			BestActor = Actor;
		}
	}

	return BestActor;
}
