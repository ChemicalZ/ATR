// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoSubsystem.h"
#include "ATR_EchoManager.h"
#include "ATR_EchoReplicationComponent.h"
#include "ATR_ActiveEcho.h"
#include "AI/ATR_EchoAIController.h"
#include "AI/ATR_EchoAILog.h"
#include "ATR_EchoSettings.h"
#include "Engine/World.h"
#include "Async/ParallelFor.h"
#include "Logging/StructuredLog.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// ─── FATR_SparseGrid ─────────────────────────────────────────────────────────

void FATR_SparseGrid::Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize)
{
	WorldMin    = InWorldMin;
	CellSize    = FMath::Max(InCellSize, 1.f);
	InvCellSize = 1.f / CellSize;

	const FVector2D Extent = InWorldMax - InWorldMin;
	NumCellsX = FMath::Max(1, FMath::CeilToInt(Extent.X * InvCellSize));
	NumCellsY = FMath::Max(1, FMath::CeilToInt(Extent.Y * InvCellSize));

	Cells.Reset();
	PopulatedCells.Reset();
}

void FATR_SparseGrid::Reset()
{
	for (int32 Key : PopulatedCells)
		if (TArray<int32>* Bucket = Cells.Find(Key)) Bucket->Reset();
	PopulatedCells.Reset();

	constexpr int32 MaxRetainedSparseCells = 4096;
	if (Cells.Num() > MaxRetainedSparseCells) Cells.Reset();
}

void FATR_SparseGrid::Build(TArrayView<const FVector3f> Positions, const TArray<int32>& LocalIndices)
{
	for (int32 e : LocalIndices)
	{
		const int32 Key = CellIndex(CellX(Positions[e].X), CellY(Positions[e].Y));
		TArray<int32>& Bucket = Cells.FindOrAdd(Key);
		if (Bucket.IsEmpty())
			PopulatedCells.Add(Key);
		Bucket.Add(e);
	}
}

int32 FATR_SparseGrid::QueryRadius(FVector3f QueryPos, float Radius,
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
			const int32 Key = CellIndex(CX, CY);
			if (const TArray<int32>* Bucket = Cells.Find(Key))
			{
				for (int32 e : *Bucket)
				{
					const float DX = Positions[e].X - QueryPos.X;
					const float DY = Positions[e].Y - QueryPos.Y;
					if (DX * DX + DY * DY <= RadiusSq)
						OutIndices.Add(e);
				}
			}
		}
	}
	return OutIndices.Num() - StartCount;
}

// ─── FATR_CoarseGrid ──────────────────────────────────────────────────────────

void FATR_CoarseGrid::Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize)
{
	WorldMin    = InWorldMin;
	CellSize    = FMath::Max(InCellSize, 1.f);
	InvCellSize = 1.f / CellSize;

	const FVector2D Extent = InWorldMax - InWorldMin;
	NumCellsX = FMath::Max(1, FMath::CeilToInt(Extent.X * InvCellSize));
	NumCellsY = FMath::Max(1, FMath::CeilToInt(Extent.Y * InvCellSize));

	CellEntities.Reset();
}

void FATR_CoarseGrid::AddEntity(int32 EntityIndex, int32 CellId, int32& OutSlotInCell)
{
	TArray<int32>& Bucket = CellEntities.FindOrAdd(CellId);
	OutSlotInCell = Bucket.Num();
	Bucket.Add(EntityIndex);
}

int32 FATR_CoarseGrid::RemoveEntityAndReturnMoved(int32 EntityIndex, int32 CellId, int32 SlotInCell)
{
	TArray<int32>* Bucket = CellEntities.Find(CellId);
	if (!ensureAlways(Bucket)) return INDEX_NONE;
	if (!ensureAlways(Bucket->IsValidIndex(SlotInCell))) return INDEX_NONE;
	if (!ensureAlways((*Bucket)[SlotInCell] == EntityIndex)) return INDEX_NONE;

	const int32 LastSlot    = Bucket->Num() - 1;
	const int32 MovedEntity = (*Bucket)[LastSlot];
	Bucket->RemoveAtSwap(SlotInCell, 1, EAllowShrinking::No);
	if (Bucket->IsEmpty()) { CellEntities.Remove(CellId); return INDEX_NONE; }
	return (SlotInCell != LastSlot) ? MovedEntity : INDEX_NONE;
}

void FATR_CoarseGrid::ReplaceEntityAtSlot(int32 CellId, int32 SlotInCell, int32 ExpectedOld, int32 NewEntity)
{
	TArray<int32>* Bucket = CellEntities.Find(CellId);
	if (!ensureAlways(Bucket)) return;
	if (!ensureAlways(Bucket->IsValidIndex(SlotInCell))) return;
	if (!ensureAlways((*Bucket)[SlotInCell] == ExpectedOld)) return;
	(*Bucket)[SlotInCell] = NewEntity;
}

bool FATR_CoarseGrid::ValidateEntitySlot(int32 EntityIndex, int32 CellId, int32 SlotInCell) const
{
	const TArray<int32>* Bucket = CellEntities.Find(CellId);
	if (!Bucket) return false;
	if (!Bucket->IsValidIndex(SlotInCell)) return false;
	return (*Bucket)[SlotInCell] == EntityIndex;
}

// ─── UATR_EchoSubsystem ──────────────────────────────────────────────────────

TStatId UATR_EchoSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UATR_EchoSubsystem, STATGROUP_Tickables);
}

void UATR_EchoSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Defensive runtime clamp — editor PostEditChangeProperty handles in-editor edits,
	// but config files can be hand-edited, so re-validate invariants before reading.
	GetMutableDefault<UATR_EchoSettings>()->ValidateAndClamp();

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
	MustPromoteRadius    = Settings->MustPromoteRadius;
	HordeWalkSpeed       = Settings->HordeWalkSpeed;
	PromoteRadius        = Settings->PromoteRadius;
	DemoteRadius         = Settings->DemoteRadius;
	MinTimeInTierSeconds = Settings->MinTimeInTierSeconds;
	LocalZoneRadius      = Settings->LocalZoneRadius;
	CoarseGridCellSize   = Settings->CoarseGridCellSize;

	Forces.SetNumZeroed(InitializeCount);
	Accelerations.SetNumZeroed(InitializeCount);
	Velocities.SetNumZeroed(InitializeCount);
	Positions.SetNumZeroed(InitializeCount);
	AnimState.SetNumZeroed(InitializeCount);
	AnimFrame.SetNumZeroed(InitializeCount);
	PromotionTimes.SetNumZeroed(InitializeCount);
	Yaws.SetNumZeroed(InitializeCount);
	DirtyStates.SetNumZeroed(InitializeCount);
	IndexToActor.SetNumZeroed(InitializeCount); // all nullptr

	// Canonical runtime state (Phase 1) — parallel to the SoA arrays above.
	// EchoIds default to INDEX_NONE for unused rows; RuntimeStates default-construct.
	EchoIds.Init(INDEX_NONE, InitializeCount);
	RuntimeStates.SetNum(InitializeCount);
	EchoIdToIndex.Reserve(InitializeCount);
	NextEchoId = 1;
	CoarseCellIds.Init(INDEX_NONE, InitializeCount);
	CoarseSlotInCell.Init(INDEX_NONE, InitializeCount);
	ClientRelevantEchoMask.Init(false, InitializeCount);
	ClientRelevantEchoIndices.Reserve(256);
	ClientRelevantEchoSlots.Init(INDEX_NONE, InitializeCount);
	LocalVisitStamp.Init(0, InitializeCount);
	LocalVisitEpoch = 1;

	PositionDirtyThresholdSq = FMath::Square(Settings->PositionDirtyThreshold);
	YawDirtyThresholdDeg     = Settings->YawDirtyThresholdDegrees;

	ServerReplicationBudgetMs      = Settings->ServerReplicationBudgetMs;
	MaxReplicationJobsPerFrame     = Settings->MaxReplicationJobsPerFrame;
	MaxSnapshotsPerClientPerFrame  = Settings->MaxSnapshotsPerClientPerFrame;
	MaxNearReplicationJobsPerFrame = Settings->MaxNearReplicationJobsPerFrame;
	MaxMidReplicationJobsPerFrame  = Settings->MaxMidReplicationJobsPerFrame;
	MaxFarReplicationJobsPerFrame  = Settings->MaxFarReplicationJobsPerFrame;

	PromotedIndices.Reserve(PoolSize);
	LocalEntityScratch.Reserve(256);
	LastLocalEntityScratch.Reserve(512);

	bInitialized = true;
}

void UATR_EchoSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Resolve soft class refs — asset registry is fully loaded by BeginPlay.
	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	ManagerClass    = Settings->ManagerClass.LoadSynchronous();
	ActiveEchoClass = Settings->ActiveEchoClass.LoadSynchronous();
	ControllerClass = Settings->ControllerClass.LoadSynchronous();

	SpatialGrid.Initialize(
		FVector2D(-WorldHalfExtent, -WorldHalfExtent),
		FVector2D( WorldHalfExtent,  WorldHalfExtent),
		GridCellSize
	);

	CoarseGrid.Initialize(
		FVector2D(-WorldHalfExtent, -WorldHalfExtent),
		FVector2D( WorldHalfExtent,  WorldHalfExtent),
		CoarseGridCellSize
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
		ControllerPool.Reserve(PoolSize);

		const TSubclassOf<AATR_EchoAIController> AIControllerClass =
			ControllerClass ? ControllerClass.Get() : AATR_EchoAIController::StaticClass();

		for (int32 i = 0; i < PoolSize; ++i)
		{
			FActorSpawnParameters EchoParams;
			EchoParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AATR_ActiveEcho* Echo = InWorld.SpawnActor<AATR_ActiveEcho>(EchoClass, FTransform::Identity, EchoParams))
			{
				Echo->EnterPool();
				EchoPool.Add(Echo);
			}

			FActorSpawnParameters CtrlParams;
			CtrlParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AATR_EchoAIController* Ctrl = InWorld.SpawnActor<AATR_EchoAIController>(AIControllerClass, FTransform::Identity, CtrlParams))
			{
				Ctrl->EnterPool();
				ControllerPool.Add(Ctrl);
			}
		}
	}
}

