// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoManager.h"
#include "ATR_EchoSubsystem.h"
#include "ATR_EchoSettings.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Async/ParallelFor.h"
#include "Net/UnrealNetwork.h"

// ─── Construction ─────────────────────────────────────────────────────────────

AATR_EchoManager::AATR_EchoManager()
{
	// Driven by Subsystem::Tick — disable Actor's own tick to avoid double-work
	PrimaryActorTick.bCanEverTick = false;

	// Must replicate so clients receive this actor and its multicast RPCs
	bReplicates     = true;
	bAlwaysRelevant = true; // multicast must reach all clients regardless of distance

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	ISM_Near = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ISM_Near"));
	ISM_Mid  = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ISM_Mid"));
	ISM_Far  = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ISM_Far"));

	ISM_Near->SetupAttachment(Root);
	ISM_Mid->SetupAttachment(Root);
	ISM_Far->SetupAttachment(Root);

	// ISM components never tick themselves — updated via UpdateISM()
	ISM_Near->SetComponentTickEnabled(false);
	ISM_Mid->SetComponentTickEnabled(false);
	ISM_Far->SetComponentTickEnabled(false);
}

void AATR_EchoManager::BeginPlay()
{
	Super::BeginPlay();

	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	NearDistance = Settings->NearDistance;
	MidDistance  = Settings->MidDistance;
	SnapshotHz   = Settings->SnapshotHz;
	SnapshotZMin = Settings->SnapshotZMin;
	SnapshotZMax = Settings->SnapshotZMax;

	// Wire ourselves into the Subsystem on all machines (server sets it via SpawnActor
	// return value; client sets it here when the replicated actor arrives).
	if (auto* Sub = GetWorld()->GetSubsystem<UATR_EchoSubsystem>())
	{
		Sub->SetManager(this);

		const int32 Cap = Sub->InitializeCount;
		AllTransforms.Reserve(Cap);
		BucketIds.Reserve(Cap);
		NearTransforms.Reserve(Cap);
		MidTransforms.Reserve(Cap);
		FarTransforms.Reserve(Cap);
		SnapshotScratch.Reserve(Cap);
	}
}

// ─── ISM Update ───────────────────────────────────────────────────────────────

void AATR_EchoManager::UpdateISM(UATR_EchoSubsystem* Sub)
{
	if (!Sub || Sub->ActiveEntities == 0) return;
	if (!ISM_Near || !ISM_Mid || !ISM_Far) return;

	const int32 Count = Sub->ActiveEntities;

	// Camera location for LOD bucketing — first local player's viewpoint
	FVector CameraLoc = FVector::ZeroVector;
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		FVector Loc; FRotator Rot;
		PC->GetPlayerViewPoint(Loc, Rot);
		CameraLoc = Loc;
	}

	const float NearSq = NearDistance * NearDistance;
	const float MidSq  = MidDistance  * MidDistance;

	AllTransforms.SetNumUninitialized(Count);
	BucketIds.SetNumUninitialized(Count);

	// Parallel: build world-space transforms + classify into LOD tiers.
	// Each lane touches only its own AllTransforms[i] and BucketIds[i] — no contention.
	ParallelFor(Count, [&](int32 i)
	{
		const FVector WP(Sub->Positions[i]);
		AllTransforms[i] = FTransform(FQuat::Identity, WP, FVector::OneVector);
		const float DSq  = FVector::DistSquaredXY(WP, CameraLoc);
		BucketIds[i]     = (DSq <= NearSq) ? 0 : (DSq <= MidSq) ? 1 : 2;
	});

	// Single-thread scatter into per-tier arrays.
	// Sequential read of BucketIds is cache-friendly (linear array).
	NearTransforms.Reset(); MidTransforms.Reset(); FarTransforms.Reset();
	for (int32 i = 0; i < Count; ++i)
	{
		// Skip promoted entities — their actor owns rendering for this slot.
		// IndexToActor is game-thread-only; UpdateISM is game-thread; no race.
		if (Sub->IndexToActor[i]) continue;

		switch (BucketIds[i])
		{
			case 0:  NearTransforms.Add(AllTransforms[i]); break;
			case 1:  MidTransforms.Add(AllTransforms[i]);  break;
			default: FarTransforms.Add(AllTransforms[i]);  break;
		}
	}

	// Batch GPU upload per tier.
	// ClearInstances + AddInstances handles tier membership changes (camera movement).
	// TODO: for moving echoes at scale, replace with stable instance-slot mapping
	//       and BatchUpdateInstancesTransforms to avoid per-frame GPU reallocation.
	auto UploadTier = [](UInstancedStaticMeshComponent* ISM, const TArray<FTransform>& Transforms)
	{
		if (!ISM->GetStaticMesh()) return;
		ISM->ClearInstances();
		if (Transforms.Num())
			ISM->AddInstances(Transforms, /*bReturnIndices=*/false);
	};

	UploadTier(ISM_Near, NearTransforms);
	UploadTier(ISM_Mid,  MidTransforms);
	UploadTier(ISM_Far,  FarTransforms);
}

