// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoSubsystem.h"
#include "ATR_EchoManager.h"
#include "ATR_EchoReplicationComponent.h"
#include "ATR_ActiveEcho.h"
#include "AI/ATR_EchoAIController.h"
#include "AI/ATR_EchoAILog.h"
#include "ATR_EchoSettings.h"
#include "Data/ATR_EchoSearchPatternDataAsset.h"
#include "Data/ATR_EchoObstacleBehaviorDataAsset.h"
#include "../Health/ATR_HealthSettings.h"
#include "../Health/Data/ATR_WeaponDamageProfile.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
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

	// Canonical runtime state — parallel to the SoA arrays above.
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

	// Structural health rows — index-parallel to the SoA, allocated once.
	// Server writes via ApplyDamageToEcho; clients mirror via replicated deltas.
	GetMutableDefault<UATR_HealthSettings>()->ValidateAndClamp();
	HealthModel.Init(InitializeCount);

	ServerReplicationBudgetMs      = Settings->ServerReplicationBudgetMs;
	MaxReplicationJobsPerFrame     = Settings->MaxReplicationJobsPerFrame;
	MaxSnapshotsPerClientPerFrame  = Settings->MaxSnapshotsPerClientPerFrame;
	MaxNearReplicationJobsPerFrame = Settings->MaxNearReplicationJobsPerFrame;
	MaxMidReplicationJobsPerFrame  = Settings->MaxMidReplicationJobsPerFrame;
	MaxFarReplicationJobsPerFrame  = Settings->MaxFarReplicationJobsPerFrame;

	// Cache the settings CDO (program-lifetime object) so behavior code reads tuning directly.
	// Hot tuning values used in the existing intent/agitation paths are mirrored to members
	// below to keep their use sites unchanged.
	CachedSettings = Settings;

	// Awareness / intent decays + thresholds (mirrors of Echo|Awareness, Echo|Sight, Echo|Search).
	ConfidenceDecayPerSec        = Settings->ConfidenceDecayPerSecond;
	UrgencyDecayPerSec           = Settings->UrgencyDecayPerSecond;
	AgitationDecayPerSec         = Settings->EchoPersonalAgitationDecayPerSecond;
	LostSightMemoryThreshold     = Settings->LostSightMemoryThreshold;
	HeardInvestigateUrgency      = Settings->HeardInvestigateUrgency;
	HeardMemorySeconds           = Settings->HeardMemorySeconds;
	AgitationJoinThreshold       = Settings->AgitationJoinThreshold;
	ReachLocationRadius          = Settings->ReachLocationRadius;
	SightProjectionSeconds       = Settings->LastSeenProjectionSeconds;
	MaxSightProjectionDistance   = Settings->MaxLastSeenProjectionDistance;
	ObstacleHandleTimeoutSeconds = Settings->ObstacleHandleTimeoutSeconds;

	// Horde momentum field (mirrors of Echo|Agitation).
	MomentumCellSize          = Settings->MomentumCellSize;
	HordeCuriosityThreshold    = Settings->HordeCuriosityThreshold;

	// Field diffusion + jitter shaping (mirrors of Echo|Agitation).
	bEnableMomentumDiffusion   = Settings->bEnableMomentumDiffusion;
	MomentumDiffusionRate      = Settings->MomentumDiffusionRate;
	HordeDirectionJitterDegrees = Settings->HordeDirectionJitterDegrees;

	// Crowd shaping (mirrors of Echo|HordeShaping).
	bEnableHordeSeparation      = Settings->bEnableHordeSeparation;
	HordeSeparationRadius       = Settings->HordeSeparationRadius;
	HordeSeparationStrength     = Settings->HordeSeparationStrength;
	HordeApproachJitterDegrees  = Settings->HordeApproachJitterDegrees;
	HordeSeparationMaxNeighbors = Settings->HordeSeparationMaxNeighbors;

	// Detachment (mirrors of Echo|HordeShaping).
	bEnableHordeDetachment   = Settings->bEnableHordeDetachment;
	DetachEdgeNeighborCount  = Settings->DetachEdgeNeighborCount;
	DetachBackDot            = Settings->DetachBackDot;
	DetachChanceEdgePerSec   = Settings->DetachChanceEdgePerSec;
	DetachChanceBackPerSec   = Settings->DetachChanceBackPerSec;
	DetachChanceRandomPerSec = Settings->DetachChanceRandomPerSec;
	DetachDriftSpeed         = Settings->DetachDriftSpeed;

	// Movement-momentum model (mirrors of Echo|HordeMomentum).
	bEnableHordeMomentum        = Settings->bEnableHordeMomentum;
	MomentumBuildRate           = Settings->MomentumBuildRate;
	MomentumDecayPerSecond      = Settings->MomentumDecayPerSecond;
	MomentumPersistence         = Settings->MomentumPersistence;
	MomentumMoverSpeedThreshold = Settings->MomentumMoverSpeedThreshold;
	MomentumRefMoverCount       = Settings->MomentumRefMoverCount;
	MomentumAlignThreshold      = Settings->MomentumAlignThreshold;
	MomentumMaxStrength         = Settings->MomentumMaxStrength;
	SoundImpulseRadius          = Settings->SoundImpulseRadius;
	SoundImpulseSpeed           = Settings->SoundImpulseSpeed;
	SoundImpulseStrengthScale   = Settings->SoundImpulseStrengthScale;

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

	// Resolve default behavior DataAssets once. Null soft refs leave the built-in defaults active.
	ResolvedDefaultSearchPattern    = Settings->DefaultSearchPattern.LoadSynchronous();
	ResolvedDefaultObstacleBehavior = Settings->DefaultObstacleBehavior.LoadSynchronous();

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

	// Server and standalone own the Manager. Clients receive it via replication.
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
		BuildMomentumFromMovement(DeltaTime); // movement → momentum field
		DiffuseMomentumField(DeltaTime);     // spread momentum to neighbouring cells
		DecayMomentumField(DeltaTime);       // decay momentum (strong hordes persist longer)
		RunSteeringPass(DeltaTime);           // align to momentum + detachment
		RunPromotionPass();
		RunIntentPass(DeltaTime);

		// Lower-tier simulation — budgeted individual (LowDetail) + cell-level (Abstract). Both run
		// off accumulators at their configured Hz so cost stays bounded regardless of horde size.
		LowDetailAccumulator += DeltaTime;
		const float LowDetailInterval = 1.f / FMath::Max(0.1f, CachedSettings ? CachedSettings->LowDetailUpdateHz : 8.f);
		if (LowDetailAccumulator >= LowDetailInterval)
		{
			RunLowDetailPass(LowDetailAccumulator);
			LowDetailAccumulator = 0.f;
		}

		AbstractAccumulator += DeltaTime;
		const float AbstractInterval = 1.f / FMath::Max(0.1f, CachedSettings ? CachedSettings->AbstractUpdateHz : 1.f);
		if (AbstractAccumulator >= AbstractInterval)
		{
			RunAbstractPass(AbstractAccumulator);
			DecayAbstractCells(AbstractAccumulator);
			AbstractAccumulator = 0.f;
		}
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
		ReplicateEchoHealthDeltas(); // structural health deltas ride after snapshots
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

	// Defensive: rows are reset on removal (HandleSwapRemove), so a reused row
	// should already be the healthy baseline. Reset (and re-announce) if not.
	if (HealthModel.IsRowDamaged(Idx))
	{
		HealthModel.ResetEcho(Idx);
	}

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

	// Mirror the swap in the structural health rows BEFORE the SoA swap below
	// (the model handles its own row move, delta cleanup, and re-announce).
	// Server only — client mirrors are corrected by the emitted refresh deltas.
	HealthModel.HandleSwapRemove(Index, Last);

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

// ─── Canonical Runtime State ───────────────────────────────────────

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

// ─── Perception Fact Reporting ───────────────────────────────────────────────

void UATR_EchoSubsystem::ReportEchoSawActor(int32 EchoId, AActor* Actor, const FVector& Location, const FVector& ObservedVelocity, float TimeSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ReportSawActor);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State || !IsValid(Actor)) return;

	FATR_EchoAwarenessState& A = State->Awareness;

	// Observed velocity is sampled only while the target is actually visible (the controller
	// derives it from a visible position delta, not the actor's movement component). Smooth it
	// into stored velocity; snap on first observation / when prior is zero so we don't lerp from
	// a stale zero. This is the only place LastSeenVelocity is written.
	const float Alpha = CachedSettings ? CachedSettings->LastSeenVelocitySmoothingAlpha : 0.5f;
	const bool  bWasVisible = (A.ConfirmedVisibleActor.Get() == Actor) && !A.LastSeenVelocity.IsNearlyZero();
	A.LastSeenVelocity = bWasVisible
		? FMath::Lerp(A.LastSeenVelocity, ObservedVelocity, Alpha)
		: ObservedVelocity;

	A.Mode                  = EATR_AwarenessMode::SawTarget;
	A.ConfirmedVisibleActor = Actor;
	A.bHasCurrentLineOfSight = true;
	A.LastSeenLocation      = Location;
	A.LastSeenTime          = TimeSeconds;
	A.Confidence            = CachedSettings ? CachedSettings->ReacquireSightConfidence : 1.f;
	A.Urgency               = FMath::Max(A.Urgency, 1.f);

	// NOTE: sight no longer deposits any horde pressure. Seeing the player only makes THIS echo
	// chase (via intent). Its chasing movement still feeds the momentum field like any mover — so a
	// chaser can drag a horde along — but nothing here reveals the player's location to other echoes.

	State->LastUpdateTime = TimeSeconds;
}