void UATR_EchoSubsystem::Tick(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoSubsystem_Tick);
	Super::Tick(DeltaTime);

	const ENetMode NetMode = GetWorld()->GetNetMode();

	// ── Server / Standalone ───────────────────────────────────────────────────
	// Sim runs at SimHz — coarse grid updated inside the fixed step loop so it
	// stays current after integration. Fine grid rebuilt once per render tick.
	if (NetMode != NM_Client && ActiveEntities > 0)
	{
		TickAccumulator += DeltaTime;
		const float SimInterval = 1.f / static_cast<float>(FMath::Max(1, SimHz));
		while (TickAccumulator >= SimInterval)
		{
			SimTick(SimInterval);
			UpdateCoarseGrid();
			TickAccumulator -= SimInterval;
		}

		RebuildFineGrid();
		RunSteeringPass();
		RunPromotionPass();
		DecayAgitationField(DeltaTime);
		RunIntentPass(DeltaTime);
	}

	// ── Client ────────────────────────────────────────────────────────────────
	// Clients receive positions via replication. They must update their coarse
	// grid before rebuilding the fine grid — otherwise ForEachEntityInRadius
	// queries an empty CellEntities map.
	if (NetMode == NM_Client && ActiveEntities > 0)
	{
		UpdateCoarseGrid();
		RebuildFineGrid();
	}

	if (Manager && NetMode != NM_DedicatedServer)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Echo_UpdateISM);
		Manager->UpdateISM(this);
	}

	if (NetMode != NM_Client)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ServerReplication);
		TickReplicationScheduler(DeltaTime);
	}
}

void UATR_EchoSubsystem::SimTick(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_SimTick);
	const int32 Count = ActiveEntities;

	// Each entity touches only its own SoA slots — no cross-entity writes.
	// ParallelFor distributes across worker threads via the task graph.
	// MarkEchoDirty writes only DirtyStates[i] per lane — no data race.
	ParallelFor(Count, [this, DeltaTime](int32 i)
	{
		if (IndexToActor[i]) return; // actor owns its movement; SoA written back on demotion
		const FVector3f OldPos = Positions[i];
		Accelerations[i]  = Forces[i]; // mass = 1
		Velocities[i]    += Accelerations[i] * DeltaTime;
		Positions[i]     += Velocities[i]    * DeltaTime;
		const FVector3f Delta = Positions[i] - OldPos;
		if (Delta.SizeSquared() > PositionDirtyThresholdSq)
			MarkEchoDirty(i, EEchoDirtyFlags::Transform);
	});
}

void UATR_EchoSubsystem::RebuildFineGrid()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RebuildFineGrid);

	if (++LocalVisitEpoch == 0)
	{
		LocalVisitStamp.Init(0, InitializeCount);
		LocalVisitEpoch = 1;
	}

	LocalEntityScratch.Reset();
	const TArrayView<const FVector3f> PosView(Positions.GetData(), ActiveEntities);

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RebuildFineGrid_GatherLocal);
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (!PC || !PC->GetPawn()) continue;
			const FVector Loc = PC->GetPawn()->GetActorLocation();
			const FVector2f XY(static_cast<float>(Loc.X), static_cast<float>(Loc.Y));

			CoarseGrid.ForEachEntityInRadius(XY, LocalZoneRadius, PosView, [this](int32 Ei)
			{
				if (!ShouldProcessEchoForLocalHorde(Ei)) return;
				if (IndexToActor[Ei]) return; // promoted — owned by actor, not horde grid
				if (LocalVisitStamp[Ei] == LocalVisitEpoch) return;
				LocalVisitStamp[Ei] = LocalVisitEpoch;
				LocalEntityScratch.Add(Ei);
			});
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RebuildFineGrid_ZeroExited);
		// Zero velocities for unpromoted entities that left the local zone since last frame.
		// Without this, an entity that was steered and then exited the zone would
		// continue integrating a stale velocity indefinitely.
		// Bounds check guards against indices invalidated by swap-remove in RemoveEcho.
		for (int32 Ei : LastLocalEntityScratch)
		{
			if (Ei < 0 || Ei >= ActiveEntities) continue;
			if (!ShouldProcessEchoForLocalHorde(Ei)) continue;
			if (LocalVisitStamp[Ei] != LocalVisitEpoch && !IndexToActor[Ei])
				Velocities[Ei] = FVector3f::ZeroVector;
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RebuildFineGrid_BuildSparseGrid);
		SpatialGrid.Reset();
		SpatialGrid.Build(PosView, LocalEntityScratch);
		bGridReady = true;
	}

	LastLocalEntityScratch.Reset();
	LastLocalEntityScratch.Append(LocalEntityScratch);
}

void UATR_EchoSubsystem::UpdateCoarseGrid()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_UpdateCoarseGrid);

	const UWorld* World = GetWorld();
	const bool bClientPartialReplication = World && World->GetNetMode() == NM_Client;

	auto UpdateOne = [this](int32 i)
	{
		if (!ShouldProcessEchoForLocalHorde(i)) return;
		if (IndexToActor[i]) return; // promoted entity SoA position is stale until demotion
		const int32 NewCell = CoarseGrid.GetCellId(FVector2f(Positions[i].X, Positions[i].Y));
		if (NewCell != CoarseCellIds[i]) MoveEntityCoarseCell(i, NewCell);
	};

	if (bClientPartialReplication)
	{
		for (int32 i : ClientRelevantEchoIndices)
		{
			UpdateOne(i);
		}
		return;
	}

	for (int32 i = 0; i < ActiveEntities; ++i)
	{
		UpdateOne(i);
	}
}

// ─── Coarse Registration Helpers ─────────────────────────────────────────────

void UATR_EchoSubsystem::RegisterEntityToCoarseGrid(int32 EntityIndex)
{
	if (!ensureAlways(EntityIndex >= 0 && EntityIndex < ActiveEntities)) return;
	if (!ensureAlways(CoarseGrid.IsInitialized())) return;
	if (CoarseCellIds[EntityIndex] != INDEX_NONE) return;

	const FVector3f& P  = Positions[EntityIndex];
	const int32 CellId  = CoarseGrid.GetCellId(FVector2f(P.X, P.Y));
	int32 SlotInCell    = INDEX_NONE;
	CoarseGrid.AddEntity(EntityIndex, CellId, SlotInCell);
	CoarseCellIds[EntityIndex]    = CellId;
	CoarseSlotInCell[EntityIndex] = SlotInCell;
}

void UATR_EchoSubsystem::UnregisterEntityFromCoarseGrid(int32 EntityIndex)
{
	if (!ensureAlways(EntityIndex >= 0 && EntityIndex < ActiveEntities)) return;
	const int32 CellId    = CoarseCellIds[EntityIndex];
	const int32 SlotInCell = CoarseSlotInCell[EntityIndex];
	if (CellId == INDEX_NONE) { CoarseSlotInCell[EntityIndex] = INDEX_NONE; return; }

	const int32 MovedEntity = CoarseGrid.RemoveEntityAndReturnMoved(EntityIndex, CellId, SlotInCell);
	if (MovedEntity != INDEX_NONE) CoarseSlotInCell[MovedEntity] = SlotInCell;

	CoarseCellIds[EntityIndex]    = INDEX_NONE;
	CoarseSlotInCell[EntityIndex] = INDEX_NONE;
}

void UATR_EchoSubsystem::MoveEntityCoarseCell(int32 EntityIndex, int32 NewCellId)
{
	if (!ensureAlways(EntityIndex >= 0 && EntityIndex < ActiveEntities)) return;
	const int32 OldCellId = CoarseCellIds[EntityIndex];
	if (OldCellId == NewCellId) return;

	if (OldCellId != INDEX_NONE)
	{
		const int32 OldSlot   = CoarseSlotInCell[EntityIndex];
		const int32 MovedEnt  = CoarseGrid.RemoveEntityAndReturnMoved(EntityIndex, OldCellId, OldSlot);
		if (MovedEnt != INDEX_NONE) CoarseSlotInCell[MovedEnt] = OldSlot;
	}

	int32 NewSlot = INDEX_NONE;
	CoarseGrid.AddEntity(EntityIndex, NewCellId, NewSlot);
	CoarseCellIds[EntityIndex]    = NewCellId;
	CoarseSlotInCell[EntityIndex] = NewSlot;
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
	AnimState[Idx]      = 0;
	AnimFrame[Idx]      = 0;
	PromotionTimes[Idx] = 0.f;
	Yaws[Idx]           = 0.f;
	DirtyStates[Idx]    = FEchoDirtyState{ EEchoDirtyFlags::Spawn, 1 };
	IndexToActor[Idx]   = nullptr;
	CoarseCellIds[Idx]    = INDEX_NONE;
	CoarseSlotInCell[Idx] = INDEX_NONE;
	LocalVisitStamp[Idx]  = 0;

	// Assign a fresh stable EchoId and seed canonical runtime state for this row.
	const int32 NewEchoId = NextEchoId++;
	EchoIds[Idx] = NewEchoId;
	RuntimeStates[Idx].ResetForReuse(NewEchoId);
	RuntimeStates[Idx].Location = FVector(Position);
	RuntimeStates[Idx].Tier     = EATR_EchoSimulationTier::Abstract;
	EchoIdToIndex.Add(NewEchoId, Idx);

	if (CoarseGrid.IsInitialized())
		RegisterEntityToCoarseGrid(Idx);

	return Idx;
}

