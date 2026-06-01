// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoSubsystem.h"
#include "ATR_EchoManager.h"
#include "ATR_ActiveEcho.h"
#include "ATR_EchoSettings.h"
#include "Engine/World.h"
#include "Async/ParallelFor.h"

// ─── FATR_SpatialGrid ────────────────────────────────────────────────────────

void FATR_SpatialGrid::Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize)
{
	WorldMin    = InWorldMin;
	CellSize    = FMath::Max(InCellSize, 1.f);
	InvCellSize = 1.f / CellSize;

	const FVector2D Extent = InWorldMax - InWorldMin;
	NumCellsX = FMath::Max(1, FMath::CeilToInt(Extent.X * InvCellSize));
	NumCellsY = FMath::Max(1, FMath::CeilToInt(Extent.Y * InvCellSize));

	const int32 NumCells = NumCellsX * NumCellsY;
	CellStart.SetNumUninitialized(NumCells + 1);
	CellCounts.SetNumUninitialized(NumCells);
	SortedEntities.Reset();
}

void FATR_SpatialGrid::Rebuild(TArrayView<const FVector3f> Positions, int32 Count)
{
	const int32 NumCells = NumCellsX * NumCellsY;

	// 1. Zero per-cell counts
	FMemory::Memzero(CellCounts.GetData(), NumCells * sizeof(int32));

	// 2. Count entities per cell
	for (int32 e = 0; e < Count; ++e)
		++CellCounts[CellIndex(CellX(Positions[e].X), CellY(Positions[e].Y))];

	// 3. Exclusive prefix-sum → CellStart
	int32 Running = 0;
	for (int32 c = 0; c < NumCells; ++c)
	{
		CellStart[c] = Running;
		Running      += CellCounts[c];
	}
	CellStart[NumCells] = Running;

	// 4. Scatter indices into slots (reuse CellCounts as write cursor)
	SortedEntities.SetNumUninitialized(Count, EAllowShrinking::No);
	FMemory::Memzero(CellCounts.GetData(), NumCells * sizeof(int32));
	for (int32 e = 0; e < Count; ++e)
	{
		const int32 C    = CellIndex(CellX(Positions[e].X), CellY(Positions[e].Y));
		const int32 Slot = CellStart[C] + CellCounts[C]++;
		SortedEntities[Slot] = e;
	}
}

int32 FATR_SpatialGrid::QueryRadius(FVector3f QueryPos, float Radius,
                                     TArrayView<const FVector3f> Positions,
                                     TArray<int32>& OutIndices) const
{
	const float RadiusSq   = Radius * Radius;
	const int32 StartCount = OutIndices.Num();

	const int32 MinCX = CellX(QueryPos.X - Radius), MaxCX = CellX(QueryPos.X + Radius);
	const int32 MinCY = CellY(QueryPos.Y - Radius), MaxCY = CellY(QueryPos.Y + Radius);

	for (int32 CY = MinCY; CY <= MaxCY; ++CY)
	{
		for (int32 CX = MinCX; CX <= MaxCX; ++CX)
		{
			const int32 C = CellIndex(CX, CY);
			for (int32 i = CellStart[C]; i < CellStart[C + 1]; ++i)
			{
				const int32 e  = SortedEntities[i];
				const float DX = Positions[e].X - QueryPos.X;
				const float DY = Positions[e].Y - QueryPos.Y;
				if (DX * DX + DY * DY <= RadiusSq)
					OutIndices.Add(e);
			}
		}
	}
	return OutIndices.Num() - StartCount;
}

// ─── UATR_EchoSubsystem ──────────────────────────────────────────────────────

TStatId UATR_EchoSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UATR_EchoSubsystem, STATGROUP_Tickables);
}