void UATR_EchoSubsystem::ReportEchoLostSight(int32 EchoId, AActor* Actor, float TimeSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_ReportLostSight);

	FATR_EchoRuntimeState* State = GetMutableEchoState(EchoId);
	if (!State) return;

	FATR_EchoAwarenessState& A = State->Awareness;

	// Only the actor we were actually confirming should clear line-of-sight. A lost-sight
	// report for some other perceived actor must not wipe a live confirmation.
	const bool bWasConfirmedActor = (A.ConfirmedVisibleActor.Get() == Actor) || !A.ConfirmedVisibleActor.IsValid();
	if (!bWasConfirmedActor) return;

	// No-cheat boundary: clear current line of sight and transition into memory/search using
	// ONLY data already captured while the target was visible. LastSeenLocation / LastSeenVelocity
	// are deliberately NOT overwritten here — the subsystem already owns the last observed values.
	A.Mode                   = EATR_AwarenessMode::LostSightSearch;
	A.ConfirmedVisibleActor  = nullptr;
	A.bHasCurrentLineOfSight = false;
	A.LastSeenTime           = TimeSeconds;
	// Confidence/urgency intentionally preserved here — they decay in the intent update.

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
	const float UrgencyScale   = CachedSettings ? CachedSettings->NoiseStrengthToUrgencyScale   : 1.0f;
	const float AgitationScale  = CachedSettings ? CachedSettings->NoiseStrengthToAgitationScale : 0.25f;
	A.Urgency = FMath::Max(A.Urgency, FMath::Clamp(Loud * UrgencyScale, 0.f, 1.f));

	// Mild agitation contribution — even weak noise nudges horde pressure. Strong noise raises
	// urgency enough to investigate; weak noise mostly just agitates/orients.
	State->Agitation = FMath::Min(1.f, State->Agitation + Loud * AgitationScale);

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
			// Sound no longer deposits pressure here; the world-stimulus path kicks nearby echoes
			// into MOVEMENT toward the source, and that movement builds the momentum field.
			break;

		case EATR_StimulusType::Smell:
			State->Awareness.LastSmelledLocation = Event.Location;
			State->Awareness.LastSmelledTime     = Event.TimeSeconds;
			break;

		case EATR_StimulusType::Blood:
		case EATR_StimulusType::EchoAgitation:
			// Direct agitation contribution from blood/echo-agitation stimuli.
			State->Agitation = FMath::Min(State->Agitation + FMath::Max(Event.Strength, 0.f), 1.f);
			break;
	}

	State->LastUpdateTime = Event.TimeSeconds;
}

// ─── World-Level Stimulus API ────────────────────────────────────────────────

void UATR_EchoSubsystem::EmitWorldStimulus(const FATR_StimulusEvent& Event)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_EmitWorldStimulus);

	// Server-authoritative — clients never inject world stimuli into Echo awareness.
	const UWorld* W = GetWorld();
	if (!W || W->GetNetMode() == NM_Client) return;

	const float Now = W->GetTimeSeconds();

	// Validate / clamp. Strength/Radius are non-negative; missing time stamps to now.
	FATR_StimulusEvent E = Event;
	E.Strength = FMath::Max(0.f, E.Strength);
	E.Radius   = FMath::Max(0.f, E.Radius);
	if (E.TimeSeconds <= 0.f) E.TimeSeconds = Now;
	if (E.Strength <= 0.f) return;

	const float Radius = (E.Radius > 0.f)
		? E.Radius
		: (CachedSettings ? CachedSettings->LowDetailStimulusQueryRadius : 3000.f);
	const float HearRange = FMath::Max(1.f, Radius);

	// Agitation deposit amount by type (smell/blood drive agitation but not heard knowledge).
	float AgitAmount = E.Strength;
	if (CachedSettings)
	{
		switch (E.Type)
		{
			case EATR_StimulusType::Combat:
				AgitAmount = CachedSettings->CombatAgitationAmount; break;
			case EATR_StimulusType::Noise:
			case EATR_StimulusType::DoorImpact:
			case EATR_StimulusType::WindowImpact:
			case EATR_StimulusType::Scripted:
				AgitAmount = E.Strength * CachedSettings->NoiseAgitationAmountScale; break;
			default: break;
		}
	}

	// Feed Abstract cell pressure at the source so the far population can react without actors.
	{
		FATR_AbstractCell& Cell = AbstractCells.FindOrAdd(AbstractCellKey(E.Location));
		Cell.Agitation = FMath::Min(1.f, Cell.Agitation + AgitAmount);
		if (E.Type == EATR_StimulusType::Smell || E.Type == EATR_StimulusType::Blood)
			Cell.SmellMemory = FMath::Min(1.f, Cell.SmellMemory + E.Strength);
		else
			Cell.NoiseMemory = FMath::Min(1.f, Cell.NoiseMemory + E.Strength);
		Cell.PressureDirection += FVector2D(E.Direction.X, E.Direction.Y).GetSafeNormal() * AgitAmount;
		Cell.LastUpdatedTime = Now;
	}

	// Fan location-only awareness to nearby Echoes (active + low-detail) via the coarse grid so
	// cost is proportional to nearby population. Distance falloff scales strength. This raises
	// urgency/agitation on affected Echoes, which is what raises their promotion priority. No
	// branch ever records a target actor — only location/field knowledge.
	const float SmellAgitScale = CachedSettings ? CachedSettings->NoiseStrengthToAgitationScale : 0.25f;
	CoarseGrid.ForEachEntityInRadius(FVector2f(E.Location.X, E.Location.Y), HearRange,
		TArrayView<const FVector3f>(Positions.GetData(), ActiveEntities),
		[&](int32 Index)
		{
			FATR_EchoRuntimeState* State = GetMutableEchoStateByIndex(Index);
			if (!State) return;

			const float Dist    = FVector::Dist(FVector(GetEchoQueryPosition(Index)), E.Location);
			const float Falloff = FMath::Clamp(1.f - Dist / HearRange, 0.f, 1.f);
			if (Falloff <= 0.f) return;
			const float Strength = E.Strength * Falloff;

			switch (E.Type)
			{
				case EATR_StimulusType::Smell:
				case EATR_StimulusType::Blood:
					State->Awareness.LastSmelledLocation = E.Location;
					State->Awareness.LastSmelledTime     = E.TimeSeconds;
					State->Agitation = FMath::Min(1.f, State->Agitation + Strength * SmellAgitScale);
					break;
				default:
					// Location-only heard knowledge; never sets a target actor.
					ReportEchoHeardLocation(GetEchoIdForIndex(Index), E.Location, Strength, E.TimeSeconds);

					// Sound MOVEMENT impulse: start this horde-tier echo moving toward the source. That
					// shared movement is what builds the momentum field next tick (a loud sound that gets
					// 30 echoes moving together seeds a strong, persistent horde). Promoted echoes are
					// driven by their controller/StateTree, so skip them here.
					if (bEnableHordeMomentum && Dist <= SoundImpulseRadius
						&& IndexToActor.IsValidIndex(Index) && !IndexToActor[Index]
						&& Velocities.IsValidIndex(Index))
					{
						FVector ToSound = E.Location - FVector(GetEchoQueryPosition(Index));
						ToSound.Z = 0.f;
						if (!ToSound.IsNearlyZero())
						{
							const FVector Dir = ToSound.GetSafeNormal();
							const float   Spd = SoundImpulseSpeed
								* FMath::Clamp(Strength * SoundImpulseStrengthScale, 0.f, 1.f);
							Velocities[Index] = FVector3f((float)Dir.X * Spd, (float)Dir.Y * Spd, 0.f);
							Yaws[Index]       = FMath::RadiansToDegrees(FMath::Atan2((float)Dir.Y, (float)Dir.X));
							MarkEchoDirty(Index, EEchoDirtyFlags::Transform);
						}
					}
					break;
			}
		});
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
	// Tie this completion to the most recently issued request so a StateTree task waiting on that
	// serial resolves now. The controller's HandleMoveCompleted already rejects superseded
	// FAIRequestIDs, so this never stamps a result for a request that was replaced mid-flight.
	M.LastCompletedMoveRequestSerial = M.MoveRequestSerial;

	// Seed the obstacle hook for blocked/unreachable failures so HandleObstacle
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

// ─── Horde Momentum Field ──────────────────────────────────────────
// The MomentumField stores MOVEMENT MOMENTUM.
//   FATR_MomentumCell::Momentum = the cell's momentum vector (direction the local horde is moving,
//                                 magnitude = strength in [0, MomentumMaxStrength]).
//   FATR_MomentumCell::Strength = that momentum's magnitude (mirrored for thresholds/heat).
// Built from echoes actually moving (BuildMomentumFromMovement) — both horde-tier AND promoted
// actors — spreads via diffusion, decays (strong hordes persist longer), and is what nearby echoes
// align to. No-cheat invariant: nothing about the player's location enters the field, only movement.