void UATR_EchoSubsystem::RemoveEcho(int32 Index)
{
	if (Index < 0 || Index >= ActiveEntities)
		return;

	if (IndexToActor[Index])
	{
		DemoteEcho(IndexToActor[Index]);
		if (!ensureAlways(IndexToActor[Index] == nullptr))
			return;
	}

	// Remove entity from coarse grid before its slot is reused.
	UnregisterEntityFromCoarseGrid(Index);

	// Capture the stable EchoId being removed before any swap overwrites the row.
	const int32 RemovedEchoId = EchoIds.IsValidIndex(Index) ? EchoIds[Index] : INDEX_NONE;

	const int32 Last = ActiveEntities - 1; // capture before decrement

	// Patch LastLocalEntityScratch to mirror the swap-remove.
	// Index is deleted; Last moves into Index. If Last was tracked as local, re-add it as Index.
	{
		const bool bMovedLastWasLocal = Index != Last && LastLocalEntityScratch.Contains(Last);
		LastLocalEntityScratch.RemoveAllSwap(
			[Index, Last](int32 Ei) { return Ei == Index || Ei == Last; },
			EAllowShrinking::No
		);
		if (bMovedLastWasLocal)
			LastLocalEntityScratch.Add(Index);
	}

	if (Index != Last)
	{
		// Swap-remove: copy Last into the gap.
		Positions[Index]     = Positions[Last];
		Velocities[Index]    = Velocities[Last];
		Accelerations[Index] = Accelerations[Last];
		Forces[Index]        = Forces[Last];
		AnimState[Index]      = AnimState[Last];
		AnimFrame[Index]      = AnimFrame[Last];
		PromotionTimes[Index] = PromotionTimes[Last];
		DirtyStates[Index]    = DirtyStates[Last];
		Yaws[Index]           = Yaws[Last];

		// Capture before overwrite — needed for ReplaceEntityAtSlot and for the
		// same-cell case where UnregisterEntityFromCoarseGrid may have updated
		// CoarseSlotInCell[Last] already.
		const int32 OldLastCoarseCell = CoarseCellIds[Last];
		const int32 OldLastCoarseSlot = CoarseSlotInCell[Last];

		CoarseCellIds[Index]    = OldLastCoarseCell;
		CoarseSlotInCell[Index] = OldLastCoarseSlot;
		LocalVisitStamp[Index]  = LocalVisitStamp[Last];

		// Move canonical identity + runtime state from Last into the vacated row and
		// repoint the moved EchoId at its new SoA index.
		EchoIds[Index]       = EchoIds[Last];
		RuntimeStates[Index] = MoveTemp(RuntimeStates[Last]);
		if (EchoIds[Index] != INDEX_NONE)
			EchoIdToIndex[EchoIds[Index]] = Index;
		EchoIds[Last] = INDEX_NONE;
		RuntimeStates[Last] = FATR_EchoRuntimeState{};

		// Patch the coarse bucket: the slot still holds Last; relabel it to Index.
		if (OldLastCoarseCell != INDEX_NONE)
			CoarseGrid.ReplaceEntityAtSlot(OldLastCoarseCell, OldLastCoarseSlot, Last, Index);

		// Clear the vacated Last slot.
		CoarseCellIds[Last]    = INDEX_NONE;
		CoarseSlotInCell[Last] = INDEX_NONE;
		LocalVisitStamp[Last]  = 0;

		// Patch the moved entity's Actor so it knows its new slot.
		if (IndexToActor[Last])
		{
			IndexToActor[Index]              = IndexToActor[Last];
			IndexToActor[Index]->SourceIndex = Index;
			IndexToActor[Last]               = nullptr;

			// Patch PromotedIndices: Last moved to Index.
			const int32 PIIdx = PromotedIndices.IndexOfByKey(Last);
			if (ensureAlways(PIIdx != INDEX_NONE))
				PromotedIndices[PIIdx] = Index;
		}
	}
	else
	{
		// Removing the tail row outright — clear its canonical identity/state.
		EchoIds[Index]      = INDEX_NONE;
		RuntimeStates[Index] = FATR_EchoRuntimeState{};
	}

	// Drop the removed EchoId from the lookup map. Done after the swap so the moved
	// entity's (different) id has already been repointed above.
	if (RemovedEchoId != INDEX_NONE)
		EchoIdToIndex.Remove(RemovedEchoId);

	--ActiveEntities; // decrement last — helpers above need the valid range
}

// ─── Canonical Runtime State (Phase 1) ───────────────────────────────────────

FATR_EchoRuntimeState* UATR_EchoSubsystem::GetMutableEchoState(int32 EchoId)
{
	const int32 Index = GetIndexForEchoId(EchoId);
	return RuntimeStates.IsValidIndex(Index) ? &RuntimeStates[Index] : nullptr;
}

const FATR_EchoRuntimeState* UATR_EchoSubsystem::GetEchoState(int32 EchoId) const
{
	const int32 Index = GetIndexForEchoId(EchoId);
	return RuntimeStates.IsValidIndex(Index) ? &RuntimeStates[Index] : nullptr;
}

void UATR_EchoSubsystem::RegisterActiveEcho(int32 EchoId, AATR_EchoAIController* Controller, APawn* Pawn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RegisterActiveEcho);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!ensureAlways(State))
	{
		UE_LOG(LogATR_EchoAI, Warning, TEXT("RegisterActiveEcho — no runtime state for EchoId %d"), EchoId);
		return;
	}

	State->Tier             = EATR_EchoSimulationTier::Active;
	State->ActiveController  = Controller;
	State->ActivePawn        = Pawn;

	// Promotion continuity: canonical awareness/search/intent are intentionally PRESERVED so a
	// re-promoted Echo resumes the chase/search it was on. Seed only the live transform (so the
	// first intent tick measures distances correctly) and drop a long-stale obstacle hook and
	// any leftover in-flight move flag — those belong to a previous, torn-down path follow.
	const int32 Index = GetIndexForEchoId(EchoId);
	if (Positions.IsValidIndex(Index))
	{
		State->Location = FVector(Positions[Index]);
		const float YawRad = FMath::DegreesToRadians(Yaws[Index]);
		State->FacingDirection = FVector(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.f);
	}

	State->Movement.bMoveInProgress = false;

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	if (State->Obstacle.bHasObstacle && (Now - State->Obstacle.LastObstacleTime) > ObstacleHandleTimeoutSeconds)
		State->Obstacle.bHasObstacle = false;

	UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("RegisterActiveEcho — EchoId %d → %s"),
		EchoId, Controller ? *Controller->GetName() : TEXT("null"));
}

void UATR_EchoSubsystem::UnregisterActiveEcho(int32 EchoId)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_UnregisterActiveEcho);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State) return; // already gone (e.g. echo destroyed) — nothing to sever

	// Keep canonical awareness/intent/search (MEMORY survives demotion); only drop the active
	// bridge + tier and the live-only facts that can't hold without an active perception/path:
	//   - no live line of sight while dormant (but LastSeenLocation/Confidence are kept),
	//   - no in-flight move (the path-following component was torn down on unpossess).
	State->Tier            = EATR_EchoSimulationTier::LowDetail;
	State->ActiveController = nullptr;
	State->ActivePawn       = nullptr;

	State->Awareness.bHasCurrentLineOfSight = false;
	State->Awareness.ConfirmedVisibleActor  = nullptr;
	State->Movement.bMoveInProgress          = false;

	UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("UnregisterActiveEcho — EchoId %d"), EchoId);
}

// ─── Perception Fact Reporting (Phase 2) ─────────────────────────────────────

void UATR_EchoSubsystem::ReportEchoSawActor(int32 EchoId, AActor* Actor, const FVector& Location, const FVector& Velocity, float TimeSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ReportSawActor);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State || !IsValid(Actor)) return;

	FATR_EchoAwarenessState& A = State->Awareness;
	A.Mode                  = EATR_AwarenessMode::SawTarget;
	A.ConfirmedVisibleActor = Actor;
	A.bHasCurrentLineOfSight = true;
	A.LastSeenLocation      = Location;
	A.LastSeenVelocity      = Velocity;
	A.LastSeenTime          = TimeSeconds;
	A.Confidence            = 1.f;            // fresh sight = full confidence
	A.Urgency               = FMath::Max(A.Urgency, 1.f);

	// Indirect spread: a seeing Echo agitates its neighborhood and biases pressure toward the
	// action — but deposits only a scalar + direction, never the target actor. Neighbors get
	// curious/pulled; distant edge Echoes get nothing and can peel away.
	AddWorldAgitation(State->Location, 0.6f, (Location - State->Location));

	State->LastUpdateTime = TimeSeconds;
}

void UATR_EchoSubsystem::ReportEchoLostSight(int32 EchoId, AActor* Actor, const FVector& LastKnownLocation, const FVector& LastKnownVelocity, float TimeSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ReportLostSight);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State) return;

	FATR_EchoAwarenessState& A = State->Awareness;

	// Only the actor we were actually confirming should clear line-of-sight. A lost-sight
	// report for some other perceived actor must not wipe a live confirmation.
	const bool bWasConfirmedActor = (A.ConfirmedVisibleActor.Get() == Actor) || !A.ConfirmedVisibleActor.IsValid();
	if (!bWasConfirmedActor) return;

	A.Mode                  = EATR_AwarenessMode::LostSightSearch;
	A.ConfirmedVisibleActor = nullptr;
	A.bHasCurrentLineOfSight = false;
	A.LastSeenLocation      = LastKnownLocation;
	A.LastSeenVelocity      = LastKnownVelocity;
	A.LastSeenTime          = TimeSeconds;
	// Confidence/urgency intentionally preserved here — they decay in Phase 3 intent update.

	State->LastUpdateTime = TimeSeconds;
}

