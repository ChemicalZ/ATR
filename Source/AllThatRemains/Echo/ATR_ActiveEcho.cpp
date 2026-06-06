// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_ActiveEcho.h"
#include "ATR_EchoSubsystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "Components/CapsuleComponent.h"

// ─── Construction ─────────────────────────────────────────────────────────────

AATR_ActiveEcho::AATR_ActiveEcho()
{
	// Start dormant — EnterPool/InitFromSoA control actual tick state.
	PrimaryActorTick.bCanEverTick      = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates              = true;
	bUseControllerRotationYaw = false;
	AutoPossessAI = EAutoPossessAI::Disabled; // subsystem controls possession via controller pool

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
}

// ─── BeginPlay ────────────────────────────────────────────────────────────────

void AATR_ActiveEcho::BeginPlay()
{
	Super::BeginPlay();
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

	// Unregister old slot (covers demotion: SourceIndex flips to INDEX_NONE).
	// Use InitializeCount/IsValidIndex rather than ActiveEntities because clients
	// receive partial horde snapshots and may see a promoted actor before the
	// matching SoA index has been replicated as horde data.
	if (ClientPrevSourceIndex != INDEX_NONE
		&& ClientPrevSourceIndex >= 0
		&& ClientPrevSourceIndex < Sub->InitializeCount
		&& Sub->IndexToActor.IsValidIndex(ClientPrevSourceIndex)
		&& Sub->IndexToActor[ClientPrevSourceIndex] == this)
	{
		Sub->IndexToActor[ClientPrevSourceIndex] = nullptr;
	}

	// Register new slot (covers promotion). ActiveEntities is expanded only far
	// enough for legacy index bounds checks; promoted actors are still excluded
	// from client horde grids/ISM unless horde snapshots mark them relevant.
	if (SourceIndex != INDEX_NONE
		&& SourceIndex >= 0
		&& SourceIndex < Sub->InitializeCount
		&& Sub->IndexToActor.IsValidIndex(SourceIndex))
	{
		if (Sub->ActiveEntities <= SourceIndex)
		{
			Sub->ActiveEntities = SourceIndex + 1;
		}

		Sub->IndexToActor[SourceIndex] = this;
	}

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

	bBlockDemotion = false; // safety net — StateTree (on controller) should clear this in ExitState
	SourceIndex    = INDEX_NONE;
	AnimStateCache = 0;
}

void AATR_ActiveEcho::InitFromSoA(const UATR_EchoSubsystem* Sub, int32 Index)
{
	if (!ensureAlways(Sub && Index >= 0 && Index < Sub->ActiveEntities)) return;

	// SoA stores feet/ground Z; capsule center must be offset up by half-height
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	SetActorLocation(FVector(Sub->Positions[Index]) + FVector(0.f, 0.f, HalfHeight), false, nullptr, ETeleportType::ResetPhysics);
	SetActorRotation(FRotator(0.f, Sub->Yaws[Index], 0.f));

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
	// AI starts via controller OnPossess — subsystem calls Possess() after InitFromSoA().
}

void AATR_ActiveEcho::WriteBackToSoA(UATR_EchoSubsystem* Sub) const
{
	if (!ensureAlways(Sub && SourceIndex >= 0 && SourceIndex < Sub->ActiveEntities)) return;

	// Write feet Z back so SoA stays ground-relative (matches ISM assumption)
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Sub->Positions[SourceIndex]  = FVector3f(GetActorLocation()) - FVector3f(0.f, 0.f, HalfHeight);
	Sub->Yaws[SourceIndex]       = GetActorRotation().Yaw;
	Sub->AnimState[SourceIndex]  = AnimStateCache;

	if (const UCharacterMovementComponent* CMC = GetCharacterMovement())
		Sub->Velocities[SourceIndex] = FVector3f(CMC->Velocity);
}