void UATR_EchoSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	InitializeCount = Settings->InitializeCount;
	SpawnCount      = Settings->SpawnCount;
	SpawnRadius     = Settings->SpawnRadius;
	SimHz           = Settings->SimHz;
	GridCellSize    = Settings->GridCellSize;
	WorldHalfExtent = Settings->WorldHalfExtent;
	PoolSize        = Settings->PoolSize;
	// Class refs resolved in OnWorldBeginPlay once the asset registry is ready

	// Pre-allocate SoA to full capacity — never resized again.
	// No UPROPERTY means GC never touches these; safe for worker threads.
	Forces.SetNumZeroed(InitializeCount);
	Accelerations.SetNumZeroed(InitializeCount);
	Velocities.SetNumZeroed(InitializeCount);
	Positions.SetNumZeroed(InitializeCount);
	AnimState.SetNumZeroed(InitializeCount);
	AnimFrame.SetNumZeroed(InitializeCount);
	IndexToActor.SetNumZeroed(InitializeCount); // all nullptr

	bInitialized = true;
}

void UATR_EchoSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Resolve soft class refs — asset registry is fully loaded by BeginPlay.
	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	ManagerClass    = Settings->ManagerClass.LoadSynchronous();
	ActiveEchoClass = Settings->ActiveEchoClass.LoadSynchronous();

	SpatialGrid.Initialize(
		FVector2D(-WorldHalfExtent, -WorldHalfExtent),
		FVector2D( WorldHalfExtent,  WorldHalfExtent),
		GridCellSize
	);

	// Server and standalone own the Manager. Clients receive it via replication (Phase 3).
	if (InWorld.GetNetMode() != NM_Client)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const TSubclassOf<AATR_EchoManager> SpawnClass = ManagerClass ? ManagerClass.Get() : AATR_EchoManager::StaticClass();
		Manager = InWorld.SpawnActor<AATR_EchoManager>(SpawnClass, FTransform::Identity, Params);

		for (int32 i = 0; i < SpawnCount; ++i)
		{
			// Uniform random point in a disk: sqrt(r) gives even area distribution.
			const float Angle = FMath::RandRange(0.f, 2.f * PI);
			const float R     = FMath::Sqrt(FMath::RandRange(0.f, 1.f)) * SpawnRadius;
			AddEcho(FVector3f(FMath::Cos(Angle) * R, FMath::Sin(Angle) * R, 0.f));
		}

		// Pre-warm ActiveEcho pool — actors spawn hidden and dormant.
		// Replicated actors arrive on clients automatically when un-hidden on promotion.
		const TSubclassOf<AATR_ActiveEcho> EchoClass =
			ActiveEchoClass ? ActiveEchoClass.Get() : AATR_ActiveEcho::StaticClass();

		EchoPool.Reserve(PoolSize);
		for (int32 i = 0; i < PoolSize; ++i)
		{
			FActorSpawnParameters EchoParams;
			EchoParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AATR_ActiveEcho* Echo = InWorld.SpawnActor<AATR_ActiveEcho>(EchoClass, FTransform::Identity, EchoParams))
			{
				Echo->EnterPool();
				EchoPool.Add(Echo);
			}
		}
	}
}

void UATR_EchoSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	const ENetMode NetMode = GetWorld()->GetNetMode();

	// Authoritative sim runs on server and standalone only.
	// Clients will extrapolate/receive state (Phase 3).
	if (NetMode != NM_Client && ActiveEntities > 0)
	{
		TickAccumulator += DeltaTime;
		const float SimInterval = 1.f / static_cast<float>(FMath::Max(1, SimHz));

		while (TickAccumulator >= SimInterval)
		{
			SimTick(SimInterval);
			TickAccumulator -= SimInterval;
		}
	}

	// Grid rebuilt every frame — clients rebuild from received positions.
	if (ActiveEntities > 0)
		RebuildGrid();

	if (Manager)
	{
		// ISM rendered on everything except dedicated server (no GPU there)
		if (NetMode != NM_DedicatedServer)
			Manager->UpdateISM(this);

		// Snapshot packing + multicast: server and listen-server only
		if (NetMode != NM_Client)
			Manager->ServerTick(this, DeltaTime);
	}
}

void UATR_EchoSubsystem::SimTick(float DeltaTime)
{
	const int32 Count = ActiveEntities;

	// Each entity touches only its own SoA slots — no cross-entity writes.
	// ParallelFor distributes across worker threads via the task graph.
	ParallelFor(Count, [this, DeltaTime](int32 i)
	{
		Accelerations[i]  = Forces[i]; // mass = 1
		Velocities[i]    += Accelerations[i] * DeltaTime;
		Positions[i]     += Velocities[i]    * DeltaTime;
	});
}

