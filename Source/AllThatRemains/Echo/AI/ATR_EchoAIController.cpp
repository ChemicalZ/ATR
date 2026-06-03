// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoAIController.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"

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
	Super::OnPossess(InPawn);

	if (AIPerception)
	{
		AIPerception->SetComponentTickEnabled(true);
		AIPerception->OnPerceptionUpdated.AddDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
	}

	if (StateTreeComp) StateTreeComp->StartLogic();
}

void AATR_EchoAIController::OnUnPossess()
{
	StopMovement(); // must be before Super — Super clears the pawn reference
	CurrentTarget.Reset();

	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));

	if (AIPerception)
	{
		AIPerception->OnPerceptionUpdated.RemoveDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
		AIPerception->SetComponentTickEnabled(false);
	}

	Super::OnUnPossess();
}

void AATR_EchoAIController::EnterPool()
{
	// Safety net — normally OnUnPossess already cleaned up.
	// RemoveDynamic on an unbound delegate is a no-op.
	CurrentTarget.Reset();

	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));

	if (AIPerception)
	{
		AIPerception->OnPerceptionUpdated.RemoveDynamic(this, &AATR_EchoAIController::HandlePerceptionUpdated);
		AIPerception->SetComponentTickEnabled(false);
	}
}

// ─── Perception ───────────────────────────────────────────────────────────────

void AATR_EchoAIController::HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors)
{
	// Authoritative on server only — clients have no AI.
	if (!HasAuthority()) return;

	AActor* Best = SelectBestTarget();

	if (Best == CurrentTarget.Get()) return; // no change, avoid redundant move requests

	CurrentTarget = Best;

	if (Best)
		MoveToActor(Best, MoveAcceptanceRadius);
	else
		StopMovement();
}

AActor* AATR_EchoAIController::SelectBestTarget() const
{
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
		if (!IsValid(Actor)) continue;

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
