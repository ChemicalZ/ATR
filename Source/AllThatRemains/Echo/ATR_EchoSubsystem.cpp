// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoSubsystem.h"
#include "ATR_EchoManager.h"
#include "ATR_EchoReplicationComponent.h"
#include "ATR_ActiveEcho.h"
#include "AI/ATR_EchoAIController.h"
#include "ATR_EchoSettings.h"
#include "Engine/World.h"
#include "Async/ParallelFor.h"
#include "Logging/StructuredLog.h"

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
		Cells[Key].Reset();
	PopulatedCells.Reset();
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

	CellCounts.Reset();
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
	CoarseCellIds.Init(INDEX_NONE, InitializeCount);

	PositionDirtyThresholdSq = FMath::Square(Settings->PositionDirtyThreshold);
	YawDirtyThresholdDeg     = Settings->YawDirtyThresholdDegrees;

	PromotedIndices.Reserve(PoolSize);
	LocalEntityScratch.Reserve(256);
	WasLocalLastFrame.Init(false, InitializeCount);

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
	Super::Tick(DeltaTime);

	const ENetMode NetMode = GetWorld()->GetNetMode();

	// Fine grid first — steering pass, promotion pass, and ISM all need it.
	// Clients rebuild from received snapshot positions; server from authoritative SoA.
	if (ActiveEntities > 0)
	{
		RebuildFineGrid();
		UpdateCoarseGrid();
	}

	if (NetMode != NM_Client && ActiveEntities > 0)
	{
		// Steering writes horde velocities before SimTick integrates them.
		RunSteeringPass();

		TickAccumulator += DeltaTime;
		const float SimInterval = 1.f / static_cast<float>(FMath::Max(1, SimHz));
		while (TickAccumulator >= SimInterval)
		{
			SimTick(SimInterval);
			TickAccumulator -= SimInterval;
		}

		// Promotion pass after integration — IndexToActor is stable for UpdateISM.
		RunPromotionPass();
	}

	if (Manager && NetMode != NM_DedicatedServer)
		Manager->UpdateISM(this);

	if (NetMode != NM_Client)
	{
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
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

			Comp->ServerTickReplication(this, DeltaTime);
		}
	}
}

void UATR_EchoSubsystem::SimTick(float DeltaTime)
{
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
	// Build local entity set — entities within LocalZoneRadius of any player.
	// Use TSet for O(1) dedup in multi-player scenarios.
	TSet<int32> LocalSet;
	LocalEntityScratch.Reset();

	const float ZoneSq = LocalZoneRadius * LocalZoneRadius;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || !PC->GetPawn()) continue;
		const FVector3f PP = FVector3f(PC->GetPawn()->GetActorLocation());

		for (int32 i = 0; i < ActiveEntities; ++i)
		{
			const FVector3f D = Positions[i] - PP;
			if (D.X * D.X + D.Y * D.Y <= ZoneSq)
			{
				bool bAlreadyIn = false;
				LocalSet.Add(i, &bAlreadyIn);
				if (!bAlreadyIn)
					LocalEntityScratch.Add(i);
			}
		}
	}

	// Zero velocities for entities that left the local zone since last frame.
	// Without this, an entity that was steered and then exited the zone would
	// continue integrating a stale velocity indefinitely.
	for (int32 i = 0; i < ActiveEntities; ++i)
	{
		if (WasLocalLastFrame[i] && !IndexToActor[i] && !LocalSet.Contains(i))
			Velocities[i] = FVector3f::ZeroVector;
	}

	// Update zone membership for next frame
	WasLocalLastFrame.Init(false, InitializeCount);
	for (int32 i : LocalEntityScratch)
		WasLocalLastFrame[i] = true;

	SpatialGrid.Reset();
	SpatialGrid.Build(
		TArrayView<const FVector3f>(Positions.GetData(), ActiveEntities),
		LocalEntityScratch
	);
	bGridReady = true;
}