void UATR_EchoSubsystem::ReportEchoHeardLocation(int32 EchoId, const FVector& Location, float Strength, float TimeSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ReportHeardLocation);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State) return;

	FATR_EchoAwarenessState& A = State->Awareness;
	A.LastHeardLocation = Location;
	A.LastHeardTime     = TimeSeconds;

	// Hearing never sets ConfirmedVisibleActor and never upgrades over a live sight.
	// It only takes over when we have no stronger awareness driver.
	if (A.Mode == EATR_AwarenessMode::None
		|| A.Mode == EATR_AwarenessMode::HeardLocation
		|| A.Mode == EATR_AwarenessMode::HordeAgitated)
	{
		A.Mode = EATR_AwarenessMode::HeardLocation;
	}

	const float Loud = FMath::Clamp(Strength, 0.f, 1.f);
	A.Urgency = FMath::Max(A.Urgency, Loud);

	// Mild agitation contribution — even weak noise nudges horde pressure (Phase 8). Strong
	// noise raises urgency enough to investigate; weak noise mostly just agitates/orients.
	State->Agitation = FMath::Min(1.f, State->Agitation + Loud * 0.25f);

	State->LastUpdateTime = TimeSeconds;
}

void UATR_EchoSubsystem::ReportEchoStimulus(int32 EchoId, const FATR_StimulusEvent& Event)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ReportStimulus);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State) return;

	// Route by stimulus type. All routes are location-based; SourceActor_DebugOnly is
	// never consulted for behavior. Richer routing (smell trails, blood) lands in later phases.
	switch (Event.Type)
	{
		case EATR_StimulusType::Noise:
		case EATR_StimulusType::DoorImpact:
		case EATR_StimulusType::WindowImpact:
		case EATR_StimulusType::Combat:
		case EATR_StimulusType::Scripted:
			ReportEchoHeardLocation(EchoId, Event.Location, Event.Strength, Event.TimeSeconds);
			// Loud world events also deposit directional horde pressure at their source.
			AddWorldAgitation(Event.Location, Event.Strength, Event.Direction);
			break;

		case EATR_StimulusType::Smell:
			State->Awareness.LastSmelledLocation = Event.Location;
			State->Awareness.LastSmelledTime     = Event.TimeSeconds;
			break;

		case EATR_StimulusType::Blood:
		case EATR_StimulusType::EchoAgitation:
			// Agitation contribution — fully modeled in Phase 8. Bump local agitation now.
			State->Agitation = FMath::Min(State->Agitation + FMath::Max(Event.Strength, 0.f), 1.f);
			break;
	}

	State->LastUpdateTime = Event.TimeSeconds;
}

void UATR_EchoSubsystem::ReportEchoMoveResult(int32 EchoId, bool bSuccess, EATR_MoveFailureReason Reason,
                                              const FVector& Location, AActor* BlockingActor, float TimeSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ReportMoveResult);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State) return;

	FATR_EchoMovementIntent& M = State->Movement;
	M.bMoveInProgress    = false;
	M.bLastMoveSucceeded = bSuccess;
	M.LastFailure        = bSuccess ? EATR_MoveFailureReason::None : Reason;
	M.LastResultTime     = TimeSeconds;

	// Seed the obstacle hook for blocked/unreachable failures so HandleObstacle (Phase 9)
	// has a classified record to act on. Successful/aborted moves clear it.
	FATR_EchoObstacleIntent& O = State->Obstacle;
	const bool bIsObstacleFailure =
		!bSuccess &&
		(Reason == EATR_MoveFailureReason::BlockedByDynamicActor ||
		 Reason == EATR_MoveFailureReason::BlockedByDoor   ||
		 Reason == EATR_MoveFailureReason::BlockedByWindow ||
		 Reason == EATR_MoveFailureReason::BlockedByFence  ||
		 Reason == EATR_MoveFailureReason::TargetUnreachable);

	if (bIsObstacleFailure)
	{
		O.bHasObstacle     = true;
		O.Reason           = Reason;
		O.ObstacleActor    = BlockingActor;
		O.ObstacleLocation = Location;
		O.LastObstacleTime = TimeSeconds;
	}
	else if (bSuccess)
	{
		O.bHasObstacle = false;
	}

	State->LastUpdateTime = TimeSeconds;

	UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("MoveResult EchoId %d: %s (reason %d)"),
		EchoId, bSuccess ? TEXT("success") : TEXT("FAIL"), static_cast<int32>(Reason));
}

// ─── Horde Agitation Field (Phase 8) ─────────────────────────────────────────

void UATR_EchoSubsystem::AddWorldAgitation(const FVector& Location, float Amount, const FVector& Direction)
{
	if (Amount <= 0.f) return;
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_AddWorldAgitation);

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	FATR_AgitationCell& Cell = AgitationField.FindOrAdd(AgitationCellKey(Location));
	Cell.Agitation        = FMath::Min(1.f, Cell.Agitation + Amount);
	Cell.WeightedDirection += Direction.GetSafeNormal2D() * Amount; // accumulate; normalized on read
	Cell.LastUpdatedTime   = Now;
}

void UATR_EchoSubsystem::DecayAgitationField(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_DecayAgitationField);

	const float Decay = AgitationFieldDecayPerSec * DeltaTime;

	for (auto It = AgitationField.CreateIterator(); It; ++It)
	{
		FATR_AgitationCell& Cell = It.Value();
		Cell.Agitation        = FMath::Max(0.f, Cell.Agitation - Decay);
		Cell.WeightedDirection *= FMath::Max(0.f, 1.f - Decay); // direction fades with pressure

		if (Cell.Agitation <= KINDA_SMALL_NUMBER)
			It.RemoveCurrent(); // prune dead cells so the map stays proportional to active hotspots
	}
}

void UATR_EchoSubsystem::SampleAgitationField(const FVector& Location, float& OutAgitation, FVector& OutDirection) const
{
	OutAgitation = 0.f;
	OutDirection = FVector::ZeroVector;

	if (AgitationField.IsEmpty()) return;

	// Blend the 3x3 neighborhood so pressure spreads smoothly across cell borders. The center
	// cell dominates; neighbors contribute toward the edge so groups drift toward hotspots.
	const FIntPoint Center = AgitationCellKey(Location);
	FVector AccumDir = FVector::ZeroVector;

	for (int32 dy = -1; dy <= 1; ++dy)
	for (int32 dx = -1; dx <= 1; ++dx)
	{
		const FATR_AgitationCell* Cell = AgitationField.Find(FIntPoint(Center.X + dx, Center.Y + dy));
		if (!Cell || Cell->Agitation <= 0.f) continue;

		const float Weight = (dx == 0 && dy == 0) ? 1.f : 0.5f;
		OutAgitation = FMath::Max(OutAgitation, Cell->Agitation * Weight);
		AccumDir    += Cell->WeightedDirection * Weight;
	}

	OutDirection = AccumDir.GetSafeNormal2D();
}

// ─── Intent Selection (Phase 3) + Lost-Sight Search (Phase 6) ────────────────

namespace
{
	// Deterministic per-Echo [0,1) hash. Stable across frames (keyed by EchoId), so each
	// Echo searches with its own consistent variation instead of identical robotic paths.
	float EchoHash01(int32 EchoId, uint32 Salt)
	{
		uint32 H = static_cast<uint32>(EchoId) * 2654435761u + Salt * 40503u;
		H ^= H >> 13; H *= 0x85ebca6bu; H ^= H >> 16;
		return static_cast<float>(H & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
	}

	// Begin a fresh lost-sight search anchored at the last-seen location, biased along the
	// target's last-known travel direction with per-Echo angular/radius/duration variation.
	// The search radius is derived from a CLAMPED velocity projection so prediction can never
	// be supernatural (ProjectionSeconds * speed, capped at MaxProjectionDistance).
	void BeginEchoSearch(FATR_EchoRuntimeState& State, float Now, float ProjectionSeconds, float MaxProjectionDistance)
	{
		FATR_EchoAwarenessState& A = State.Awareness;
		FATR_EchoSearchState&    S = State.Search;

		FVector Dir = A.LastSeenVelocity.GetSafeNormal2D();
		if (Dir.IsNearlyZero()) Dir = State.FacingDirection.GetSafeNormal2D();
		if (Dir.IsNearlyZero()) Dir = FVector::ForwardVector;

		// Per-Echo angular jitter (+/- 20deg) so a group fans out instead of stacking.
		const float JitterDeg = (EchoHash01(State.EchoId, 1) - 0.5f) * 40.f;
		Dir = Dir.RotateAngleAxis(JitterDeg, FVector::UpVector);

		const float Lead = FMath::Clamp(A.LastSeenVelocity.Size2D() * ProjectionSeconds, 300.f, MaxProjectionDistance);

		S.bSearchActive    = true;
		S.Origin           = A.LastSeenLocation;
		S.PrimaryDirection = Dir;
		S.SearchStepIndex  = 0;
		S.StartedTime      = Now;
		// Aggressive echoes sweep wider and persist longer.
		S.SearchRadius      = Lead * (0.8f + 0.6f * EchoHash01(State.EchoId, 2) + 0.3f * State.Aggression);
		S.MaxSearchDuration = 12.f * (0.7f + 0.6f * EchoHash01(State.EchoId, 3) + 0.3f * State.Aggression);
	}

	// Compute the world point for the current search step. Returns false once the fan
	// pattern is exhausted. Pattern mirrors the design doc (forward probe, then widening
	// left/right fan), scaled by the Echo's varied SearchRadius.
	bool ComputeEchoSearchPoint(const FATR_EchoRuntimeState& State, FVector& OutPoint)
	{
		// {forward, side} multipliers of SearchRadius. Index 0 = projected direction.
		static const FVector2f Pattern[] = {
			FVector2f(1.0f,  0.0f),
			FVector2f(0.7f,  0.6f),
			FVector2f(0.7f, -0.6f),
			FVector2f(1.4f,  1.0f),
			FVector2f(1.4f, -1.0f),
		};
		const int32 N = UE_ARRAY_COUNT(Pattern);

		const FATR_EchoSearchState& S = State.Search;
		if (S.SearchStepIndex < 0 || S.SearchStepIndex >= N) return false;

		const FVector Fwd   = S.PrimaryDirection.GetSafeNormal2D();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
		const FVector2f P   = Pattern[S.SearchStepIndex];

		OutPoint = S.Origin + Fwd * (P.X * S.SearchRadius) + Right * (P.Y * S.SearchRadius);
		return true;
	}

	// Drive the lost-sight memory → projected → fan progression. Returns true and fills
	// OutIntent/OutMove while a search step is active; returns false (and zeroes confidence)
	// once the search is exhausted or times out, letting lower-priority drivers take over.
	bool AdvanceLostSightSearch(FATR_EchoRuntimeState& State, float Now, float ReachRadius,
	                            float ProjectionSeconds, float MaxProjectionDistance,
	                            EATR_EchoIntent& OutIntent, FATR_EchoMoveRequest& OutMove)
	{
		FATR_EchoAwarenessState& A = State.Awareness;
		FATR_EchoSearchState&    S = State.Search;

		// First, walk to the last-seen location before any directional search begins.
		if (!S.bSearchActive)
		{
			if (FVector::Dist(State.Location, A.LastSeenLocation) > ReachRadius)
			{
				OutIntent            = EATR_EchoIntent::ChaseLastSeenLocation;
				OutMove.Type         = EATR_EchoMoveTargetType::Location;
				OutMove.Location     = A.LastSeenLocation;
				OutMove.AcceptanceRadius = ReachRadius;
				return true;
			}
			BeginEchoSearch(State, Now, ProjectionSeconds, MaxProjectionDistance);
		}

		const bool bExpired = (Now - S.StartedTime) > S.MaxSearchDuration;

		FVector Point;
		bool bHaveStep = ComputeEchoSearchPoint(State, Point);

		// Advance to the next fan point once the current one is reached.
		if (!bExpired && bHaveStep && FVector::Dist(State.Location, Point) <= ReachRadius)
		{
			++S.SearchStepIndex;
			bHaveStep = ComputeEchoSearchPoint(State, Point);
		}

		if (bExpired || !bHaveStep)
		{
			S.bSearchActive = false;
			A.Confidence    = 0.f;                       // exhausted — fall through to idle/wander/horde
			A.Mode          = EATR_AwarenessMode::None;
			return false;
		}

		A.ProjectedSearchLocation = Point;
		OutIntent            = (S.SearchStepIndex == 0) ? EATR_EchoIntent::SearchProjectedDirection
		                                                : EATR_EchoIntent::FanSearchArea;
		OutMove.Type         = EATR_EchoMoveTargetType::Location;
		OutMove.Location     = Point;
		OutMove.AcceptanceRadius = ReachRadius;
		return true;
	}
}

void UATR_EchoSubsystem::RunIntentPass(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RunIntentPass);

