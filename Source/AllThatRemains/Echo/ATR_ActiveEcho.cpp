// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_ActiveEcho.h"
#include "ATR_EchoSubsystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Net/UnrealNetwork.h"
#include "Components/SkeletalMeshComponent.h"

// ─── Construction ─────────────────────────────────────────────────────────────

AATR_ActiveEcho::AATR_ActiveEcho()
{
	// Start dormant — EnterPool/InitFromSoA control actual tick state.
	PrimaryActorTick.bCanEverTick      = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates              = true;
	bUseControllerRotationYaw = false;

	// CMC — zombie defaults, tunable in Blueprint subclass
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxWalkSpeed               = 150.f;
		CMC->bOrientRotationToMovement  = true;
		CMC->RotationRate               = FRotator(0.f, 360.f, 0.f);
		CMC->MaxAcceleration            = 512.f;
		CMC->BrakingDecelerationWalking = 512.f;
		CMC->bCanWalkOffLedges          = true;
		CMC->SetIsReplicated(true);
	}

	// AI Perception component
	AIPerception = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));

	// Sight — parameters tunable on the config in Blueprint Details panel
	UAISenseConfig_Sight* SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius                           = 2000.f;
	SightConfig->LoseSightRadius                       = 2500.f;
	SightConfig->PeripheralVisionAngleDegrees          = 90.f;
	SightConfig->SetMaxAge(5.f);
	SightConfig->DetectionByAffiliation.bDetectEnemies    = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals   = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*SightConfig);
	AIPerception->SetDominantSense(SightConfig->GetSenseImplementation());

	// Hearing
	UAISenseConfig_Hearing* HearingConfig = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("HearingConfig"));
	HearingConfig->HearingRange                            = 3000.f;
	HearingConfig->DetectionByAffiliation.bDetectEnemies  = true;
	HearingConfig->DetectionByAffiliation.bDetectNeutrals = true;
	HearingConfig->DetectionByAffiliation.bDetectFriendlies = false;
	AIPerception->ConfigureSense(*HearingConfig);

	// StateTree — assign asset in Blueprint subclass; component is the runner.
	StateTreeComp = CreateDefaultSubobject<UStateTreeComponent>(TEXT("StateTree"));
}

// ─── BeginPlay ────────────────────────────────────────────────────────────────

void AATR_ActiveEcho::BeginPlay()
{
	Super::BeginPlay();

	// Apply mesh offsets from Blueprint class defaults — corrects pivot/facing vs ISM.
	if (USkeletalMeshComponent* mesh = GetMesh())
		mesh->SetRelativeLocationAndRotation(MeshLocationOffset, MeshRotationOffset);

	// AI and behavior are authoritative on server only.
	// Clients drive visuals entirely via CMC replication — no local AI needed.
	if (!HasAuthority())
	{
		if (AIPerception) AIPerception->SetComponentTickEnabled(false);
		if (StateTreeComp) StateTreeComp->SetComponentTickEnabled(false);
	}

	// Do NOT start StateTree here — InitFromSoA starts it on promotion so it
	// never runs while the actor is sitting in the pool.
}

void AATR_ActiveEcho::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void AATR_ActiveEcho::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AATR_ActiveEcho, SourceIndex);
	DOREPLIFETIME(AATR_ActiveEcho, AnimStateCache);
}

void AATR_ActiveEcho::OnRep_SourceIndex()
{
	auto* Sub = GetWorld() ? GetWorld()->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	if (!Sub) return;

	// Unregister old slot (covers demotion: SourceIndex flips to INDEX_NONE)
	if (ClientPrevSourceIndex != INDEX_NONE
		&& ClientPrevSourceIndex < Sub->ActiveEntities
		&& Sub->IndexToActor[ClientPrevSourceIndex] == this)
	{
		Sub->IndexToActor[ClientPrevSourceIndex] = nullptr;
	}

	// Register new slot (covers promotion)
	if (SourceIndex != INDEX_NONE && SourceIndex < Sub->ActiveEntities)
		Sub->IndexToActor[SourceIndex] = this;

	ClientPrevSourceIndex = SourceIndex;
}

void AATR_ActiveEcho::OnRep_AnimStateCache()
{
	// AnimBP polls AnimStateCache directly each frame — no push needed here.
	// Override in Blueprint subclass if event-driven anim transitions are required.
}

void AATR_ActiveEcho::EnterPool()
{
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);

	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->StopMovementImmediately();
		CMC->SetComponentTickEnabled(false);
	}

	if (StateTreeComp) StateTreeComp->StopLogic(TEXT("Pooled"));
	if (AIPerception)  AIPerception->SetComponentTickEnabled(false);

	bBlockDemotion = false; // safety net — StateTree should clear this in ExitState
	SourceIndex    = INDEX_NONE;
	AnimStateCache = 0;
}

void AATR_ActiveEcho::InitFromSoA(const UATR_EchoSubsystem* Sub, int32 Index)
{
	if (!ensureAlways(Sub && Index >= 0 && Index < Sub->ActiveEntities)) return;

	// Teleport to SoA position — skip sweep so no stale collision blocks activation
	SetActorLocation(FVector(Sub->Positions[Index]), false, nullptr, ETeleportType::TeleportPhysics);

	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		// Seed velocity so SoA integration → CMC handoff is seamless
		CMC->Velocity = FVector(Sub->Velocities[Index]);
		CMC->SetComponentTickEnabled(true);
	}

	AnimStateCache = Sub->AnimState[Index];

	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	SetActorTickEnabled(true);

	// AI systems only run on authority — BeginPlay already disabled them on clients
	if (HasAuthority())
	{
		if (AIPerception) AIPerception->SetComponentTickEnabled(true);
		if (StateTreeComp) StateTreeComp->StartLogic();
	}
}

void AATR_ActiveEcho::WriteBackToSoA(UATR_EchoSubsystem* Sub) const
{
	if (!ensureAlways(Sub && SourceIndex >= 0 && SourceIndex < Sub->ActiveEntities)) return;

	Sub->Positions[SourceIndex]  = FVector3f(GetActorLocation());
	Sub->AnimState[SourceIndex]  = AnimStateCache;

	if (const UCharacterMovementComponent* CMC = GetCharacterMovement())
		Sub->Velocities[SourceIndex] = FVector3f(CMC->Velocity);
}