void UATR_EchoSubsystem::BuildMomentumFromMovement(float DeltaTime)
{
	if (!bEnableHordeMomentum || MomentumBuildRate <= 0.f)
		return;

	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_BuildMomentumFromMovement);

	const float Now         = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const float SpeedThresh2 = MomentumMoverSpeedThreshold * MomentumMoverSpeedThreshold;

	// Aggregate each cell's mover directions: |average| = alignment coherence, Count = how many moved.
	struct FAccum { FVector2D Sum = FVector2D::ZeroVector; int32 Count = 0; };
	TMap<FIntPoint, FAccum> Accum;
	Accum.Reserve(LocalEntityScratch.Num() + PromotedIndices.Num());

	auto AddMover = [&](const FVector2D& Vel2D, const FVector& Pos)
	{
		const float S2 = Vel2D.X * Vel2D.X + Vel2D.Y * Vel2D.Y;
		if (S2 < SpeedThresh2) return;
		const float Inv = FMath::InvSqrt(S2);
		FAccum& Ac = Accum.FindOrAdd(MomentumCellKey(Pos));
		Ac.Sum += Vel2D * Inv;
		Ac.Count++;
	};

	// Horde-tier movers — SoA velocity (SimTick integrates these).
	for (const int32 Idx : LocalEntityScratch)
	{
		if (!Velocities.IsValidIndex(Idx) || !Positions.IsValidIndex(Idx)) continue;
		const FVector3f& V = Velocities[Idx];
		AddMover(FVector2D(V.X, V.Y), FVector(Positions[Idx]));
	}

	// Promoted (actor-driven) movers — 30 actors chasing the player IS momentum. SimTick does NOT
	// integrate SoA velocity for promoted echoes, so read each pawn's real velocity directly.
	for (const int32 Idx : PromotedIndices)
	{
		AATR_ActiveEcho* Actor = IndexToActor.IsValidIndex(Idx) ? IndexToActor[Idx] : nullptr;
		if (!Actor) continue;
		const FVector AV = Actor->GetVelocity();
		AddMover(FVector2D(AV.X, AV.Y), Actor->GetActorLocation());
	}

	const float Build = FMath::Clamp(MomentumBuildRate * DeltaTime, 0.f, 1.f);
	const float MaxS  = FMath::Max(0.1f, MomentumMaxStrength);
	const float RefN  = FMath::Max(1.f, MomentumRefMoverCount);

	for (const TPair<FIntPoint, FAccum>& P : Accum)
	{
		const FAccum& Ac = P.Value;
		if (Ac.Count <= 0) continue;

		const FVector2D Avg = Ac.Sum / (float)Ac.Count; // magnitude is the alignment coherence 0..1
		const float Coherence = Avg.Size();
		if (Coherence <= KINDA_SMALL_NUMBER) continue;

		// Strong momentum needs BOTH alignment AND enough movers — 30 moving together >> 3 wandering.
		const FVector2D DirN       = Avg / Coherence;
		const float     MoverScale = FMath::Min(1.f, (float)Ac.Count / RefN);
		const FVector2D Target     = DirN * (Coherence * MoverScale * MaxS);

		FATR_MomentumCell& Cell = MomentumField.FindOrAdd(P.Key);
		FVector2D Cur(Cell.Momentum.X, Cell.Momentum.Y);
		FVector2D New = Cur + (Target - Cur) * Build; // EMA toward this tick's movement
		const float Mag = New.Size();
		if (Mag > MaxS && Mag > KINDA_SMALL_NUMBER) New *= MaxS / Mag;

		Cell.Momentum = FVector(New.X, New.Y, 0.f);
		Cell.Strength = New.Size();
		Cell.LastUpdatedTime = Now;
	}
}

void UATR_EchoSubsystem::DiffuseMomentumField(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_DiffuseAgitationField);

	if (!bEnableMomentumDiffusion || MomentumDiffusionRate <= 0.f || MomentumField.IsEmpty())
		return;

	// Per-tick spread fraction. Scaled by DeltaTime for framerate independence and clamped well
	// below the explicit-diffusion stability limit. Each agitated cell donates this fraction of its
	// pressure to its 8 neighbours (mass-conserving), so a deposit smears into a smooth multi-cell
	// gradient over a second or two instead of staying a single hard-edged stamp.
	const float Spread = FMath::Clamp(MomentumDiffusionRate * DeltaTime, 0.f, 0.6f);
	if (Spread <= KINDA_SMALL_NUMBER) return;

	// 8-neighbour offsets — Moore neighbourhood is more isotropic than 4-neighbour, which helps
	// break the axis-aligned (grid-line) look.
	static const FIntPoint Neighbours[8] = {
		{ 1, 0}, {-1, 0}, {0, 1}, {0,-1}, {1, 1}, {1,-1}, {-1, 1}, {-1,-1}
	};

	// Gather net momentum-vector deltas first so the pass is order-independent.
	TMap<FIntPoint, FVector2D> Deltas;
	Deltas.Reserve(MomentumField.Num() * 2);

	for (const TPair<FIntPoint, FATR_MomentumCell>& Pair : MomentumField)
	{
		const FVector2D Mom(Pair.Value.Momentum.X, Pair.Value.Momentum.Y);
		const FVector2D Out = Mom * Spread;
		if (Out.IsNearlyZero()) continue;

		const FVector2D PerNbr = Out / 8.f;
		Deltas.FindOrAdd(Pair.Key) -= Out;
		for (const FIntPoint& N : Neighbours)
			Deltas.FindOrAdd(FIntPoint(Pair.Key.X + N.X, Pair.Key.Y + N.Y)) += PerNbr;
	}

	// Apply. Neighbours that did not exist are created here; fully-empty ones get pruned in decay.
	const float MaxS = FMath::Max(0.1f, MomentumMaxStrength);
	for (const TPair<FIntPoint, FVector2D>& D : Deltas)
	{
		FATR_MomentumCell& Cell = MomentumField.FindOrAdd(D.Key);
		FVector2D M(Cell.Momentum.X, Cell.Momentum.Y);
		M += D.Value;
		const float Mag = M.Size();
		if (Mag > MaxS && Mag > KINDA_SMALL_NUMBER) M *= MaxS / Mag;
		Cell.Momentum = FVector(M.X, M.Y, 0.f);
		Cell.Strength = M.Size();
	}
}

void UATR_EchoSubsystem::DecayMomentumField(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_DecayAgitationField);

	// Momentum decay: strong, coherent hordes resist decay (MomentumPersistence), so a big horde
	// takes much longer to peter out than a weak one.
	const float Base    = MomentumDecayPerSecond * DeltaTime;
	const float Persist = FMath::Clamp(MomentumPersistence, 0.f, 1.f);

	for (auto It = MomentumField.CreateIterator(); It; ++It)
	{
		FATR_MomentumCell& Cell = It.Value();
		FVector2D M(Cell.Momentum.X, Cell.Momentum.Y);
		const float Str = FMath::Clamp(M.Size(), 0.f, 1.f);
		const float Eff = FMath::Clamp(Base * (1.f - Persist * Str), 0.f, 1.f);

		M *= FMath::Max(0.f, 1.f - Eff);
		Cell.Momentum = FVector(M.X, M.Y, 0.f);
		Cell.Strength = M.Size();

		if (Cell.Strength <= KINDA_SMALL_NUMBER)
			It.RemoveCurrent(); // prune dead cells so the map stays proportional to active hordes
	}
}

void UATR_EchoSubsystem::SampleMomentumField(const FVector& Location, float& OutAgitation, FVector& OutDirection) const
{
	OutAgitation = 0.f;
	OutDirection = FVector::ZeroVector;

	if (MomentumField.IsEmpty()) return;

	const float CS = FMath::Max(1.f, MomentumCellSize);

	// Bilinearly interpolate the momentum VECTOR at an arbitrary world point (cell-centre space, so
	// it's continuous — no hard cell edges). Direction = where the local horde flows; magnitude =
	// strength. This is exactly the movement echoes align to, so the debug 'flow' matches reality.
	const double Cx = Location.X / CS - 0.5;
	const double Cy = Location.Y / CS - 0.5;
	const int32  X0 = FMath::FloorToInt(Cx);
	const int32  Y0 = FMath::FloorToInt(Cy);
	const float  Tx = (float)(Cx - X0);
	const float  Ty = (float)(Cy - Y0);

	auto MomAt = [this](int32 GX, int32 GY) -> FVector2D
	{
		const FATR_MomentumCell* C = MomentumField.Find(FIntPoint(GX, GY));
		return C ? FVector2D(C->Momentum.X, C->Momentum.Y) : FVector2D::ZeroVector;
	};

	const FVector2D Bottom = FMath::Lerp(MomAt(X0, Y0),   MomAt(X0+1, Y0),   Tx);
	const FVector2D Top    = FMath::Lerp(MomAt(X0, Y0+1), MomAt(X0+1, Y0+1), Tx);
	const FVector2D Mom    = FMath::Lerp(Bottom, Top, Ty);

	OutAgitation = Mom.Size();
	OutDirection = FVector(Mom.X, Mom.Y, 0.0).GetSafeNormal2D();
}

// ─── Intent Selection + Lost-Sight Search ────────────────────────────────────

namespace
{
	// Built-in standard directional fan: {forward, side} multipliers of the search radius.
	// Index 0 is the projected-direction probe. Used when no search-pattern DataAsset is set.
	static const FVector2D GBuiltInSearchPattern[] = {
		FVector2D(1.0,  0.0),
		FVector2D(0.7,  0.6),
		FVector2D(0.7, -0.6),
		FVector2D(1.4,  1.0),
		FVector2D(1.4, -1.0),
	};

	// Tuning + context threaded into the search functions so nothing is hardcoded and the
	// navigation system is reachable for point projection.
	struct FEchoSearchContext
	{
		const UWorld* World = nullptr;
		const TArray<FVector2D>* Offsets = nullptr; // null → built-in fan
		float ReachRadius            = 120.f;
		float ProjectionSeconds      = 2.f;
		float MaxProjectionDistance  = 800.f;
		float DefaultRadius          = 600.f;
		float MaxSearchDurationSeconds = 12.f;
		float RandomAngleDegrees     = 20.f;
		float NavProjectionRadius    = 500.f;