	const UWorld* W = GetWorld();
	if (!W) return;
	const float Now = W->GetTimeSeconds();

	// Active (promoted) echoes drive the StateTree, so they need fresh intent every tick.
	// Lower-tier simulated echoes are folded into this pass in Phase 11.
	for (int32 Idx : PromotedIndices)
	{
		if (RuntimeStates.IsValidIndex(Idx))
			UpdateEchoIntent(Idx, Now, DeltaTime);
	}
}

void UATR_EchoSubsystem::UpdateEchoIntent(int32 Index, float Now, float DeltaTime)
{
	FATR_EchoRuntimeState* StatePtr = GetMutableEchoStateByIndex(Index);
	if (!StatePtr) return;
	FATR_EchoRuntimeState& State = *StatePtr;

	// Refresh live transform from the SoA / promoted actor so distance checks are accurate.
	State.Location = FVector(GetEchoQueryPosition(Index));
	State.Velocity = FVector(Velocities[Index]);
	const float YawRad = FMath::DegreesToRadians(Yaws[Index]);
	State.FacingDirection = FVector(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.f);

	FATR_EchoAwarenessState& A = State.Awareness;
	FATR_EchoSearchState&    S = State.Search;

	// --- Decays --- (confidence holds at full while we actually see the target)
	if (A.bHasCurrentLineOfSight && A.ConfirmedVisibleActor.IsValid())
		A.Confidence = 1.f;
	else
		A.Confidence = FMath::Max(0.f, A.Confidence - ConfidenceDecayPerSec * DeltaTime);

	A.Urgency       = FMath::Max(0.f, A.Urgency       - UrgencyDecayPerSec   * DeltaTime);
	State.Agitation = FMath::Max(0.f, State.Agitation - AgitationDecayPerSec * DeltaTime);

	// Sample the indirect horde-agitation field around this Echo. The effective agitation is
	// the stronger of its own (direct stimulus) and the surrounding field; the pressure
	// direction comes from the field only. This is the only horde coupling — no shared target.
	float   FieldAgitation = 0.f;
	FVector FieldDirection  = FVector::ZeroVector;
	SampleAgitationField(State.Location, FieldAgitation, FieldDirection);
	const float EffectiveAgitation = FMath::Max(State.Agitation, FieldAgitation);
	A.HordePressureDirection = FieldDirection;

	const EATR_EchoIntent OldIntent = State.Intent;
	EATR_EchoIntent      NewIntent = EATR_EchoIntent::Idle;
	FATR_EchoMoveRequest Move; // defaults to Type=None — no accidental origin move

	const bool bHeardRecent = (A.LastHeardTime >= 0.f) && (Now - A.LastHeardTime <= HeardMemorySeconds);
	const bool bSeeing      = A.bHasCurrentLineOfSight && A.ConfirmedVisibleActor.IsValid();

	// A fresh physical block takes priority over everything — we cannot make progress until it
	// is resolved. The placeholder fallback sidesteps around the obstacle; real break behavior
	// hangs off the same HandleObstacle intent later.
	const FATR_EchoObstacleIntent& O = State.Obstacle;
	const bool bObstacleFresh = O.bHasObstacle && (Now - O.LastObstacleTime) <= ObstacleHandleTimeoutSeconds;

	// Run the lost-sight search machine (unless blocked). It may set NewIntent/Move (active
	// search) or exhaust and zero confidence so lower-priority drivers take over this same tick.
	bool bSearchProducedIntent = false;
	if (!bObstacleFresh && !bSeeing && A.Confidence > LostSightMemoryThreshold)
		bSearchProducedIntent = AdvanceLostSightSearch(State, Now, ReachLocationRadius,
			SightProjectionSeconds, MaxSightProjectionDistance, NewIntent, Move);

	if (bObstacleFresh)
	{
		// Placeholder obstacle response: sidestep around the obstacle (per-Echo side choice)
		// while nudging forward. Real door/window/fence breaking attaches here later.
		const FVector ToObs = (O.ObstacleLocation - State.Location).GetSafeNormal2D();
		const FVector Side  = FVector::CrossProduct(FVector::UpVector, ToObs).GetSafeNormal();
		const float   Sign  = (EchoHash01(State.EchoId, 7) < 0.5f) ? 1.f : -1.f;

		NewIntent           = EATR_EchoIntent::HandleObstacle;
		Move.Type           = EATR_EchoMoveTargetType::Location;
		Move.Location       = State.Location + Side * (Sign * 300.f) + ToObs * 100.f;
		Move.AcceptanceRadius = ReachLocationRadius;
		A.Mode              = EATR_AwarenessMode::ObstacleBlocked;
	}
	else if (bSeeing)
	{
		// Confirmed visible → chase the actor itself.
		NewIntent           = EATR_EchoIntent::ChaseVisibleActor;
		Move.Type           = EATR_EchoMoveTargetType::Actor;
		Move.Actor          = A.ConfirmedVisibleActor;
		Move.AcceptanceRadius = ReachLocationRadius;
		S.bSearchActive     = false; // reacquired — abandon any search
	}
	else if (bSearchProducedIntent)
	{
		// NewIntent / Move already populated by AdvanceLostSightSearch.
	}
	else if (bHeardRecent)
	{
		// Heard a noise but never saw anything → investigate the LOCATION only.
		S.bSearchActive = false;
		if (A.Urgency >= HeardInvestigateUrgency)
		{
			NewIntent           = EATR_EchoIntent::InvestigateLocation;
			Move.Type           = EATR_EchoMoveTargetType::Location;
			Move.Location       = A.LastHeardLocation;
			Move.AcceptanceRadius = ReachLocationRadius;
		}
		else
		{
			NewIntent = EATR_EchoIntent::TurnTowardStimulus; // weak — orient only, no path move
		}
	}
	else if (EffectiveAgitation >= AgitationJoinThreshold && !A.HordePressureDirection.IsNearlyZero())
	{
		// Strong indirect pressure with a clear direction → migrate toward the hotspot.
		// Direction-only: this Echo never learns the seer's actual target.
		NewIntent           = EATR_EchoIntent::JoinHordePressure;
		Move.Type           = EATR_EchoMoveTargetType::Location;
		Move.Location       = State.Location + A.HordePressureDirection * 800.f;
		Move.AcceptanceRadius = ReachLocationRadius;
		A.Mode              = EATR_AwarenessMode::HordeAgitated;
	}
	else if (EffectiveAgitation >= AgitationJoinThreshold)
	{
		NewIntent = EATR_EchoIntent::TurnTowardStimulus; // agitated but no clear direction
		A.Mode    = EATR_AwarenessMode::HordeAgitated;
	}
	else if (EffectiveAgitation >= HordeCuriosityThreshold)
	{
		// Mild pressure → curious. Orient toward the hotspot but don't commit to migrating.
		NewIntent = EATR_EchoIntent::TurnTowardStimulus;
		A.Mode    = EATR_AwarenessMode::HordeAgitated;
	}
	else
	{
		// Nothing actionable — wind down to idle and forget.
		NewIntent       = EATR_EchoIntent::ReturnToIdle;
		A.Mode          = EATR_AwarenessMode::None;
		S.bSearchActive = false;
	}

	State.Intent           = NewIntent;
	State.Movement.Request = Move;
	State.LastUpdateTime   = Now;

	// Push compact intent to the promoted actor for client animation/FX (presentation only —
	// never AI memory). Replicates to clients on change.
	if (AATR_ActiveEcho* Actor = IndexToActor.IsValidIndex(Index) ? IndexToActor[Index] : nullptr)
		Actor->SetEchoIntentForPresentation(NewIntent);

	if (NewIntent != OldIntent)
	{
		UE_LOG(LogATR_EchoAI, VeryVerbose, TEXT("Intent EchoId %d: %d -> %d (conf %.2f urg %.2f agit %.2f)"),
			State.EchoId, static_cast<int32>(OldIntent), static_cast<int32>(NewIntent),
			A.Confidence, A.Urgency, State.Agitation);
	}
}