// ─── Snapshot Encoding / Decoding ─────────────────────────────────────────────

FEchoSnapshot AATR_EchoManager::EncodeEcho(FVector3f Pos, uint8 AnimState, uint16 Index,
                                             float HalfExtent, float ZMin, float ZMax)
{
	FEchoSnapshot S;
	S.PosX  = static_cast<uint16>(FMath::Clamp((Pos.X + HalfExtent) / (2.f * HalfExtent), 0.f, 1.f) * 65535.f);
	S.PosY  = static_cast<uint16>(FMath::Clamp((Pos.Y + HalfExtent) / (2.f * HalfExtent), 0.f, 1.f) * 65535.f);
	S.PosZ  = static_cast<uint16>(FMath::Clamp((Pos.Z - ZMin) / (ZMax - ZMin), 0.f, 1.f) * 65535.f);
	S.Anim  = AnimState;
	S.Index = Index;
	return S;
}

FVector3f AATR_EchoManager::DecodeEcho(const FEchoSnapshot& S,
                                         float HalfExtent, float ZMin, float ZMax)
{
	return FVector3f(
		(S.PosX / 65535.f) * 2.f * HalfExtent - HalfExtent,
		(S.PosY / 65535.f) * 2.f * HalfExtent - HalfExtent,
		(S.PosZ / 65535.f) * (ZMax - ZMin) + ZMin
	);
}

// ─── Server Snapshot Sending ──────────────────────────────────────────────────

void AATR_EchoManager::ServerTick(UATR_EchoSubsystem* Sub, float DeltaTime)
{
	SnapshotAccumulator += DeltaTime;
	const float SendInterval = 1.f / FMath::Max(1.f, SnapshotHz);
	if (SnapshotAccumulator < SendInterval) return;
	SnapshotAccumulator -= SendInterval;

	if (!Sub || Sub->ActiveEntities == 0) return;

	const int32 Count      = Sub->ActiveEntities;
	const float HalfExtent = Sub->WorldHalfExtent;

	SnapshotScratch.Reset(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		SnapshotScratch.Add(
			EncodeEcho(Sub->Positions[i], Sub->AnimState[i],
			           static_cast<uint16>(i), HalfExtent, SnapshotZMin, SnapshotZMax)
		);
	}

	Multicast_EchoSnapshot(SnapshotScratch, Count);
}

// ─── Multicast RPC ────────────────────────────────────────────────────────────

void AATR_EchoManager::Multicast_EchoSnapshot_Implementation(const TArray<FEchoSnapshot>& Snapshots,
                                                               int32 TotalActiveCount)
{
	// Server already has authoritative SoA — skip decode
	const ENetMode NM = GetWorld()->GetNetMode();
	if (NM == NM_DedicatedServer || NM == NM_ListenServer) return;

	ApplySnapshotsToSubsystem(Snapshots, TotalActiveCount);
}

void AATR_EchoManager::ApplySnapshotsToSubsystem(const TArray<FEchoSnapshot>& Snapshots,
                                                   int32 TotalActiveCount)
{
	auto* Sub = GetWorld()->GetSubsystem<UATR_EchoSubsystem>();
	if (!Sub) return;

	// Sync entity count so client grid and ISM iterate the right range
	Sub->ActiveEntities = FMath::Clamp(TotalActiveCount, 0, Sub->InitializeCount);

	const float HalfExtent = Sub->WorldHalfExtent;

	for (const FEchoSnapshot& S : Snapshots)
	{
		if (S.Index >= static_cast<uint16>(Sub->ActiveEntities)) continue;
		Sub->Positions[S.Index] = DecodeEcho(S, HalfExtent, SnapshotZMin, SnapshotZMax);
		Sub->AnimState[S.Index] = S.Anim;
	}
}