		int32 NumSteps() const { return Offsets ? Offsets->Num() : UE_ARRAY_COUNT(GBuiltInSearchPattern); }
		FVector2D Step(int32 i) const { return Offsets ? (*Offsets)[i] : GBuiltInSearchPattern[i]; }
	};

	// Deterministic per-Echo [0,1) hash. Stable across frames (keyed by EchoId), so each
	// Echo searches with its own consistent variation instead of identical robotic paths.
	float EchoHash01(int32 EchoId, uint32 Salt)
	{
		uint32 H = static_cast<uint32>(EchoId) * 2654435761u + Salt * 40503u;
		H ^= H >> 13; H *= 0x85ebca6bu; H ^= H >> 16;
		return static_cast<float>(H & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
	}

	// Project a raw world point onto the navmesh. Returns false if there is no navigation system
	// or no navmesh within the projection radius. Out is left equal to In on failure.
	bool ProjectSearchPointToNav(const UWorld* World, const FVector& In, float Radius, FVector& Out)
	{
		Out = In;
		if (!World) return false;
		const UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (!Nav) return false;

		FNavLocation Projected;
		if (Nav->ProjectPointToNavigation(In, Projected, FVector(Radius, Radius, Radius)))
		{
			Out = Projected.Location;
			return true;
		}
		return false;
	}

	// Begin a fresh lost-sight search anchored at the last-seen location, biased along the
	// target's last-known travel direction with per-Echo angular/radius/duration variation.
	// The search radius is derived from a CLAMPED velocity projection so prediction can never
	// be supernatural (ProjectionSeconds * speed, capped at MaxProjectionDistance, floored at
	// the configured default radius). All base values come from settings via the context.
	void BeginEchoSearch(FATR_EchoRuntimeState& State, float Now, const FEchoSearchContext& Ctx)
	{
		FATR_EchoAwarenessState& A = State.Awareness;
		FATR_EchoSearchState&    S = State.Search;

		FVector Dir = A.LastSeenVelocity.GetSafeNormal2D();
		if (Dir.IsNearlyZero()) Dir = State.FacingDirection.GetSafeNormal2D();
		if (Dir.IsNearlyZero()) Dir = FVector::ForwardVector;

		// Per-Echo angular jitter (± half the configured angle) so a group fans out.
		const float JitterDeg = (EchoHash01(State.EchoId, 1) - 0.5f) * Ctx.RandomAngleDegrees;
		Dir = Dir.RotateAngleAxis(JitterDeg, FVector::UpVector);

		const float Lead = FMath::Clamp(A.LastSeenVelocity.Size2D() * Ctx.ProjectionSeconds, 0.f, Ctx.MaxProjectionDistance);
		const float BaseRadius = FMath::Max(Lead, Ctx.DefaultRadius);

		S.bSearchActive    = true;
		S.Origin           = A.LastSeenLocation;
		S.PrimaryDirection = Dir;
		S.SearchStepIndex  = 0;
		S.StartedTime      = Now;
		// Aggressive echoes sweep wider and persist longer (per-Echo variation around the base).
		S.SearchRadius      = BaseRadius * (0.8f + 0.6f * EchoHash01(State.EchoId, 2) + 0.3f * State.Aggression);
		S.MaxSearchDuration = Ctx.MaxSearchDurationSeconds * (0.7f + 0.6f * EchoHash01(State.EchoId, 3) + 0.3f * State.Aggression);
	}

	// Raw (un-projected) world point for a given fan step. False if the step is out of range.
	bool ComputeRawSearchPoint(const FATR_EchoRuntimeState& State, const FEchoSearchContext& Ctx, int32 StepIndex, FVector& OutPoint)
	{
		if (StepIndex < 0 || StepIndex >= Ctx.NumSteps()) return false;

		const FATR_EchoSearchState& S = State.Search;
		const FVector Fwd   = S.PrimaryDirection.GetSafeNormal2D();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
		const FVector2D P   = Ctx.Step(StepIndex);

		OutPoint = S.Origin + Fwd * (P.X * S.SearchRadius) + Right * (P.Y * S.SearchRadius);
		return true;
	}

	// Drive the lost-sight memory → projected → fan progression. Every emitted move target is
	// nav-projected before becoming a move; steps that cannot project are skipped, and once no
	// projectable step remains (or the search times out) the search ends and confidence is zeroed
	// so lower-priority drivers take over. Returns true and fills OutIntent/OutMove while active.
	bool AdvanceLostSightSearch(FATR_EchoRuntimeState& State, float Now, const FEchoSearchContext& Ctx,
	                            EATR_EchoIntent& OutIntent, FATR_EchoMoveRequest& OutMove)
	{
		FATR_EchoAwarenessState& A = State.Awareness;
		FATR_EchoSearchState&    S = State.Search;

		auto EndSearch = [&]()
		{
			S.bSearchActive = false;
			A.Confidence    = 0.f;                       // exhausted — fall through to idle/wander/horde
			A.Mode          = EATR_AwarenessMode::None;
			A.ProjectedSearchLocation = FVector::ZeroVector;
			return false;
		};

		// First, walk to the last-seen location before any directional search begins. The last-seen
		// location was a real observed point; if it cannot project we still accept it as the goal.
		if (!S.bSearchActive)
		{
			if (FVector::Dist(State.Location, A.LastSeenLocation) > Ctx.ReachRadius)
			{
				FVector Goal;
				ProjectSearchPointToNav(Ctx.World, A.LastSeenLocation, Ctx.NavProjectionRadius, Goal);
				OutIntent            = EATR_EchoIntent::ChaseLastSeenLocation;
				OutMove.Type         = EATR_EchoMoveTargetType::Location;
				OutMove.Location     = Goal;
				OutMove.AcceptanceRadius = Ctx.ReachRadius;
				return true;
			}
			BeginEchoSearch(State, Now, Ctx);
		}

		if ((Now - S.StartedTime) > S.MaxSearchDuration)
			return EndSearch();

		// Advance past the current step once we've reached the point we were actually sent to.
		if (!A.ProjectedSearchLocation.IsZero() &&
			FVector::Dist(State.Location, A.ProjectedSearchLocation) <= Ctx.ReachRadius)
		{
			++S.SearchStepIndex;
		}

		// Find the next step whose point projects onto the navmesh; skip the rest.
		FVector Projected;
		bool bHave = false;
		while (S.SearchStepIndex < Ctx.NumSteps())
		{
			FVector Raw;
			if (ComputeRawSearchPoint(State, Ctx, S.SearchStepIndex, Raw) &&
				ProjectSearchPointToNav(Ctx.World, Raw, Ctx.NavProjectionRadius, Projected))
			{
				bHave = true;
				break;
			}
			++S.SearchStepIndex; // unprojectable — try the next fan point
		}

		if (!bHave)
			return EndSearch();

		A.ProjectedSearchLocation = Projected;
		OutIntent            = (S.SearchStepIndex == 0) ? EATR_EchoIntent::SearchProjectedDirection
		                                                : EATR_EchoIntent::FanSearchArea;
		OutMove.Type         = EATR_EchoMoveTargetType::Location;
		OutMove.Location     = Projected;
		OutMove.AcceptanceRadius = Ctx.ReachRadius;
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
	// Lower-tier simulated echoes are folded into this pass by the lower-tier simulation.
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
	SampleMomentumField(State.Location, FieldAgitation, FieldDirection);
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
	// All search tuning is settings-driven and every emitted point is nav-projected.
	bool bSearchProducedIntent = false;
	if (!bObstacleFresh && !bSeeing && A.Confidence > LostSightMemoryThreshold)
	{
		FEchoSearchContext Ctx;
		Ctx.World                 = GetWorld();
		Ctx.ReachRadius           = ReachLocationRadius;
		Ctx.ProjectionSeconds     = SightProjectionSeconds;
		Ctx.MaxProjectionDistance = MaxSightProjectionDistance;
		if (CachedSettings)
		{
			Ctx.DefaultRadius            = CachedSettings->SearchDefaultRadius;
			Ctx.MaxSearchDurationSeconds = CachedSettings->SearchMaxDurationSeconds;
			Ctx.RandomAngleDegrees       = CachedSettings->SearchRandomAngleDegrees;
			Ctx.NavProjectionRadius      = CachedSettings->SearchPointNavProjectionRadius;
		}
		if (ResolvedDefaultSearchPattern && ResolvedDefaultSearchPattern->SearchOffsets.Num() > 0)
			Ctx.Offsets = &ResolvedDefaultSearchPattern->SearchOffsets;

		bSearchProducedIntent = AdvanceLostSightSearch(State, Now, Ctx, NewIntent, Move);
	}

	if (bObstacleFresh)
	{
		// Obstacle handling currently requests a sidestep/repath (per-Echo side choice) while
		// nudging forward. Door/window/fence interactions attach through FATR_EchoObstacleIntent
		// and UATR_EchoObstacleBehaviorDataAsset without changing this intent/task seam. The
		// sidestep target is nav-projected so the fallback never paths off-mesh.
		const FVector ToObs = (O.ObstacleLocation - State.Location).GetSafeNormal2D();
		const FVector Side  = FVector::CrossProduct(FVector::UpVector, ToObs).GetSafeNormal();
		const float   Sign  = (EchoHash01(State.EchoId, 7) < 0.5f) ? 1.f : -1.f;

		const float Sidestep   = CachedSettings ? CachedSettings->ObstacleSidestepDistance     : 300.f;
		const float FwdNudge   = CachedSettings ? CachedSettings->ObstacleForwardNudgeDistance : 100.f;
		const float NavRadius  = CachedSettings ? CachedSettings->SearchPointNavProjectionRadius : 500.f;

		FVector Raw = State.Location + Side * (Sign * Sidestep) + ToObs * FwdNudge;
		FVector Projected;
		ProjectSearchPointToNav(GetWorld(), Raw, NavRadius, Projected); // Projected == Raw on failure

		NewIntent           = EATR_EchoIntent::HandleObstacle;
		Move.Type           = EATR_EchoMoveTargetType::Location;
		Move.Location       = Projected;
		Move.AcceptanceRadius = ReachLocationRadius;
		A.Mode              = EATR_AwarenessMode::ObstacleBlocked;
	}
	else if (bSeeing)
	{
		// Confirmed visible → chase the actor itself. Keep the Actor move target even when we flip to
		// Attack, so the echo keeps closing/pushing forward (the melee task layers grab/bite/pull on
		// top without stopping movement — momentum is preserved).
		NewIntent           = EATR_EchoIntent::ChaseVisibleActor;
		Move.Type           = EATR_EchoMoveTargetType::Actor;
		Move.Actor          = A.ConfirmedVisibleActor;
		Move.AcceptanceRadius = ReachLocationRadius;
		S.bSearchActive     = false; // reacquired — abandon any search

		// Within reach → engage melee. Structurally gated: an Echo that can
		// neither grab nor bite (no arms, no jaw) never enters Attack — it just
		// keeps pressing the chase. FAIL-OPEN: Caps == 0 means the row is
		// unknown/uninitialized, never a reason to disable attacking.
		const bool  bMelee   = !CachedSettings || CachedSettings->bEnableMeleeAttack;
		const float AtkRange = CachedSettings ? CachedSettings->MeleeAttackRange : 220.f;
		const uint16 Caps    = HealthModel.GetCapabilityFlags(Index);
		const bool bCanAttack = Caps == 0
			|| (Caps & (ATR_EchoCapability::CanAttackStanding |
			            ATR_EchoCapability::CanAttackCrawling)) != 0;
		if (bMelee && bCanAttack)
		{
			if (const AActor* Tgt = A.ConfirmedVisibleActor.Get())
			{
				if (FVector::Dist2D(State.Location, Tgt->GetActorLocation()) <= AtkRange)
					NewIntent = EATR_EchoIntent::Attack;
			}
		}
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
			NewIntent     = EATR_EchoIntent::TurnTowardStimulus; // weak — orient only, no path move
			Move.Location = A.LastHeardLocation;                 // orient target for the orient task
		}
	}
	else if (EffectiveAgitation >= AgitationJoinThreshold && !A.HordePressureDirection.IsNearlyZero())
	{
		// Strong indirect pressure with a clear direction → migrate toward the hotspot.
		// Direction-only: this Echo never learns the seer's actual target.
		const float PressureDist = CachedSettings ? CachedSettings->HordePressureMoveDistance : 800.f;
		NewIntent           = EATR_EchoIntent::JoinHordePressure;
		Move.Type           = EATR_EchoMoveTargetType::Location;
		Move.Location       = State.Location + A.HordePressureDirection * PressureDist;
		Move.AcceptanceRadius = ReachLocationRadius;
		A.Mode              = EATR_AwarenessMode::HordeAgitated;
	}
	else if (EffectiveAgitation >= AgitationJoinThreshold)
	{
		NewIntent     = EATR_EchoIntent::TurnTowardStimulus; // agitated but no clear direction
		Move.Location = State.Location + A.HordePressureDirection * 500.f;
		A.Mode        = EATR_AwarenessMode::HordeAgitated;
	}
	else if (EffectiveAgitation >= HordeCuriosityThreshold)
	{
		// Mild pressure → curious. Orient toward the hotspot but don't commit to migrating.
		NewIntent     = EATR_EchoIntent::TurnTowardStimulus;
		Move.Location = State.Location + A.HordePressureDirection * 500.f;
		A.Mode        = EATR_AwarenessMode::HordeAgitated;
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

// ─── Lower-Tier Simulation ───────────────────────────────────────────────────

void UATR_EchoSubsystem::RunLowDetailPass(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RunLowDetailPass);
	if (ActiveEntities <= 0) return;

	const int32 Num = LocalEntityScratch.Num();
	if (Num == 0) return;

	const float Now    = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const int32 Budget = CachedSettings ? CachedSettings->LowDetailMaxUpdatesPerTick : 256;

	// LowDetail simulates the non-active Echoes NEAR players (the local set rebuilt this frame).
	// Far Echoes are handled by the cell-level Abstract pass. Round-robin a budget window over the
	// local set so each is serviced within a few ticks; active (promoted) echoes are skipped — they
	// run the full intent pass + StateTree.
	int32 Processed = 0;
	int32 Scanned   = 0;
	while (Processed < Budget && Scanned < Num)
	{
		if (LowDetailCursor >= Num) LowDetailCursor = 0;
		const int32 Index = LocalEntityScratch[LowDetailCursor];
		++LowDetailCursor;
		++Scanned;

		if (IndexToActor.IsValidIndex(Index) && IndexToActor[Index]) continue;

		UpdateLowDetailEcho(Index, Now, DeltaTime);
		++Processed;
	}
}

void UATR_EchoSubsystem::UpdateLowDetailEcho(int32 Index, float Now, float DeltaTime)
{
	FATR_EchoRuntimeState* StatePtr = GetMutableEchoStateByIndex(Index);
	if (!StatePtr) return;
	FATR_EchoRuntimeState& State = *StatePtr;

	// Sync transform from the SoA row (no promoted actor at this tier).
	State.Location = FVector(Positions[Index]);
	State.Velocity = FVector(Velocities[Index]);

	// Use real elapsed time for this Echo so decays are correct regardless of round-robin cadence.
	const float Dt = (State.LastUpdateTime >= 0.f)
		? FMath::Clamp(Now - State.LastUpdateTime, 0.f, 1.f)
		: DeltaTime;

	FATR_EchoAwarenessState& A = State.Awareness;
	A.Confidence    = FMath::Max(0.f, A.Confidence    - ConfidenceDecayPerSec * Dt);
	A.Urgency       = FMath::Max(0.f, A.Urgency       - UrgencyDecayPerSec    * Dt);
	State.Agitation = FMath::Max(0.f, State.Agitation - AgitationDecayPerSec  * Dt);

	float   FieldAgit = 0.f;
	FVector FieldDir  = FVector::ZeroVector;
	SampleMomentumField(State.Location, FieldAgit, FieldDir);
	const float EffAgit = FMath::Max(State.Agitation, FieldAgit);
	A.HordePressureDirection = FieldDir;

	const float InvSpeed    = CachedSettings ? CachedSettings->LowDetailInvestigateSpeed : 150.f;
	const float SearchSpeed = CachedSettings ? CachedSettings->LowDetailSearchSpeed      : 120.f;
	const float WanderSpeed = CachedSettings ? CachedSettings->LowDetailWanderSpeed      : 60.f;

	FVector         DesiredDir = FVector::ZeroVector;
	float           Speed      = 0.f;
	EATR_EchoIntent NewIntent  = EATR_EchoIntent::Idle;

	// Continue a demoted/ongoing search via the shared nav-projected search machine — produces a
	// target location only; LowDetail steers toward it instead of issuing an active MoveTo.
	if (A.Confidence > LostSightMemoryThreshold)
	{
		FEchoSearchContext Ctx;
		Ctx.World                 = GetWorld();
		Ctx.ReachRadius           = ReachLocationRadius;
		Ctx.ProjectionSeconds     = SightProjectionSeconds;
		Ctx.MaxProjectionDistance = MaxSightProjectionDistance;
		if (CachedSettings)
		{
			Ctx.DefaultRadius            = CachedSettings->LowDetailSearchRadius;
			Ctx.MaxSearchDurationSeconds = CachedSettings->LowDetailSearchDurationSeconds;
			Ctx.RandomAngleDegrees       = CachedSettings->SearchRandomAngleDegrees;
			Ctx.NavProjectionRadius      = CachedSettings->SearchPointNavProjectionRadius;
		}
		if (ResolvedDefaultSearchPattern && ResolvedDefaultSearchPattern->SearchOffsets.Num() > 0)
			Ctx.Offsets = &ResolvedDefaultSearchPattern->SearchOffsets;

		EATR_EchoIntent      OutIntent = EATR_EchoIntent::Idle;
		FATR_EchoMoveRequest OutMove;
		if (AdvanceLostSightSearch(State, Now, Ctx, OutIntent, OutMove) && OutMove.Type == EATR_EchoMoveTargetType::Location)
		{
			DesiredDir = (OutMove.Location - State.Location).GetSafeNormal2D();
			Speed      = SearchSpeed;
			NewIntent  = OutIntent;
		}
	}

	const bool bHeardRecent = (A.LastHeardTime >= 0.f) && (Now - A.LastHeardTime <= HeardMemorySeconds);
	if (Speed <= 0.f && bHeardRecent && A.Urgency >= HeardInvestigateUrgency)
	{
		DesiredDir = (A.LastHeardLocation - State.Location).GetSafeNormal2D();
		Speed      = InvSpeed;
		NewIntent  = EATR_EchoIntent::InvestigateLocation;
		A.Mode     = EATR_AwarenessMode::HeardLocation;
	}
	if (Speed <= 0.f && EffAgit >= AgitationJoinThreshold && !FieldDir.IsNearlyZero())
	{
		DesiredDir = FieldDir.GetSafeNormal2D();
		Speed      = InvSpeed;
		NewIntent  = EATR_EchoIntent::JoinHordePressure;
		A.Mode     = EATR_AwarenessMode::HordeAgitated;
	}
	else if (Speed <= 0.f && EffAgit >= HordeCuriosityThreshold && !FieldDir.IsNearlyZero())
	{
		DesiredDir = FieldDir.GetSafeNormal2D();
		Speed      = WanderSpeed;
		NewIntent  = EATR_EchoIntent::Wander;
		A.Mode     = EATR_AwarenessMode::HordeAgitated;
	}

	State.Intent = NewIntent;

	// Only drive velocity when this Echo actually knows/feels something; otherwise leave it to the
	// steering/ambient pass so LowDetail is purely additive and never fights close-range steering.
	if (Speed > 0.f && !DesiredDir.IsNearlyZero())
	{
		const FVector3f Vel = FVector3f(DesiredDir * Speed);
		Velocities[Index] = Vel;
		Yaws[Index]       = FMath::RadiansToDegrees(FMath::Atan2(DesiredDir.Y, DesiredDir.X));
		MarkEchoDirty(Index, EEchoDirtyFlags::Transform);
	}

	State.LastUpdateTime = Now;
}

void UATR_EchoSubsystem::RunAbstractPass(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_RunAbstractPass);
	if (ActiveEntities <= 0) return;

	const float Now           = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const float MigrationRate = CachedSettings ? CachedSettings->AbstractCellMigrationRate : 0.05f;
	const float DriftSpeed    = CachedSettings ? CachedSettings->LowDetailWanderSpeed       : 60.f;

	// Recompute population each pass; pressure/memory persist and decay in DecayAbstractCells.
	for (auto& Pair : AbstractCells) Pair.Value.Population = 0;

	// Cheap full sweep at AbstractUpdateHz (≈1 Hz): aggregate population/pressure per cell and
	// migrate a rotating fraction of each agitated cell toward its pressure. Active echoes are
	// handled by the intent pass; everything else gets at least this cell-level reaction.
	const uint32 TimeSalt = static_cast<uint32>(Now);
	for (int32 Index = 0; Index < ActiveEntities; ++Index)
	{
		if (IndexToActor.IsValidIndex(Index) && IndexToActor[Index]) continue; // active tier

		// Skip Echoes in the local set — those are simulated individually by the LowDetail pass.
		// The Abstract tier owns only the far population.
		if (LocalVisitStamp.IsValidIndex(Index) && LocalVisitStamp[Index] == LocalVisitEpoch) continue;

		const FVector Pos = FVector(Positions[Index]);
		FATR_AbstractCell& Cell = AbstractCells.FindOrAdd(AbstractCellKey(Pos));
		++Cell.Population;

		float   FieldAgit = 0.f;
		FVector FieldDir  = FVector::ZeroVector;
		SampleMomentumField(Pos, FieldAgit, FieldDir);
		if (FieldAgit > Cell.Agitation) Cell.Agitation = FieldAgit;
		if (!FieldDir.IsNearlyZero())   Cell.PressureDirection = FVector2D(FieldDir.X, FieldDir.Y);
		Cell.LastUpdatedTime = Now;

		// Migrate ~MigrationRate of the cell toward pressure. Selection rotates over time (salt) so
		// it isn't always the same Echoes, yet stays RNG-free for replay stability.
		if (Cell.Agitation >= HordeCuriosityThreshold && !FieldDir.IsNearlyZero())
		{
			if (EchoHash01(GetEchoIdForIndex(Index), TimeSalt) < MigrationRate)
			{
				const FVector3f Dir = FVector3f(FieldDir.GetSafeNormal2D());
				Velocities[Index] = Dir * DriftSpeed;
				Yaws[Index]       = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
				MarkEchoDirty(Index, EEchoDirtyFlags::Transform);
			}
		}
	}
}

void UATR_EchoSubsystem::DecayAbstractCells(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Echo_DecayAbstractCells);