bool UATR_EchoSubsystem::PromoteToActive(int32 SoAIndex, AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor && SoAIndex >= 0 && SoAIndex < ActiveEntities)) return false;
	if (!ensureAlways(!IndexToActor[SoAIndex])) return false; // double-promote guard

	IndexToActor[SoAIndex]   = Actor;
	Actor->SourceIndex        = SoAIndex;
	PromotionTimes[SoAIndex] = GetWorld()->GetTimeSeconds();
	return true;
}

void UATR_EchoSubsystem::DemoteToHorde(AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor)) return;

	const int32 Idx = Actor->SourceIndex;
	if (!ensureAlways(Idx >= 0 && Idx < ActiveEntities)) return;

	Actor->WriteBackToSoA(this);  // flush Actor state → SoA before severing link

	const int32 NewCell = CoarseGrid.GetCellId(FVector2f(Positions[Idx].X, Positions[Idx].Y));
	MoveEntityCoarseCell(Idx, NewCell);

	IndexToActor[Idx]   = nullptr;
	PromotionTimes[Idx] = 0.f;
	Actor->SourceIndex  = INDEX_NONE;
}

AATR_ActiveEcho* UATR_EchoSubsystem::PromoteEcho(int32 SoAIndex)
{
	if (!ensureAlways(SoAIndex >= 0 && SoAIndex < ActiveEntities)) return nullptr;
	if (EchoPool.IsEmpty() || ControllerPool.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("UATR_EchoSubsystem::PromoteEcho — pool exhausted (SoAIndex=%d)"), SoAIndex);
		return nullptr;
	}

	AATR_ActiveEcho* Actor = EchoPool.Pop().Get();
	AATR_EchoAIController* Controller = ControllerPool.Pop().Get();

	if (!PromoteToActive(SoAIndex, Actor))
	{
		EchoPool.Add(Actor);
		ControllerPool.Add(Controller);
		return nullptr;
	}

	PromotedIndices.Add(SoAIndex);

	// Wire the canonical-state bridge and flip tier to Active before the controller wakes,
	// so OnPossess (and later-phase intent reads) can resolve this Echo's runtime state.
	RegisterActiveEcho(GetEchoIdForIndex(SoAIndex), Controller, Actor);

	Actor->InitFromSoA(this, SoAIndex);  // teleport to SoA position + seed velocity first
	Controller->Possess(Actor);          // OnPossess → AI wakes at correct world position
	return Actor;
}

void UATR_EchoSubsystem::DemoteEcho(AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor)) return;

	const int32 SoAIndex = Actor->SourceIndex;
	if (!ensureAlways(SoAIndex >= 0 && SoAIndex < ActiveEntities)) return;
	if (!ensureAlways(IndexToActor[SoAIndex] == Actor)) return;

	// Resolve the stable EchoId before DemoteToHorde clears SourceIndex.
	const int32 EchoId = GetEchoIdForIndex(SoAIndex);

	AATR_EchoAIController* Controller = Cast<AATR_EchoAIController>(Actor->GetController());

	// Sever the canonical-state bridge and drop tier back to LowDetail. Canonical
	// awareness/intent/search remain in RuntimeStates so memory survives demotion.
	UnregisterActiveEcho(EchoId);

	DemoteToHorde(Actor);  // WriteBackToSoA + coarse grid update + clear IndexToActor + SourceIndex

	const int32 PIIdx = PromotedIndices.IndexOfByKey(SoAIndex);
	if (ensureAlways(PIIdx != INDEX_NONE))
		PromotedIndices.RemoveAtSwap(PIIdx, 1, EAllowShrinking::No);

	if (Controller)
	{
		Controller->UnPossess(); // OnUnPossess → stops StateTree + perception
		ControllerPool.Add(Controller);
	}

	Actor->EnterPool(); // hide, disable collision, stop movement
	EchoPool.Add(Actor);
}

// ─── RunPromotionPass ─────────────────────────────────────────────────────────

void UATR_EchoSubsystem::RunPromotionPass()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RunPromotionPass);

	// Server-authoritative — clients never promote/demote locally.
	if (GetWorld()->GetNetMode() == NM_Client) return;
	if (ActiveEntities == 0) return;

	const float Now       = GetWorld()->GetTimeSeconds();
	const float DemRadSq  = DemoteRadius * DemoteRadius;

	// ── Promote ──────────────────────────────────────────────────────────────
	// Use the spatial grid (rebuilt this frame) to find candidates near each player.
	// Cost: O(grid_cells_in_PromoteRadius × Players) — far cheaper than O(ActiveEntities).

	TArray<int32> Candidates;
	Candidates.Reserve(64);

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || !PC->GetPawn()) continue;

		const FVector3f PP = FVector3f(PC->GetPawn()->GetActorLocation());
		Candidates.Reset();
		SpatialGrid.QueryRadius(PP, PromoteRadius,
			TArrayView<const FVector3f>(Positions.GetData(), ActiveEntities),
			Candidates);

		// Closest entities first — inner-ring (MustPromoteRadius) entities sort to top
		// naturally and consume pool slots before outer-ring ones.
		Candidates.Sort([&PP, this](int32 A, int32 B)
		{
			const FVector3f DA = GetEchoQueryPosition(A) - PP;
			const FVector3f DB = GetEchoQueryPosition(B) - PP;
			return (DA.X*DA.X + DA.Y*DA.Y) < (DB.X*DB.X + DB.Y*DB.Y);
		});

		// Sorted farthest-first list of promoted actors — built lazily when pool empties.
		// Lets us swap farthest actor out so a closer candidate can take its slot.
		TArray<TPair<float, int32>> FarPromoted;
		bool  bFarListBuilt = false;
		int32 FarListIdx    = 0;

		auto BuildFarList = [&]()
		{
			if (bFarListBuilt) return;
			bFarListBuilt = true;
			for (const int32 j : PromotedIndices)
			{
				if (!IndexToActor[j]) continue; // paranoia guard
				const FVector3f D = GetEchoQueryPosition(j) - PP;
				FarPromoted.Add({ D.X*D.X + D.Y*D.Y, j });
			}
			FarPromoted.Sort([](const TPair<float,int32>& A, const TPair<float,int32>& B)
			{
				return A.Key > B.Key; // farthest first
			});
		};

		for (const int32 i : Candidates)
		{
			if (IndexToActor[i]) continue; // already promoted

			if (EchoPool.IsEmpty() || ControllerPool.IsEmpty())
			{
				// Pool exhausted — try to evict the farthest promoted actor that is
				// farther from the player than this candidate. Bypasses MinTimeInTierSeconds
				// so closest echoes are always preferred; bBlockDemotion is still respected.
				BuildFarList();

				const FVector3f DC       = Positions[i] - PP;
				const float     CandDSq  = DC.X*DC.X + DC.Y*DC.Y;
				bool            bSwapped = false;

				while (FarListIdx < FarPromoted.Num())
				{
					const float     FarDSq  = FarPromoted[FarListIdx].Key;
					const int32     FarIdx  = FarPromoted[FarListIdx].Value;
					++FarListIdx;

					// Farthest promoted is now closer than this candidate — no beneficial swap possible.
					if (FarDSq <= CandDSq) break;

					AATR_ActiveEcho* FarActor = IndexToActor[FarIdx];
					if (!FarActor || FarActor->bBlockDemotion) continue; // already gone or locked

					DemoteEcho(FarActor);
					bSwapped = true;
					break;
				}

				if (!bSwapped || EchoPool.IsEmpty() || ControllerPool.IsEmpty()) break;
			}

			PromoteEcho(i); // stamps PromotionTimes[i] inside PromoteToActive
		}
	}

	// ── Demote ───────────────────────────────────────────────────────────────
	// Iterate PromotedIndices only — O(P ≤ PoolSize) instead of O(ActiveEntities).
	// Snapshot because DemoteEcho mutates PromotedIndices via RemoveAtSwap.

	TArray<int32> PromotedSnapshot = PromotedIndices;
	for (const int32 i : PromotedSnapshot)
	{
		AATR_ActiveEcho* Actor = IndexToActor[i];
		if (!Actor) continue; // safety: already demoted this frame

		// Hysteresis: demotion only after MinTimeInTierSeconds has elapsed
		if ((Now - PromotionTimes[i]) < MinTimeInTierSeconds) continue;

		// StateTree interruptibility guard
		if (Actor->bBlockDemotion) continue;

		// Urgency guard (Phase 11): don't yank an Echo that currently sees its target — keep
		// the full actor while it is actively chasing, even near the demotion ring.
		if (const FATR_EchoRuntimeState* RS = GetMutableEchoStateByIndex(i))
		{
			if (RS->Awareness.bHasCurrentLineOfSight)
				continue;
		}

		// Demotion requires entity to be beyond DemoteRadius from ALL players
		bool bAnyClose = false;
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (!PC || !PC->GetPawn()) continue;
			const FVector3f D = GetEchoQueryPosition(i) - FVector3f(PC->GetPawn()->GetActorLocation());
			if (D.X * D.X + D.Y * D.Y <= DemRadSq) { bAnyClose = true; break; }
		}
		if (bAnyClose) continue;

		DemoteEcho(Actor); // clears PromotionTimes[i] inside DemoteToHorde
	}
}

// ─── RunSteeringPass ──────────────────────────────────────────────────────────