void UATR_EchoSubsystem::UpdateCoarseGrid()
{
	for (int32 i = 0; i < ActiveEntities; ++i)
	{
		const int32 NewCell = CoarseGrid.GetCellId(FVector2f(Positions[i].X, Positions[i].Y));
		if (CoarseCellIds[i] == INDEX_NONE)
		{
			CoarseGrid.OnEntityAdded(NewCell);
			CoarseCellIds[i] = NewCell;
		}
		else if (NewCell != CoarseCellIds[i])
		{
			CoarseGrid.OnEntityRemoved(CoarseCellIds[i]);
			CoarseGrid.OnEntityAdded(NewCell);
			CoarseCellIds[i] = NewCell;
		}
	}
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
	CoarseCellIds[Idx]  = INDEX_NONE; // registered by UpdateCoarseGrid() on first tick
	return Idx;
}

void UATR_EchoSubsystem::RemoveEcho(int32 Index)
{
	if (Index < 0 || Index >= ActiveEntities)
		return;

	// Caller must demote before removing — promoted actors track their SoA slot.
	ensureAlways(!IndexToActor[Index]);

	// Remove entity from coarse grid before slot is vacated
	if (CoarseCellIds[Index] != INDEX_NONE)
	{
		CoarseGrid.OnEntityRemoved(CoarseCellIds[Index]);
		CoarseCellIds[Index] = INDEX_NONE;
	}

	const int32 Last = --ActiveEntities;

	if (Index != Last)
	{
		// Swap-remove: copy Last into gap
		Positions[Index]     = Positions[Last];
		Velocities[Index]    = Velocities[Last];
		Accelerations[Index] = Accelerations[Last];
		Forces[Index]        = Forces[Last];
		AnimState[Index]      = AnimState[Last];
		AnimFrame[Index]      = AnimFrame[Last];
		PromotionTimes[Index] = PromotionTimes[Last];
		DirtyStates[Index]    = DirtyStates[Last];
		Yaws[Index]           = Yaws[Last];
		CoarseCellIds[Index]  = CoarseCellIds[Last];
		CoarseCellIds[Last]   = INDEX_NONE;

		// Patch the moved entity's Actor so it knows its new slot
		if (IndexToActor[Last])
		{
			IndexToActor[Index]              = IndexToActor[Last];
			IndexToActor[Index]->SourceIndex = Index;
			IndexToActor[Last]               = nullptr;

			// Patch PromotedIndices: Last moved to Index
			const int32 PIIdx = PromotedIndices.IndexOfByKey(Last);
			if (ensureAlways(PIIdx != INDEX_NONE))
				PromotedIndices[PIIdx] = Index;
		}
	}
}

void UATR_EchoSubsystem::PromoteToActive(int32 SoAIndex, AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor && SoAIndex >= 0 && SoAIndex < ActiveEntities)) return;
	if (!ensureAlways(!IndexToActor[SoAIndex])) return; // double-promote guard

	IndexToActor[SoAIndex]    = Actor;
	Actor->SourceIndex         = SoAIndex;
	PromotionTimes[SoAIndex]  = GetWorld()->GetTimeSeconds();
}

void UATR_EchoSubsystem::DemoteToHorde(AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor)) return;

	const int32 Idx = Actor->SourceIndex;
	if (!ensureAlways(Idx >= 0 && Idx < ActiveEntities)) return;

	Actor->WriteBackToSoA(this);  // flush Actor state → SoA before severing link
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

	PromoteToActive(SoAIndex, Actor);    // wire IndexToActor + SourceIndex
	PromotedIndices.Add(SoAIndex);
	Actor->InitFromSoA(this, SoAIndex);  // teleport to SoA position + seed velocity first
	Controller->Possess(Actor);          // OnPossess → AI wakes at correct world position
	return Actor;
}