void UATR_EchoSubsystem::RebuildGrid()
{
	SpatialGrid.Rebuild(
		TArrayView<const FVector3f>(Positions.GetData(), ActiveEntities),
		ActiveEntities
	);
	bGridReady = true;
}

int32 UATR_EchoSubsystem::AddEcho(FVector3f Position)
{
	if (ActiveEntities >= InitializeCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("UATR_EchoSubsystem::AddEcho — SoA at capacity (%d)"), InitializeCount);
		return INDEX_NONE;
	}

	const int32 Idx    = ActiveEntities++;
	Positions[Idx]     = Position;
	Velocities[Idx]    = FVector3f::ZeroVector;
	Accelerations[Idx] = FVector3f::ZeroVector;
	Forces[Idx]        = FVector3f::ZeroVector;
	AnimState[Idx]     = 0;
	AnimFrame[Idx]     = 0;
	IndexToActor[Idx]  = nullptr;
	return Idx;
}

void UATR_EchoSubsystem::RemoveEcho(int32 Index)
{
	if (Index < 0 || Index >= ActiveEntities)
		return;

	// Caller must demote before removing — promoted actors track their SoA slot.
	ensureAlways(!IndexToActor[Index]);

	const int32 Last = --ActiveEntities;

	if (Index != Last)
	{
		// Swap-remove: copy Last into gap
		Positions[Index]     = Positions[Last];
		Velocities[Index]    = Velocities[Last];
		Accelerations[Index] = Accelerations[Last];
		Forces[Index]        = Forces[Last];
		AnimState[Index]     = AnimState[Last];
		AnimFrame[Index]     = AnimFrame[Last];

		// Patch the moved entity's Actor so it knows its new slot
		if (IndexToActor[Last])
		{
			IndexToActor[Index]              = IndexToActor[Last];
			IndexToActor[Index]->SourceIndex = Index;
			IndexToActor[Last]               = nullptr;
		}
	}
}

void UATR_EchoSubsystem::PromoteToActive(int32 SoAIndex, AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor && SoAIndex >= 0 && SoAIndex < ActiveEntities)) return;
	if (!ensureAlways(!IndexToActor[SoAIndex])) return; // double-promote guard

	IndexToActor[SoAIndex] = Actor;
	Actor->SourceIndex     = SoAIndex;
}

void UATR_EchoSubsystem::DemoteToHorde(AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor)) return;

	const int32 Idx = Actor->SourceIndex;
	if (!ensureAlways(Idx >= 0 && Idx < ActiveEntities)) return;

	Actor->WriteBackToSoA(this);  // flush Actor state → SoA before severing link
	IndexToActor[Idx]  = nullptr;
	Actor->SourceIndex = INDEX_NONE;
}

AATR_ActiveEcho* UATR_EchoSubsystem::PromoteEcho(int32 SoAIndex)
{
	if (!ensureAlways(SoAIndex >= 0 && SoAIndex < ActiveEntities)) return nullptr;
	if (EchoPool.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("UATR_EchoSubsystem::PromoteEcho — pool exhausted (SoAIndex=%d)"), SoAIndex);
		return nullptr;
	}

	// Pop from pool — actor is kept alive by the world's actor list
	TObjectPtr<AATR_ActiveEcho> PooledRef = EchoPool.Pop();
	AATR_ActiveEcho* Actor = PooledRef.Get();

	PromoteToActive(SoAIndex, Actor);    // wire IndexToActor + SourceIndex
	Actor->InitFromSoA(this, SoAIndex);  // seed position/velocity/anim, activate systems
	return Actor;
}

void UATR_EchoSubsystem::DemoteEcho(AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor)) return;
	DemoteToHorde(Actor);  // WriteBackToSoA + clear IndexToActor + SourceIndex = INDEX_NONE
	Actor->EnterPool();    // hide, disable collision, stop AI and StateTree
	EchoPool.Add(Actor);
}