void UATR_EchoSubsystem::RunSteeringPass()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RunSteeringPass);

	TArray<FVector3f> PlayerPositions;
	PlayerPositions.Reserve(8);

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || !PC->GetPawn()) continue;
		PlayerPositions.Add(FVector3f(PC->GetPawn()->GetActorLocation()));
	}

	if (PlayerPositions.IsEmpty()) return;

	const float MustPromoteSq = MustPromoteRadius * MustPromoteRadius;

	// Single pass over all local entities — each entity steers toward its nearest player within
	// MustPromoteRadius. Avoids last-player-wins when multiple players' zones overlap.
	ParallelFor(LocalEntityScratch.Num(), [this, &PlayerPositions, MustPromoteSq](int32 LocalIdx)
	{
		const int32 EntityIndex = LocalEntityScratch[LocalIdx];
		if (IndexToActor[EntityIndex]) return;

		float BestSq          = MustPromoteSq;
		int32 BestPlayerIndex = INDEX_NONE;

		for (int32 PlayerIndex = 0; PlayerIndex < PlayerPositions.Num(); ++PlayerIndex)
		{
			const FVector3f Delta  = PlayerPositions[PlayerIndex] - Positions[EntityIndex];
			const float     DistSq = Delta.X * Delta.X + Delta.Y * Delta.Y;
			if (DistSq <= BestSq) { BestSq = DistSq; BestPlayerIndex = PlayerIndex; }
		}

		if (BestPlayerIndex == INDEX_NONE)
		{
			// No player within MustPromoteRadius → low-detail horde migration (Phase 11).
			// Sample the indirect agitation field and drift toward pressure. This is how
			// far/non-active echoes react to gunshots, combat, and seeing-echoes without any
			// AIController/perception — cell-level pressure becomes population movement.
			float   FieldAgit = 0.f;
			FVector FieldDir   = FVector::ZeroVector;
			SampleAgitationField(FVector(Positions[EntityIndex]), FieldAgit, FieldDir);

			if (FieldAgit >= HordeCuriosityThreshold && !FieldDir.IsNearlyZero())
			{
				const FVector3f Dir = FVector3f(FieldDir.GetSafeNormal2D());
				Velocities[EntityIndex] = Dir * (HordeWalkSpeed * FMath::Clamp(FieldAgit, 0.f, 1.f));
				Yaws[EntityIndex]       = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
				MarkEchoDirty(EntityIndex, EEchoDirtyFlags::Transform);
			}
			return;
		}

		FVector3f Dir = PlayerPositions[BestPlayerIndex] - Positions[EntityIndex];
		Dir.Z = 0.f;

		if (BestSq > KINDA_SMALL_NUMBER)
		{
			Velocities[EntityIndex] = Dir * FMath::InvSqrt(BestSq) * HordeWalkSpeed;
			Yaws[EntityIndex]       = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
			MarkEchoDirty(EntityIndex, EEchoDirtyFlags::Transform);
		}
	});
}

// ─── GetEchoQueryPosition ─────────────────────────────────────────────────────

FVector3f UATR_EchoSubsystem::GetEchoQueryPosition(int32 Index) const
{
	if (!ensureAlways(Index >= 0 && Index < ActiveEntities))
		return FVector3f::ZeroVector;

	if (AATR_ActiveEcho* Actor = IndexToActor[Index])
		return FVector3f(Actor->GetActorLocation());

	return Positions[Index];
}

// ─── QueryEchoesByRelevancyBands ──────────────────────────────────────────────

// Queries the local fine grid only.
// Correct only when FarRange <= LocalZoneRadius.
// OutNear/Mid/Far are non-overlapping.
// Appends to caller arrays.
void UATR_EchoSubsystem::QueryEchoesByRelevancyBands(
	const FVector& Origin,
	float NearRange, float MidRange, float FarRange,
	TArray<int32>& OutNear, TArray<int32>& OutMid, TArray<int32>& OutFar) const
{
	if (!bGridReady || ActiveEntities == 0) return;

	ensureMsgf(FarRange <= LocalZoneRadius,
		TEXT("QueryEchoesByRelevancyBands: FarRange %.1f > LocalZoneRadius %.1f — results incomplete."),
		FarRange, LocalZoneRadius);

	const FVector3f Origin3f(Origin);
	const float NearSq = NearRange * NearRange;
	const float MidSq  = MidRange  * MidRange;

	// Single query at FarRange, then classify each result by XY distance band.
	TArray<int32> AllInRange;
	AllInRange.Reserve(64);
	SpatialGrid.QueryRadius(Origin3f, FarRange,
		TArrayView<const FVector3f>(Positions.GetData(), ActiveEntities),
		AllInRange);

	for (int32 Idx : AllInRange)
	{
		if (!ShouldProcessEchoForLocalHorde(Idx)) continue;
		if (IndexToActor[Idx]) continue; // promoted this frame — handled as real actor, not horde/ISM

		const FVector3f D   = Positions[Idx] - Origin3f;
		const float     DSq = D.X * D.X + D.Y * D.Y;

		if      (DSq <= NearSq) OutNear.Add(Idx);
		else if (DSq <= MidSq)  OutMid.Add(Idx);
		else                    OutFar.Add(Idx);
	}
}

// ─── Dirty State ─────────────────────────────────────────────────────────────

void UATR_EchoSubsystem::MarkEchoDirty(int32 Index, EEchoDirtyFlags Flags)
{
	if (!DirtyStates.IsValidIndex(Index)) return;
	DirtyStates[Index].Flags = DirtyStates[Index].Flags | Flags;
	++DirtyStates[Index].Version;
	if (DirtyStates[Index].Version == 0) DirtyStates[Index].Version = 1; // skip 0 — used as "never sent"
}

bool UATR_EchoSubsystem::IsEchoDirty(int32 Index, EEchoDirtyFlags Flags) const
{
	return DirtyStates.IsValidIndex(Index) &&
	       EnumHasAnyFlags(DirtyStates[Index].Flags, Flags);
}

// ─── ValidateEchoSpatialState ─────────────────────────────────────────────────

bool UATR_EchoSubsystem::ValidateEchoSpatialState() const
{
#if DO_CHECK
	bool bValid = true;

	for (int32 i = 0; i < ActiveEntities; ++i)
	{
		if (!ShouldProcessEchoForLocalHorde(i)) continue;

		if (CoarseCellIds[i] == INDEX_NONE)
		{
			UE_LOG(LogTemp, Error, TEXT("Echo %d has invalid CoarseCellId"), i);
			bValid = false;
		}
		if (CoarseSlotInCell[i] == INDEX_NONE)
		{
			UE_LOG(LogTemp, Error, TEXT("Echo %d has invalid CoarseSlotInCell"), i);
			bValid = false;
		}
		if (CoarseCellIds[i] != INDEX_NONE && CoarseSlotInCell[i] != INDEX_NONE &&
		    !CoarseGrid.ValidateEntitySlot(i, CoarseCellIds[i], CoarseSlotInCell[i]))
		{
			UE_LOG(LogTemp, Error,
				TEXT("Echo %d coarse-grid bucket mismatch. Cell=%d Slot=%d"),
				i, CoarseCellIds[i], CoarseSlotInCell[i]);
			bValid = false;
		}
		if (AATR_ActiveEcho* Actor = IndexToActor[i])
		{
			if (Actor->SourceIndex != i)
			{
				UE_LOG(LogTemp, Error,
					TEXT("Echo %d actor SourceIndex mismatch: %d"), i, Actor->SourceIndex);
				bValid = false;
			}
		}
	}

	for (int32 PromotedIndex : PromotedIndices)
	{
		if (!IndexToActor.IsValidIndex(PromotedIndex) || !IndexToActor[PromotedIndex])
		{
			UE_LOG(LogTemp, Error,
				TEXT("PromotedIndices contains invalid or unpromoted index: %d"), PromotedIndex);
			bValid = false;
		}
	}

	return bValid;
#else
	return true;
#endif
}


// ─── Client Relevancy State ───────────────────────────────────────────────────

bool UATR_EchoSubsystem::IsEchoClientRelevant(int32 Index) const
{
	if (Index < 0 || Index >= InitializeCount)
		return false;

	const UWorld* World = GetWorld();
	if (!World || World->GetNetMode() != NM_Client)
		return Index < ActiveEntities;

	return ClientRelevantEchoMask.IsValidIndex(Index) && ClientRelevantEchoMask[Index];
}

bool UATR_EchoSubsystem::ShouldProcessEchoForLocalHorde(int32 Index) const
{
	if (Index < 0 || Index >= ActiveEntities)
		return false;

	const UWorld* World = GetWorld();
	if (!World || World->GetNetMode() != NM_Client)
		return true;

	return ClientRelevantEchoMask.IsValidIndex(Index) && ClientRelevantEchoMask[Index];
}

void UATR_EchoSubsystem::MarkEchoClientRelevant(int32 Index)
{
	if (Index < 0 || Index >= InitializeCount)
		return;

	if (ClientRelevantEchoMask.Num() < InitializeCount)
		ClientRelevantEchoMask.Init(false, InitializeCount);

	if (ClientRelevantEchoSlots.Num() < InitializeCount)
		ClientRelevantEchoSlots.Init(INDEX_NONE, InitializeCount);

	if (ActiveEntities <= Index)
		ActiveEntities = Index + 1;

	const bool bWasRelevant = ClientRelevantEchoMask[Index];
	ClientRelevantEchoMask[Index] = true;

	if (!bWasRelevant)
	{
		ClientRelevantEchoSlots[Index] = ClientRelevantEchoIndices.Num();
		ClientRelevantEchoIndices.Add(Index);
	}
	else if (!ClientRelevantEchoIndices.IsValidIndex(ClientRelevantEchoSlots[Index]) ||
		ClientRelevantEchoIndices[ClientRelevantEchoSlots[Index]] != Index)
	{
		const int32 ExistingSlot = ClientRelevantEchoIndices.IndexOfByKey(Index);
		if (ExistingSlot != INDEX_NONE)
		{
			ClientRelevantEchoSlots[Index] = ExistingSlot;
		}
		else
		{
			ClientRelevantEchoSlots[Index] = ClientRelevantEchoIndices.Num();
			ClientRelevantEchoIndices.Add(Index);
		}
	}

	if (CoarseGrid.IsInitialized())
	{
		const int32 NewCell = CoarseGrid.GetCellId(FVector2f(Positions[Index].X, Positions[Index].Y));
		if (CoarseCellIds[Index] == INDEX_NONE)
		{
			RegisterEntityToCoarseGrid(Index);
		}
		else if (NewCell != CoarseCellIds[Index])
		{
			MoveEntityCoarseCell(Index, NewCell);
		}
	}
}