void UATR_EchoSubsystem::DemoteEcho(AATR_ActiveEcho* Actor)
{
	if (!ensureAlways(Actor)) return;

	// Capture SoAIndex before DemoteToHorde clears Actor->SourceIndex
	const int32 SoAIndex = Actor->SourceIndex;

	// Capture controller before UnPossess clears the pawn reference.
	AATR_EchoAIController* Controller = Cast<AATR_EchoAIController>(Actor->GetController());

	DemoteToHorde(Actor);  // WriteBackToSoA + clear IndexToActor + SourceIndex = INDEX_NONE

	// Maintain compact promoted list
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
			const FVector3f DA = Positions[A] - PP;
			const FVector3f DB = Positions[B] - PP;
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
				const FVector3f D = Positions[j] - PP;
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

		// Demotion requires entity to be beyond DemoteRadius from ALL players
		bool bAnyClose = false;
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (!PC || !PC->GetPawn()) continue;
			const FVector3f D = Positions[i] - FVector3f(PC->GetPawn()->GetActorLocation());
			if (D.X * D.X + D.Y * D.Y <= DemRadSq) { bAnyClose = true; break; }
		}
		if (bAnyClose) continue;

		DemoteEcho(Actor); // clears PromotionTimes[i] inside DemoteToHorde
	}
}

// ─── RunSteeringPass ──────────────────────────────────────────────────────────

void UATR_EchoSubsystem::RunSteeringPass()
{
	// Horde velocities for zone-exiters are zeroed inside RebuildFineGrid().
	// Entities entering the local zone start at zero (AddEcho initializes to zero).

	// Apply steering velocity for unpromoted overflow entities within MustPromoteRadius.
	TArray<int32> InnerCandidates;
	InnerCandidates.Reserve(64);

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || !PC->GetPawn()) continue;

		const FVector3f PP = FVector3f(PC->GetPawn()->GetActorLocation());
		InnerCandidates.Reset();
		SpatialGrid.QueryRadius(PP, MustPromoteRadius,
			TArrayView<const FVector3f>(Positions.GetData(), ActiveEntities),
			InnerCandidates);

		// QueryRadius returns each entity at most once — unique SoA index per lane, no cross-lane writes.
		ParallelFor(InnerCandidates.Num(), [this, &InnerCandidates, PP](int32 CandIdx)
		{
			const int32 i = InnerCandidates[CandIdx];
			if (IndexToActor[i]) return;

			FVector3f Dir = PP - Positions[i];
			Dir.Z = 0.f;
			const float DistSq = Dir.X * Dir.X + Dir.Y * Dir.Y;
			if (DistSq > KINDA_SMALL_NUMBER)
			{
				Velocities[i] = Dir * FMath::InvSqrt(DistSq) * HordeWalkSpeed;
				Yaws[i]       = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
				MarkEchoDirty(i, EEchoDirtyFlags::Transform);
			}
		});
	}
}

// ─── QueryEchoesByRelevancyBands ──────────────────────────────────────────────

void UATR_EchoSubsystem::QueryEchoesByRelevancyBands(
	const FVector& Origin,
	float NearRange, float MidRange, float FarRange,
	TArray<int32>& OutNear, TArray<int32>& OutMid, TArray<int32>& OutFar) const
{
	if (!bGridReady || ActiveEntities == 0) return;

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

// ─── ForceDestroyEcho ─────────────────────────────────────────────────────────

void UATR_EchoSubsystem::ForceDestroyEcho(int32 SoAIndex)
{
	if (SoAIndex < 0 || SoAIndex >= ActiveEntities) return;

	// Demote first if promoted — bypasses bBlockDemotion and hysteresis.
	if (AATR_ActiveEcho* Actor = IndexToActor[SoAIndex])
		DemoteEcho(Actor);

	RemoveEcho(SoAIndex);
}

void UATR_EchoSubsystem::ForceDestroyEcho(AATR_ActiveEcho* Actor)
{
	if (!Actor || Actor->SourceIndex == INDEX_NONE) return;

	const int32 Idx = Actor->SourceIndex; // capture before DemoteEcho clears SourceIndex
	DemoteEcho(Actor);
	RemoveEcho(Idx);
}