	const float Decay = (CachedSettings ? CachedSettings->AgitationFieldDecayPerSecond : 0.25f) * DeltaTime;

	for (auto It = AbstractCells.CreateIterator(); It; ++It)
	{
		FATR_AbstractCell& C = It.Value();
		C.Agitation         = FMath::Max(0.f, C.Agitation   - Decay);
		C.NoiseMemory       = FMath::Max(0.f, C.NoiseMemory - Decay);
		C.SmellMemory       = FMath::Max(0.f, C.SmellMemory - Decay);
		C.PressureDirection *= FMath::Max(0.f, 1.f - Decay);

		// Prune fully-decayed, empty cells so the map stays proportional to active hotspots.
		if (C.Population == 0 && C.Agitation <= KINDA_SMALL_NUMBER
			&& C.NoiseMemory <= KINDA_SMALL_NUMBER && C.SmellMemory <= KINDA_SMALL_NUMBER)
		{
			It.RemoveCurrent();
		}
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

	// Stamp the demotion time for the recently-demoted promotion penalty. Awareness/search state
	// is intentionally left intact so the demoted Echo continues its search at the LowDetail tier.
	if (FATR_EchoRuntimeState* State = GetMutableEchoStateByIndex(Idx))
	{
		State->LastDemotedTime = GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f;
		State->Tier            = EATR_EchoSimulationTier::LowDetail;
	}

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

// ─── Promotion scoring + demotion guards ─────────────────────────────────────

float UATR_EchoSubsystem::ComputePromotionScore(int32 Index, const FVector3f& PlayerPos, const FVector3f& PlayerForward, float Now) const
{
	const FVector3f P    = GetEchoQueryPosition(Index);
	const FVector3f D    = P - PlayerPos;
	const float     Dist = FMath::Sqrt(D.X * D.X + D.Y * D.Y);

	// Distance priority: closer = higher (less negative). Bonuses add on top.
	float Score = -Dist;

	const UATR_EchoSettings* S = CachedSettings;
	if (!S) return Score;

	if (Dist <= MustPromoteRadius) Score += S->MustPromoteScoreBonus;

	if (RuntimeStates.IsValidIndex(Index))
	{
		const FATR_EchoRuntimeState& RS = RuntimeStates[Index];
		Score += RS.Awareness.Urgency    * S->PromotionUrgencyBoost;
		Score += RS.Awareness.Confidence * S->PromotionConfidenceBoost;
		Score += FMath::Max(0.f, RS.Agitation) * S->PromotionAgitationBoost;

		if (RS.LastDemotedTime >= 0.f && S->RecentlyDemotedSeconds > 0.f)
		{
			const float Since = Now - RS.LastDemotedTime;
			if (Since >= 0.f && Since < S->RecentlyDemotedSeconds)
				Score -= S->RecentlyDemotedPenalty * (1.f - Since / S->RecentlyDemotedSeconds);
		}
	}

	// Player-facing relevance: an Echo in the player's forward hemisphere is worth more.
	if (!PlayerForward.IsNearlyZero() && Dist > KINDA_SMALL_NUMBER)
	{
		const FVector3f Dir    = D / Dist;
		const float     Facing = FVector3f::DotProduct(PlayerForward.GetSafeNormal(), Dir);
		if (Facing > 0.f) Score += Facing * S->PromotionPlayerFacingBoost;
	}

	return Score;
}

bool UATR_EchoSubsystem::IsDemotionBlocked(int32 Index, float Now) const
{
	const FATR_EchoRuntimeState* RS = RuntimeStates.IsValidIndex(Index) ? &RuntimeStates[Index] : nullptr;
	if (!RS) return false;

	const UATR_EchoSettings* S = CachedSettings;

	// Visible chase — keep the full actor while it actually sees its target.
	if ((!S || S->bBlockDemotionDuringVisibleChase) &&
		RS->Awareness.bHasCurrentLineOfSight && RS->Awareness.ConfirmedVisibleActor.IsValid())
		return true;

	if (S)
	{
		if (RS->Awareness.Confidence >= S->DemotionConfidenceBlockThreshold) return true;
		if (RS->Awareness.Urgency    >= S->DemotionUrgencyBlockThreshold)    return true;

		// Fresh lost-sight search (chase-last-seen / projected / fan).
		if (S->bBlockDemotionDuringFreshSearch)
		{
			const bool bSearchIntent =
				RS->Intent == EATR_EchoIntent::ChaseLastSeenLocation    ||
				RS->Intent == EATR_EchoIntent::SearchProjectedDirection ||
				RS->Intent == EATR_EchoIntent::FanSearchArea;
			if (bSearchIntent && RS->Search.bSearchActive &&
				(Now - RS->Search.StartedTime) <= S->DemotionSearchBlockSeconds)
				return true;
		}

		// Fresh obstacle handling.
		if (S->bBlockDemotionDuringObstacleHandling &&
			RS->Intent == EATR_EchoIntent::HandleObstacle &&
			RS->Obstacle.bHasObstacle &&
			(Now - RS->Obstacle.LastObstacleTime) <= ObstacleHandleTimeoutSeconds)
			return true;
	}

	return false;
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

		const FVector3f PP  = FVector3f(PC->GetPawn()->GetActorLocation());
		const FVector3f PFwd = FVector3f(PC->GetPawn()->GetActorForwardVector());
		Candidates.Reset();
		SpatialGrid.QueryRadius(PP, PromoteRadius,
			TArrayView<const FVector3f>(Positions.GetData(), ActiveEntities),
			Candidates);

		// Highest promotion score first — score blends distance with urgency/confidence/agitation/
		// facing and the must-promote bonus, so an engaged Echo just outside the nearest ring can
		// still outrank a closer but idle one. Inner-ring echoes still sort to the top via the bonus.
		Candidates.Sort([&PP, &PFwd, Now, this](int32 A, int32 B)
		{
			return ComputePromotionScore(A, PP, PFwd, Now) > ComputePromotionScore(B, PP, PFwd, Now);
		});

		// Lowest-score-first list of currently promoted actors — built lazily when the pool empties.
		// Lets a higher-scoring candidate evict the weakest promoted Echo (not merely the farthest).
		TArray<TPair<float, int32>> WeakPromoted;
		bool  bWeakListBuilt = false;
		int32 WeakListIdx    = 0;

		auto BuildWeakList = [&]()
		{
			if (bWeakListBuilt) return;
			bWeakListBuilt = true;
			for (const int32 j : PromotedIndices)
			{
				if (!IndexToActor[j]) continue; // paranoia guard
				WeakPromoted.Add({ ComputePromotionScore(j, PP, PFwd, Now), j });
			}
			WeakPromoted.Sort([](const TPair<float,int32>& A, const TPair<float,int32>& B)
			{
				return A.Key < B.Key; // weakest (lowest score) first
			});
		};

		for (const int32 i : Candidates)
		{
			if (IndexToActor[i]) continue; // already promoted

			if (EchoPool.IsEmpty() || ControllerPool.IsEmpty())
			{
				// Pool exhausted — evict the weakest promoted Echo if this candidate outscores it.
				// Respects engagement guards (IsDemotionBlocked / bBlockDemotion); the hard pool cap
				// is never exceeded, so the fixed population still caps simultaneous full actors.
				BuildWeakList();

				const float CandScore = ComputePromotionScore(i, PP, PFwd, Now);
				bool        bSwapped  = false;

				while (WeakListIdx < WeakPromoted.Num())
				{
					const float Weakest    = WeakPromoted[WeakListIdx].Key;
					const int32 WeakIdx    = WeakPromoted[WeakListIdx].Value;
					++WeakListIdx;

					// Weakest promoted now outscores this candidate — no beneficial swap remains.
					if (Weakest >= CandScore) break;

					AATR_ActiveEcho* WeakActor = IndexToActor[WeakIdx];
					if (!WeakActor || WeakActor->bBlockDemotion) continue; // gone or locked
					if (IsDemotionBlocked(WeakIdx, Now)) continue;          // actively engaged

					DemoteEcho(WeakActor);
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

		// Engagement guards: keep the full actor while it sees its target, has high confidence/
		// urgency, is in a fresh search, or is handling a fresh obstacle (all settings-driven).
		if (IsDemotionBlocked(i, Now)) continue;

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

namespace
{
	// Rotate a 2D vector by an angle (radians).
	FORCEINLINE FVector2f ATR_RotateVec2(const FVector2f& V, float AngleRad)
	{
		const float C = FMath::Cos(AngleRad), S = FMath::Sin(AngleRad);
		return FVector2f(V.X * C - V.Y * S, V.X * S + V.Y * C);
	}
}

void UATR_EchoSubsystem::RunSteeringPass(float DeltaTime)
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
	const float Now           = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// Crowd-shaping params, read-only inside the parallel body.
	const bool  bSep        = bEnableHordeSeparation && HordeSeparationRadius > 1.f && SpatialGrid.IsInitialized();
	const float SepR        = HordeSeparationRadius;
	const float SepR2       = SepR * SepR;
	const float SepStrength = HordeSeparationStrength;
	const int32 SepMaxN     = FMath::Max(1, HordeSeparationMaxNeighbors);
	const float ApproachJit = FMath::DegreesToRadians(HordeApproachJitterDegrees);
	const float FlowJit     = FMath::DegreesToRadians(HordeDirectionJitterDegrees);

	// Detachment params.
	const bool   bDetach     = bEnableHordeDetachment;
	const int32  EdgeN       = DetachEdgeNeighborCount;
	const float  BackDot     = DetachBackDot;
	const float  PEdge       = DetachChanceEdgePerSec;
	const float  PBack       = DetachChanceBackPerSec;
	const float  PRandom     = DetachChanceRandomPerSec;
	const float  DriftSpeed  = DetachDriftSpeed;
	const float  AlignThresh = MomentumAlignThreshold;
	const uint32 DetachSalt  = (uint32)(Now * 1000.0);

	// Single pass over all local entities — each aligns to local horde momentum (with separation,
	// jitter, and edge/back/random detachment), or seeks a very-near player.
	ParallelFor(LocalEntityScratch.Num(), [&](int32 LocalIdx)
	{
		const int32 EntityIndex = LocalEntityScratch[LocalIdx];
		if (IndexToActor[EntityIndex]) return;

		const FVector3f Pi = Positions[EntityIndex];

		// Short-range separation + neighbour stats (count + centroid) for detachment.
		// Allocation-free — walks the fine-grid cells overlapping the separation box.
		FVector2f Sep(0.f, 0.f);
		FVector2f NbrSum(0.f, 0.f);
		int32     Count = 0;
		if (bSep)
		{
			const FVector2D Mn(Pi.X - SepR, Pi.Y - SepR);
			const FVector2D Mx(Pi.X + SepR, Pi.Y + SepR);
			SpatialGrid.ForEachInBounds(Mn, Mx, [&](int32 j)
			{
				if (j == EntityIndex || Count >= SepMaxN) return;
				const FVector3f& Pj = Positions[j];
				const float dx = Pi.X - Pj.X, dy = Pi.Y - Pj.Y;
				const float d2 = dx * dx + dy * dy;
				if (d2 > KINDA_SMALL_NUMBER && d2 < SepR2)
				{
					const float inv     = FMath::InvSqrt(d2);
					const float falloff = 1.f - (d2 * inv) / SepR; // 1 - dist/R
					Sep.X += dx * inv * falloff;
					Sep.Y += dy * inv * falloff;
					NbrSum.X += Pj.X; NbrSum.Y += Pj.Y;
					++Count;
				}
			});
		}

		const int32 EchoId = GetEchoIdForIndex(EntityIndex);

		float BestSq          = MustPromoteSq;
		int32 BestPlayerIndex = INDEX_NONE;
		for (int32 PlayerIndex = 0; PlayerIndex < PlayerPositions.Num(); ++PlayerIndex)
		{
			const FVector3f Delta  = PlayerPositions[PlayerIndex] - Pi;
			const float     DistSq = Delta.X * Delta.X + Delta.Y * Delta.Y;
			if (DistSq <= BestSq) { BestSq = DistSq; BestPlayerIndex = PlayerIndex; }
		}

		if (BestPlayerIndex == INDEX_NONE)
		{
			// No very-near player → align to local horde MOMENTUM (what others are doing).
			float   Strength = 0.f;
			FVector MomDir   = FVector::ZeroVector;
			SampleMomentumField(FVector(Pi), Strength, MomDir);

			// Weak local momentum → no horde to follow (this is how a horde finally disperses).
			const bool bInHorde = (Strength >= AlignThresh && !MomDir.IsNearlyZero());

			if (bInHorde && bDetach)
			{
				// Detachment: edge (few neighbours) + back (behind the crowd along momentum) + a
				// constant random trickle. If it fires, the echo peels OUTWARD instead of aligning.
				const bool bEdge = (Count < EdgeN);
				bool bBack = false;
				if (Count > 0)
				{
					const FVector2f Cen(NbrSum.X / Count, NbrSum.Y / Count);
					FVector2f ToCen(Cen.X - Pi.X, Cen.Y - Pi.Y);
					if (!ToCen.IsNearlyZero())
					{
						ToCen.Normalize();
						const FVector2f MomN = FVector2f((float)MomDir.X, (float)MomDir.Y).GetSafeNormal();
						bBack = FVector2f::DotProduct(MomN, ToCen) > BackDot; // crowd is ahead of me
					}
				}

				float P = PRandom + (bEdge ? PEdge : 0.f) + (bBack ? PBack : 0.f);
				if (P > 0.f && EchoHash01(EchoId, DetachSalt) < P * DeltaTime)
				{
					// Peel off: drift away from the crowd centre (or just use separation), no align.
					FVector2f Out = Sep;
					if (Count > 0)
					{
						const FVector2f Cen(NbrSum.X / Count, NbrSum.Y / Count);
						FVector2f Away(Pi.X - Cen.X, Pi.Y - Cen.Y);
						if (!Away.IsNearlyZero()) { Away.Normalize(); Out += Away; }
					}
					if (Out.IsNearlyZero())
					{
						// Dead-zone fix: no neighbours to push away from. Peel BACKWARD against the
						// momentum (leave the flow), with a per-echo jitter; if there's no momentum
						// direction either, pick a deterministic per-echo random heading.
						FVector2f Back(-(float)MomDir.X, -(float)MomDir.Y);
						if (Back.IsNearlyZero())
						{
							const float Ang = EchoHash01(EchoId, 0xDEADu) * 2.f * PI;
							Back = FVector2f(FMath::Cos(Ang), FMath::Sin(Ang));
						}
						else
						{
							Back.Normalize();
							Back = ATR_RotateVec2(Back, (EchoHash01(EchoId, 0xBEEFu) * 2.f - 1.f) * FlowJit);
						}
						Out = Back;
					}
					Out.Normalize();
					Velocities[EntityIndex] = FVector3f(Out.X, Out.Y, 0.f) * DriftSpeed;
					Yaws[EntityIndex]       = FMath::RadiansToDegrees(FMath::Atan2(Out.Y, Out.X));
					MarkEchoDirty(EntityIndex, EEchoDirtyFlags::Transform);
					return;
				}
			}

			if (!bInHorde && Sep.IsNearlyZero()) return; // genuinely idle → leave velocity as-is

			FVector2f Steer(0.f, 0.f);
			float Speed = 0.f;
			if (bInHorde)
			{
				FVector2f Flow((float)MomDir.X, (float)MomDir.Y);
				Flow.Normalize();
				if (FlowJit > 0.f)
					Flow = ATR_RotateVec2(Flow, (EchoHash01(EchoId, 0xF10Du) * 2.f - 1.f) * FlowJit);
				Steer += Flow;
				Speed = HordeWalkSpeed * FMath::Clamp(Strength, 0.f, 1.f);
			}
			Steer += Sep * SepStrength;
			if (Steer.IsNearlyZero()) return;
			Steer.Normalize();
			if (Speed <= 0.f) Speed = HordeWalkSpeed * 0.35f; // gentle de-clumping when only separating

			Velocities[EntityIndex] = FVector3f(Steer.X, Steer.Y, 0.f) * Speed;
			Yaws[EntityIndex]       = FMath::RadiansToDegrees(FMath::Atan2(Steer.Y, Steer.X));
			MarkEchoDirty(EntityIndex, EEchoDirtyFlags::Transform);
			return;
		}

		// A player is within MustPromoteRadius → seek, but with per-Echo jitter + separation so the
		// crowd forms an organic mass instead of a perfect ring on the player's exact point.
		FVector2f Seek(PlayerPositions[BestPlayerIndex].X - Pi.X, PlayerPositions[BestPlayerIndex].Y - Pi.Y);
		if (Seek.IsNearlyZero() && Sep.IsNearlyZero()) return;
		if (!Seek.IsNearlyZero())
		{
			Seek.Normalize();
			if (ApproachJit > 0.f)
				Seek = ATR_RotateVec2(Seek, (EchoHash01(EchoId, 0x5EE6u) * 2.f - 1.f) * ApproachJit);
		}

		FVector2f Steer = Seek + Sep * SepStrength;
		if (Steer.IsNearlyZero()) return;
		Steer.Normalize();

		Velocities[EntityIndex] = FVector3f(Steer.X, Steer.Y, 0.f) * HordeWalkSpeed;
		Yaws[EntityIndex]       = FMath::RadiansToDegrees(FMath::Atan2(Steer.Y, Steer.X));
		MarkEchoDirty(EntityIndex, EEchoDirtyFlags::Transform);
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

// ─── Structural Health ───────────────────────────────────────────────────────

bool UATR_EchoSubsystem::ApplyDamageToEcho(int32 SoAIndex, const FATR_DamageEvent& Event)
{
	// Server authority only. Clients hold a read-only replicated mirror.
	if (!GetWorld() || GetWorld()->GetNetMode() == NM_Client) return false;
	if (SoAIndex < 0 || SoAIndex >= ActiveEntities) return false;
	if (HealthModel.IsDead(SoAIndex)) return false;
	if (Event.Region == EATR_BodyRegion::None) return false;

	const UATR_HealthSettings* HS = GetDefault<UATR_HealthSettings>();

	// Resolve damage components: profile preferred (data-driven), explicit
	// fields otherwise — same convention as the human pipeline.
	struct FResolved { EATR_DamageType Type; float Severity; };
	TArray<FResolved, TInlineAllocator<4>> Resolved;
	float DismemberChance = 0.f;

	if (const UATR_WeaponDamageProfile* Profile = Event.WeaponProfile)
	{
		for (const FATR_WeaponDamageComponent& C : Profile->DamageComponents)
		{
			Resolved.Add({ C.DamageType, FMath::Clamp(C.Severity01 * Event.EventScale01, 0.f, 1.f) });
		}
		DismemberChance = Profile->DismemberChance01;
	}
	else if (Event.DamageType != EATR_DamageType::None)
	{
		Resolved.Add({ Event.DamageType, FMath::Clamp(Event.Severity01 * Event.EventScale01, 0.f, 1.f) });
		// Profile-less events carry no dismember data — limb severing requires
		// a profile (gun/axe/etc.) so debug melee MUST set DebugMeleeProfile to
		// dismember anything; raw explicit damage just erodes brain / cosmetics.
		DismemberChance = 0.f;
	}

	if (Resolved.IsEmpty()) return false;
	DismemberChance = FMath::Clamp(DismemberChance * HS->EchoDismemberScale, 0.f, 1.f);

	const uint32 PartBit = ATR_EchoParts::PartBitsForBodyRegion(Event.Region);

	for (const FResolved& C : Resolved)
	{
		if (C.Severity <= 0.f) continue;

		switch (Event.Region)
		{
		case EATR_BodyRegion::Head:
			// The ONLY path to Echo death: erode the brain. A severing hit can
			// also take the head outright (which zeroes the brain — rule).
			HealthModel.ApplyBrainDamage(SoAIndex, C.Severity * HS->EchoBrainDamageScale);
			if (DismemberChance > 0.f && FMath::FRand() < DismemberChance * C.Severity)
			{
				HealthModel.SeverParts(SoAIndex, ATR_EchoParts::HeadPresent);
			}
			break;

		case EATR_BodyRegion::Chest:
		case EATR_BodyRegion::Abdomen:
		case EATR_BodyRegion::Pelvis:
		{
			// Deep torso trauma can destroy spine function (crawler/twitcher per
			// UATR_HealthSettings). Slashes never reach the spine; pressure does.
			const bool bSpineCapable =
				C.Type == EATR_DamageType::Ballistic ||
				C.Type == EATR_DamageType::Crush     ||
				C.Type == EATR_DamageType::Explosion;
			if (bSpineCapable && C.Severity >= HS->EchoSpineDestroySeverity)
			{
				HealthModel.DestroyFunction(SoAIndex, ATR_EchoParts::SpineFunctional);
			}
			break;
		}

		default:
			// Neck and limb regions: severing roll. Distal parts go with it.
			if (PartBit != 0 && DismemberChance > 0.f && FMath::FRand() < DismemberChance * C.Severity)
			{
				HealthModel.SeverParts(SoAIndex, PartBit);
			}
			break;
		}

		// Cosmetic accumulation for gore presentation (decals/exposed bone).
		FATR_EchoDetailedDamageRecord& Detail = HealthModel.GetOrCreateDetailRecord(SoAIndex);
		Detail.AccumulatedDamage = static_cast<uint8>(
			FMath::Min(255, static_cast<int32>(Detail.AccumulatedDamage) + FMath::RoundToInt(C.Severity * 64.f)));
	}

	if (HS->bLogDamageEvents)
	{
		UE_LOGFMT(LogATR_Health, Log, "EchoDamage Index={Index} Region={Region} Brain={Brain} Caps={Caps} Dead={Dead}",
			("Index", SoAIndex),
			("Region", static_cast<int32>(Event.Region)),
			("Brain", HealthModel.GetBrainIntegrity(SoAIndex)),
			("Caps", HealthModel.GetCapabilityFlags(SoAIndex)),
			("Dead", HealthModel.IsDead(SoAIndex)));
	}

	if (HealthModel.IsDead(SoAIndex))
	{
		// Flush the EchoKilled delta to clients NOW — RemoveEcho recycles this
		// SoA row and would otherwise invalidate the queued kill notification.
		ReplicateEchoHealthDeltas(/*bFlushAll =*/ true);
		ForceDestroyEcho(SoAIndex);
		return true;
	}

	// Surviving structural damage: promoted actors re-derive movement from the
	// fresh capability flags (limp/crawl/immobile) immediately.
	if (AATR_ActiveEcho* Actor = IndexToActor.IsValidIndex(SoAIndex) ? IndexToActor[SoAIndex] : nullptr)
	{
		Actor->ApplyStructuralStateToMovement();
	}

	return true;
}

void UATR_EchoSubsystem::ReplicateEchoHealthDeltas(const bool bFlushAll)
{
	if (HealthModel.NumPendingDeltas() == 0) return;
	if (!GetWorld() || GetWorld()->GetNetMode() == NM_Client) return;

	const int32 MaxPerUpdate = GetDefault<UATR_HealthSettings>()->MaxStructuralDeltasPerUpdate;

	// Components are (re)gathered here because flush calls can arrive outside
	// the scheduler (death path) when the scratch list may be stale.
	GatherReplicationClients();

	do
	{
		HealthDeltaScratch.Reset();
		HealthModel.DrainPendingDeltas(HealthDeltaScratch, MaxPerUpdate);
		if (HealthDeltaScratch.IsEmpty()) break;

		// Reliable broadcast: structural changes are rare, small, and must
		// arrive ordered. Standalone has no remote clients — drained deltas
		// simply expire (the local model is already authoritative).
		for (UATR_EchoReplicationComponent* Comp : ReplicationClientsScratch)
		{
			if (Comp)
			{
				Comp->Client_EchoHealthDeltas(HealthDeltaScratch);
			}
		}
	}
	while (bFlushAll && HealthModel.NumPendingDeltas() > 0);
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