void UATR_EchoSubsystem::MarkEchoClientIrrelevant(int32 Index)
{
	TArray<int32> SingleIndex;
	SingleIndex.Reserve(1);
	SingleIndex.Add(Index);
	MarkEchoesClientIrrelevant(SingleIndex);
}

void UATR_EchoSubsystem::MarkEchoesClientIrrelevant(const TArray<int32>& Indices)
{
	if (Indices.IsEmpty())
		return;

	const UWorld* World = GetWorld();
	const bool bClientWorld = World && World->GetNetMode() == NM_Client;
	bool bRemovedAnyRelevantEcho = false;

	for (int32 Index : Indices)
	{
		if (Index < 0 || Index >= InitializeCount)
			continue;

		const bool bWasRelevant =
			ClientRelevantEchoMask.IsValidIndex(Index) && ClientRelevantEchoMask[Index];

		if (ClientRelevantEchoMask.IsValidIndex(Index))
			ClientRelevantEchoMask[Index] = false;

		if (ClientRelevantEchoSlots.IsValidIndex(Index))
		{
			const int32 Slot = ClientRelevantEchoSlots[Index];
			if (ClientRelevantEchoIndices.IsValidIndex(Slot) && ClientRelevantEchoIndices[Slot] == Index)
			{
				const int32 LastSlot = ClientRelevantEchoIndices.Num() - 1;
				const int32 MovedIndex = ClientRelevantEchoIndices[LastSlot];
				ClientRelevantEchoIndices.RemoveAtSwap(Slot, 1, EAllowShrinking::No);

				if (Slot != LastSlot && ClientRelevantEchoSlots.IsValidIndex(MovedIndex))
				{
					ClientRelevantEchoSlots[MovedIndex] = Slot;
				}
			}
			else if (bWasRelevant)
			{
				const int32 FoundSlot = ClientRelevantEchoIndices.IndexOfByKey(Index);
				if (FoundSlot != INDEX_NONE)
				{
					const int32 LastSlot = ClientRelevantEchoIndices.Num() - 1;
					const int32 MovedIndex = ClientRelevantEchoIndices[LastSlot];
					ClientRelevantEchoIndices.RemoveAtSwap(FoundSlot, 1, EAllowShrinking::No);

					if (FoundSlot != LastSlot && ClientRelevantEchoSlots.IsValidIndex(MovedIndex))
					{
						ClientRelevantEchoSlots[MovedIndex] = FoundSlot;
					}
				}
			}

			ClientRelevantEchoSlots[Index] = INDEX_NONE;
		}

		if (Index < ActiveEntities && CoarseCellIds.IsValidIndex(Index) && CoarseCellIds[Index] != INDEX_NONE)
			UnregisterEntityFromCoarseGrid(Index);

		if (Velocities.IsValidIndex(Index))     Velocities[Index]     = FVector3f::ZeroVector;
		if (Accelerations.IsValidIndex(Index)) Accelerations[Index] = FVector3f::ZeroVector;
		if (Forces.IsValidIndex(Index))        Forces[Index]        = FVector3f::ZeroVector;
		if (LocalVisitStamp.IsValidIndex(Index)) LocalVisitStamp[Index] = 0;

		LocalEntityScratch.RemoveAllSwap([Index](int32 Ei) { return Ei == Index; }, EAllowShrinking::No);
		LastLocalEntityScratch.RemoveAllSwap([Index](int32 Ei) { return Ei == Index; }, EAllowShrinking::No);

		bRemovedAnyRelevantEcho |= bWasRelevant;
	}

	// Keep ActiveEntities as the highest currently relevant client index + 1 so legacy
	// array range checks remain valid without forcing client scans over stale sparse rows.
	if (bClientWorld && bRemovedAnyRelevantEcho)
	{
		int32 HighestRelevant = INDEX_NONE;
		for (int32 RelevantIndex : ClientRelevantEchoIndices)
		{
			HighestRelevant = FMath::Max(HighestRelevant, RelevantIndex);
		}
		ActiveEntities = HighestRelevant + 1;
	}
}

void UATR_EchoSubsystem::ClearClientEchoRelevancy()
{
	const UWorld* World = GetWorld();
	if (World && World->GetNetMode() != NM_Client)
		return;

	for (int32 Index : ClientRelevantEchoIndices)
	{
		if (Index >= 0 && Index < ActiveEntities &&
			CoarseCellIds.IsValidIndex(Index) && CoarseCellIds[Index] != INDEX_NONE)
		{
			UnregisterEntityFromCoarseGrid(Index);
		}
	}

	ClientRelevantEchoMask.Init(false, InitializeCount);
	ClientRelevantEchoSlots.Init(INDEX_NONE, InitializeCount);
	ClientRelevantEchoIndices.Reset();
	LocalEntityScratch.Reset();
	LastLocalEntityScratch.Reset();
	SpatialGrid.Reset();
	bGridReady = false;
	ActiveEntities = 0;
}

// ─── ForceDestroyEcho ─────────────────────────────────────────────────────────

void UATR_EchoSubsystem::ForceDestroyEcho(int32 SoAIndex)
{
	if (SoAIndex < 0 || SoAIndex >= ActiveEntities) return;

	if (AATR_ActiveEcho* Actor = IndexToActor[SoAIndex])
	{
		DemoteEcho(Actor);
		if (!ensureAlways(IndexToActor[SoAIndex] == nullptr)) return;
	}

	RemoveEcho(SoAIndex);
}

void UATR_EchoSubsystem::ForceDestroyEcho(AATR_ActiveEcho* Actor)
{
	if (!Actor || Actor->SourceIndex == INDEX_NONE) return;

	const int32 Idx = Actor->SourceIndex;
	DemoteEcho(Actor);

	if (!ensureAlways(Idx >= 0 && Idx < ActiveEntities)) return;
	if (!ensureAlways(IndexToActor[Idx] == nullptr)) return;

	RemoveEcho(Idx);
}

// ─── Replication Scheduler ───────────────────────────────────────────────────

void UATR_EchoSubsystem::GatherReplicationClients()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_GatherReplicationClients);

	ReplicationClientsScratch.Reset();

	UWorld* World = GetWorld();
	if (!World) return;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC) continue;

		UATR_EchoReplicationComponent* Comp =
			PC->FindComponentByClass<UATR_EchoReplicationComponent>();

		if (!Comp)
		{
			Comp = NewObject<UATR_EchoReplicationComponent>(PC);
			Comp->RegisterComponent();
			UE_LOG(LogATR_EchoNet, Log,
				TEXT("Created EchoReplicationComponent for PlayerController %s"),
				*GetNameSafe(PC));
		}

		ReplicationClientsScratch.Add(Comp);
	}

	if (ReplicationClientCursor >= ReplicationClientsScratch.Num())
	{
		ReplicationClientCursor = 0;
	}
}

void UATR_EchoSubsystem::TickReplicationScheduler(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_Scheduler);

	GatherReplicationClients();

	const int32 NumClients = ReplicationClientsScratch.Num();
	if (NumClients == 0) return;

	const double Now = GetWorld()->GetTimeSeconds();

	int32 JobsProcessed = 0;
	int32 NearJobs = 0;
	int32 MidJobs  = 0;
	int32 FarJobs  = 0;

	const int32 StartCursor = ReplicationClientCursor;

	auto TryProcessBandForClient = [&](UATR_EchoReplicationComponent* Comp,
	                                    EEchoRelevancyBand Band) -> bool
	{
		if (!Comp) return false;
		if (!Comp->IsBandDue(Band, Now)) return false;
		if (JobsProcessed >= MaxReplicationJobsPerFrame) return false;

		if (Band == EEchoRelevancyBand::Near && NearJobs >= MaxNearReplicationJobsPerFrame) return false;
		if (Band == EEchoRelevancyBand::Mid  && MidJobs  >= MaxMidReplicationJobsPerFrame)  return false;
		if (Band == EEchoRelevancyBand::Far  && FarJobs  >= MaxFarReplicationJobsPerFrame)  return false;

		const int32 RemainingClientBudget = Comp->GetRemainingSnapshotBudget();
		if (RemainingClientBudget <= 0) return false;

		FVector ViewOrigin = FVector::ZeroVector;
		APlayerController* PC = Cast<APlayerController>(Comp->GetOwner());
		if (!PC) return false;

		FRotator ViewRot;
		PC->GetPlayerViewPoint(ViewOrigin, ViewRot);

		const int32 Sent = Comp->ServerReplicateBandBudgeted(
			this, Band, ViewOrigin, Now, RemainingClientBudget);

		if (Sent < 0) return false;

		++JobsProcessed;
		if      (Band == EEchoRelevancyBand::Near) ++NearJobs;
		else if (Band == EEchoRelevancyBand::Mid)  ++MidJobs;
		else                                        ++FarJobs;

		return true;
	};

	for (int32 Pass = 0; Pass < NumClients && JobsProcessed < MaxReplicationJobsPerFrame; ++Pass)
	{
		const int32 ClientIdx = (StartCursor + Pass) % NumClients;
		UATR_EchoReplicationComponent* Comp = ReplicationClientsScratch[ClientIdx];
		if (!Comp) continue;

		Comp->ResetFrameReplicationBudget();

		TryProcessBandForClient(Comp, EEchoRelevancyBand::Near);
		if (JobsProcessed >= MaxReplicationJobsPerFrame) break;

		TryProcessBandForClient(Comp, EEchoRelevancyBand::Mid);
		if (JobsProcessed >= MaxReplicationJobsPerFrame) break;

		TryProcessBandForClient(Comp, EEchoRelevancyBand::Far);
	}

	ReplicationClientCursor = (StartCursor + 1) % NumClients;
}
