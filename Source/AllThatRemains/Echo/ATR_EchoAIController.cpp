// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoAIController.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"

AATR_EchoAIController::AATR_EchoAIController()
{
	AIPerception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));

	UAISenseConfig_Sight* SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius                              = 2000.f;
	SightConfig->LoseSightRadius                          = 2500.f;
	SightConfig->PeripheralVisionAngleDegrees             = 90.f;
	SightConfig->SetMaxAge(5.f);
	SightConfig->DetectionByAffiliation.bDetectEnemies    = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals   = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*SightConfig);
	AIPerception->SetDominantSense(SightConfig->GetSenseImplementation());

	UAISenseConfig_Hearing* HearingConfig = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("HearingConfig"));
	HearingConfig->HearingRange                               = 3000.f;
	HearingConfig->DetectionByAffiliation.bDetectEnemies     = true;
	HearingConfig->DetectionByAffiliation.bDetectNeutrals    = true;
	HearingConfig->DetectionByAffiliation.bDetectFriendlies  = false;
	AIPerception->ConfigureSense(*HearingConfig);

	StateTreeComp = CreateDefaultSubobject<UStateTreeComponent>(TEXT("StateTree"));
}

void AATR_EchoAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	if (AIPerception)  AIPerception->SetComponentTickEnabled(true);
	if (StateTreeComp) StateTreeComp->StartLogic();
}

void AATR_EchoAIController::OnUnPossess()
{
	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));
	if (AIPerception)  AIPerception->SetComponentTickEnabled(false);

	Super::OnUnPossess();
}

void AATR_EchoAIController::EnterPool()
{
	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));
	if (AIPerception)  AIPerception->SetComponentTickEnabled(false);
}
