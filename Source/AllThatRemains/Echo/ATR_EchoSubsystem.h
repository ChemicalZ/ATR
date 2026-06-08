// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"
#include "Subsystems/WorldSubsystem.h"
#include "ATR_EchoRuntimeTypes.h"
#include "ATR_EchoSubsystem.generated.h"

class UATR_EchoReplicationComponent;
class AATR_EchoManager;
class AATR_ActiveEcho;
class AATR_EchoAIController;

UENUM()
enum class EEchoDirtyFlags : uint8
{
	None      = 0,
	Transform = 1 << 0,
	Anim      = 1 << 1,
	Spawn     = 1 << 3,
	Despawn   = 1 << 4,
};
ENUM_CLASS_FLAGS(EEchoDirtyFlags)

struct FEchoDirtyState
{
	EEchoDirtyFlags Flags   = EEchoDirtyFlags::None;
	uint32          Version = 0; // increments each dirty marking; never wraps to 0
};

// Sparse uniform spatial grid. Only occupied cells allocate memory.
// Rebuilt each frame from a local-entity set (entities near players only).
// Never owns entity data. Member named SpatialGrid on the subsystem for
// backward compat with ATR_EchoReplicationComponent direct access.
struct FATR_SparseGrid
{
public:
	void  Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize);
	void  Reset();
	void  Build(TArrayView<const FVector3f> Positions, const TArray<int32>& LocalIndices);
	int32 QueryRadius(FVector3f QueryPos, float Radius,
	                  TArrayView<const FVector3f> Positions,
	                  TArray<int32>& OutIndices) const;

	template<typename FuncType>
	void ForEachInBounds(FVector2D Min, FVector2D Max, FuncType&& Func) const
	{
		const int32 MinCx = CellX(Min.X), MaxCx = CellX(Max.X);
		const int32 MinCy = CellY(Min.Y), MaxCy = CellY(Max.Y);
		for (int32 Cy = MinCy; Cy <= MaxCy; ++Cy)
			for (int32 Cx = MinCx; Cx <= MaxCx; ++Cx)
			{
				const int32 Key = CellIndex(Cx, Cy);
				if (const TArray<int32>* Bucket = Cells.Find(Key))
					for (int32 i : *Bucket)
						Func(i);
			}
	}

	int32 GetNumCells()   const { return NumCellsX * NumCellsY; }
	bool  IsInitialized() const { return NumCellsX > 0; }

	FORCEINLINE float GetCellSize() const { return CellSize; }

	FORCEINLINE bool IsValidCellId(int32 InCellId) const
	{
		return InCellId >= 0 && InCellId < NumCellsX * NumCellsY;
	}

	FORCEINLINE int32 GetCellId(FVector2f Pos2D) const
	{
		return CellIndex(CellX(Pos2D.X), CellY(Pos2D.Y));
	}

	FORCEINLINE FVector2f GetCellOrigin2D(int32 InCellId) const
	{
		const int32 Cx = InCellId % FMath::Max(1, NumCellsX);
		const int32 Cy = InCellId / FMath::Max(1, NumCellsX);
		return FVector2f(
			static_cast<float>(WorldMin.X) + Cx * CellSize,
			static_cast<float>(WorldMin.Y) + Cy * CellSize
		);
	}

private:
	FORCEINLINE int32 CellX(float X) const
	{
		return FMath::Clamp(static_cast<int32>((X - WorldMin.X) * InvCellSize), 0, NumCellsX - 1);
	}
	FORCEINLINE int32 CellY(float Y) const
	{
		return FMath::Clamp(static_cast<int32>((Y - WorldMin.Y) * InvCellSize), 0, NumCellsY - 1);
	}
	FORCEINLINE int32 CellIndex(int32 Cx, int32 Cy) const { return Cy * NumCellsX + Cx; }

	FVector2D WorldMin    = FVector2D::ZeroVector;
	float     CellSize    = 500.f;
	float     InvCellSize = 1.f / 500.f;
	int32     NumCellsX   = 0;
	int32     NumCellsY   = 0;

	TMap<int32, TArray<int32>> Cells;
	TArray<int32>              PopulatedCells;
};

// Global coarse grid storing entity indices per large cell.
// Updated incrementally — only when an entity crosses a coarse cell boundary.
// Enables O(nearby cells) queries instead of O(ActiveEntities) scans.
struct FATR_CoarseGrid
{
public:
	void Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize);

	FORCEINLINE bool IsInitialized() const { return NumCellsX > 0 && NumCellsY > 0; }

	FORCEINLINE int32 GetCellId(FVector2f Pos2D) const
	{
		return CellIndex(CellX(Pos2D.X), CellY(Pos2D.Y));
	}

	void  AddEntity(int32 EntityIndex, int32 CellId, int32& OutSlotInCell);
	int32 RemoveEntityAndReturnMoved(int32 EntityIndex, int32 CellId, int32 SlotInCell);
	void  ReplaceEntityAtSlot(int32 CellId, int32 SlotInCell, int32 ExpectedOld, int32 NewEntity);

	bool ValidateEntitySlot(int32 EntityIndex, int32 CellId, int32 SlotInCell) const;

	template<typename FuncType>
	void ForEachEntityInRadius(FVector2f Origin, float Radius,
		TArrayView<const FVector3f> Positions, FuncType&& Func) const
	{
		const float RadiusSq = Radius * Radius;
		const int32 MinCX = CellX(Origin.X - Radius), MaxCX = CellX(Origin.X + Radius);
		const int32 MinCY = CellY(Origin.Y - Radius), MaxCY = CellY(Origin.Y + Radius);
		for (int32 CY = MinCY; CY <= MaxCY; ++CY)
			for (int32 CX = MinCX; CX <= MaxCX; ++CX)
			{
				const int32 CellId = CellIndex(CX, CY);
				const TArray<int32>* Bucket = CellEntities.Find(CellId);
				if (!Bucket) continue;
				for (int32 Ei : *Bucket)
				{
					const FVector3f& P = Positions[Ei];
					const float DX = P.X - Origin.X, DY = P.Y - Origin.Y;
					if (DX*DX + DY*DY <= RadiusSq) Func(Ei);
				}
			}
	}

	int32 GetOccupiedCellCount()       const { return CellEntities.Num(); }
	int32 GetEntityCountInCell(int32 CellId) const
	{
		const TArray<int32>* B = CellEntities.Find(CellId);
		return B ? B->Num() : 0;
	}

private:
	FORCEINLINE int32 CellX(float X) const
	{
		return FMath::Clamp(static_cast<int32>((X - WorldMin.X) * InvCellSize), 0, NumCellsX - 1);
	}
	FORCEINLINE int32 CellY(float Y) const
	{
		return FMath::Clamp(static_cast<int32>((Y - WorldMin.Y) * InvCellSize), 0, NumCellsY - 1);
	}
	FORCEINLINE int32 CellIndex(int32 Cx, int32 Cy) const { return Cy * NumCellsX + Cx; }

	FVector2D WorldMin    = FVector2D::ZeroVector;
	float     CellSize    = 15000.f;
	float     InvCellSize = 1.f / 15000.f;
	int32     NumCellsX   = 0;
	int32     NumCellsY   = 0;

	TMap<int32, TArray<int32>> CellEntities;
};

UCLASS()
class ALLTHATREMAINS_API UATR_EchoSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void    Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void    OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void    Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool    IsTickable() const override { return bInitialized; }

	// --- Config (read-only at runtime — edit via Project Settings > AllThatRemains > Echo Horde) ---

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 InitializeCount = 10000;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 SpawnCount = 10;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	TSubclassOf<AATR_EchoManager> ManagerClass;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	TSubclassOf<AATR_ActiveEcho> ActiveEchoClass;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	TSubclassOf<AATR_EchoAIController> ControllerClass;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float SpawnRadius = 5000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 PoolSize = 20;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 SimHz = 20;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float GridCellSize = 500.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float WorldHalfExtent = 512000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float MustPromoteRadius = 800.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float HordeWalkSpeed = 120.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float PromoteRadius = 2500.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float DemoteRadius = 4000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float MinTimeInTierSeconds = 1.5f;

	// --- SoA (no UPROPERTY — worker-thread written, never GC-traced) ---
	// Parallel arrays, one entry per live entity [0, ActiveEntities).
	// Never resized after Initialize(). Capacity = InitializeCount.

	TArray<FVector3f>     Forces;
	TArray<FVector3f>     Accelerations;
	TArray<FVector3f>     Velocities;
	TArray<FVector3f>     Positions;
	TArray<uint8>         AnimState;
	TArray<uint8>         AnimFrame;
	TArray<float>         PromotionTimes; // GetWorld()->GetTimeSeconds() at promotion; 0 = not promoted
	TArray<float>         Yaws;           // degrees [0, 360) — facing direction per entity
	TArray<FEchoDirtyState> DirtyStates;
	int32             ActiveEntities = 0;

	// --- Canonical runtime state (Phase 1) ---
	// Stable identity layer. The SoA index is NOT stable (RemoveEcho swap-removes and
	// relabels rows), so canonical intelligence is keyed by a monotonic EchoId instead.
	//
	//   EchoIds[i]        : stable EchoId of the entity currently at SoA row i.
	//   RuntimeStates[i]  : canonical state of the entity at SoA row i (moves with the row).
	//   EchoIdToIndex     : EchoId → current SoA row, for O(1) lookup by id.
	//
	// EchoIds/RuntimeStates are parallel to Positions and are moved alongside it during
	// AddEcho / RemoveEcho swap-remove. EchoId 0 is reserved as invalid.
	TArray<int32>                 EchoIds;
	TArray<FATR_EchoRuntimeState> RuntimeStates;
	TMap<int32, int32>            EchoIdToIndex;
	int32                         NextEchoId = 1;

	// EchoId ⇄ SoA index helpers. Return INDEX_NONE / nullptr on a stale or unknown id.
	int32 GetEchoIdForIndex(int32 Index) const
	{
		return EchoIds.IsValidIndex(Index) ? EchoIds[Index] : INDEX_NONE;
	}
	int32 GetIndexForEchoId(int32 EchoId) const
	{
		const int32* Found = EchoIdToIndex.Find(EchoId);
		return Found ? *Found : INDEX_NONE;
	}

	// Canonical state access by stable EchoId. Mutable variant for the active layer to
	// write facts/intent; const variant for read-only consumers. nullptr if id is stale.
	FATR_EchoRuntimeState*       GetMutableEchoState(int32 EchoId);
	const FATR_EchoRuntimeState* GetEchoState(int32 EchoId) const;

	// Fast index-based accessors for hot subsystem-internal paths that already hold a row.
	FATR_EchoRuntimeState*       GetMutableEchoStateByIndex(int32 Index)
	{
		return RuntimeStates.IsValidIndex(Index) ? &RuntimeStates[Index] : nullptr;
	}

	// Active-layer registration. Called on promotion/demotion to wire (or sever) the
	// controller/pawn bridge and flip the simulation tier. Does not change behavior in
	// Phase 1 — it only records the bridge and tier.
	void RegisterActiveEcho(int32 EchoId, AATR_EchoAIController* Controller, APawn* Pawn);
	void UnregisterActiveEcho(int32 EchoId);

	// --- Perception fact reporting (Phase 2) ---
	// The active AIController calls these to feed canonical awareness. They only WRITE
	// awareness state; intent selection that consumes it lands in Phase 3. Hearing is
	// strictly location-only — it never records an actor as a behavioral target.
	void ReportEchoSawActor(int32 EchoId, AActor* Actor, const FVector& Location, const FVector& Velocity, float TimeSeconds);
	void ReportEchoLostSight(int32 EchoId, AActor* Actor, const FVector& LastKnownLocation, const FVector& LastKnownVelocity, float TimeSeconds);
	void ReportEchoHeardLocation(int32 EchoId, const FVector& Location, float Strength, float TimeSeconds);
	void ReportEchoStimulus(int32 EchoId, const FATR_StimulusEvent& Event);

	// --- Movement result reporting (Phase 5) ---
	// The active controller reports classified movement outcomes here. Records the result on
	// the Echo's movement intent and, for blocked/unreachable failures, seeds the obstacle
	// hook (fully consumed in Phase 9). Never paths anywhere itself.
	void ReportEchoMoveResult(int32 EchoId, bool bSuccess, EATR_MoveFailureReason Reason,
	                          const FVector& Location, AActor* BlockingActor, float TimeSeconds);

	// --- Public API ---

	int32 AddEcho(FVector3f Position);
	void  RemoveEcho(int32 Index);

	// Immediately demote (if promoted) and remove from SoA. Use when an echo dies.
	// Bypasses bBlockDemotion and hysteresis — unconditional.
	void ForceDestroyEcho(int32 SoAIndex);
	void ForceDestroyEcho(AATR_ActiveEcho* Actor); // convenience overload for promoted echoes

	// Promote SoA entity to a pooled Actor for full simulation.
	// Caller must pop Actor from EchoPool first.
	bool PromoteToActive(int32 SoAIndex, AATR_ActiveEcho* Actor);

	// Write Actor state back to SoA and return Actor to pool.
	void DemoteToHorde(AATR_ActiveEcho* Actor);

	// --- High-level pool API (call these from gameplay code) ---

	// Pop from pool, wire IndexToActor, seed from SoA, activate.
	// Returns nullptr and logs a warning if pool is empty.
	AATR_ActiveEcho* PromoteEcho(int32 SoAIndex);

	// Flush Actor state to SoA, deactivate, return to pool.
	void DemoteEcho(AATR_ActiveEcho* Actor);

	const FATR_SparseGrid& GetSpatialGrid() const { return SpatialGrid; }
	bool                    IsGridReady()    const { return bGridReady; }

	void MarkEchoDirty(int32 Index, EEchoDirtyFlags Flags);
	bool IsEchoDirty  (int32 Index, EEchoDirtyFlags Flags) const;

	float PositionDirtyThresholdSq = 25.f;  // set from settings in Initialize()
	float YawDirtyThresholdDeg     = 2.f;

	// Returns actor world location for promoted entities, SoA position otherwise.
	FVector3f GetEchoQueryPosition(int32 Index) const;

	// Single FarRange grid query, results classified into three XY-distance bands.
	// OutNear/Mid/Far are non-overlapping. Appends to caller's arrays (caller owns Reset).
	void QueryEchoesByRelevancyBands(
		const FVector& Origin,
		float NearRange, float MidRange, float FarRange,
		TArray<int32>& OutNear, TArray<int32>& OutMid, TArray<int32>& OutFar) const;

	bool IsInitialized() const
	{
		return bInitialized                       &&
		       Positions.Num()  >= InitializeCount &&
		       Yaws.Num()       >= InitializeCount &&
		       AnimState.Num()  >= InitializeCount;
	}

	bool ValidateActiveArrays() const
	{
		return ActiveEntities >= 0                  &&
		       ActiveEntities <= InitializeCount    &&
		       Positions.Num()   >= ActiveEntities  &&
		       Yaws.Num()        >= ActiveEntities  &&
		       AnimState.Num()   >= ActiveEntities  &&
		       IndexToActor.Num() >= ActiveEntities;
	}

	bool ValidateEchoSpatialState() const;

	// Client-side partial replication support. On clients, only snapshot-received
	// echo indices are relevant/valid for local coarse/fine grid and ISM queries.
	// Server/standalone treat every index in [0, ActiveEntities) as relevant.
	bool IsEchoClientRelevant(int32 Index) const;
	void MarkEchoClientRelevant(int32 Index);
	void MarkEchoClientIrrelevant(int32 Index);
	void MarkEchoesClientIrrelevant(const TArray<int32>& Indices);
	void ClearClientEchoRelevancy();

	// Called by AATR_EchoManager::BeginPlay on all machines — wires the Manager pointer
	// on clients (where the Subsystem didn't spawn the Manager itself).
	void SetManager(AATR_EchoManager* InManager) { Manager = InManager; }

	// Reverse map: SoA index → promoted Actor. nullptr = in horde.
	// Raw observer pointers — lifetime guaranteed by EchoPool TObjectPtr.
	UPROPERTY()
	TArray<AATR_ActiveEcho*> IndexToActor;

	// Compact list of SoA indices currently promoted to an Actor.
	// Maintained by PromoteEcho/DemoteEcho/RemoveEcho. Never has nullptr entries.
	TArray<int32> PromotedIndices;

	// Client-only validity mask for partial echo replication. On clients, indices
	// with false bits must not be registered into spatial grids or rendered by ISM.
	// On server/standalone this mask is ignored.
	TBitArray<> ClientRelevantEchoMask;

	// Compact client-only list of snapshot-relevant horde echo indices. This keeps
	// client coarse-grid updates proportional to relevant echoes instead of the
	// highest replicated SoA index. ClientRelevantEchoSlots maps SoA index -> slot
	// in ClientRelevantEchoIndices for O(1) removal by swap.
	TArray<int32> ClientRelevantEchoIndices;
	TArray<int32> ClientRelevantEchoSlots;

	// SoA parallel arrays for coarse grid registration (parallel to Positions).
	// CoarseCellIds: current coarse cell per entity (INDEX_NONE = not registered).
	// CoarseSlotInCell: slot within that cell's bucket.
	TArray<int32> CoarseCellIds;
	TArray<int32> CoarseSlotInCell;

	// Epoch-stamp per entity for O(1) dedup in RebuildFineGrid.
	TArray<uint32> LocalVisitStamp;
	uint32         LocalVisitEpoch = 1;

	// Local-entity indices from the previous frame; used to zero exited velocities.
	TArray<int32> LastLocalEntityScratch;

	void SimTick(float DeltaTime);
	void RebuildFineGrid();
	void UpdateCoarseGrid();

	// Fine local grid — rebuilt each tick from entities within LocalZoneRadius of any player.
	// Member name SpatialGrid preserved for ATR_EchoReplicationComponent direct access.
	FATR_SparseGrid SpatialGrid;

	// Global coarse grid — entity-bucket spatial index, updated on cell boundary crossings.
	FATR_CoarseGrid CoarseGrid;

	UPROPERTY()
	TObjectPtr<AATR_EchoManager> Manager;

	UPROPERTY()
	TArray<TObjectPtr<AATR_ActiveEcho>> EchoPool;

	UPROPERTY()
	TArray<TObjectPtr<AATR_EchoAIController>> ControllerPool;

	void RunSteeringPass();
	void RunPromotionPass();

	// --- Intent selection (Phase 3) ---
	// Chooses each relevant Echo's high-level EATR_EchoIntent + move request from its
	// canonical awareness, and decays confidence/urgency/agitation. Runs server-side on
	// the render tick over active (promoted) echoes; lower-tier echoes are folded in by
	// Phase 11. The active StateTree consumes the result via the Phase 4 intent evaluator.
	void RunIntentPass(float DeltaTime);

	// Per-Echo intent state machine. Refreshes live transform from the SoA, applies
	// decays, and writes State.Intent + State.Movement.Request. Pure function of state.
	void UpdateEchoIntent(int32 Index, float Now, float DeltaTime);

	// --- Horde agitation field (Phase 8) ---
	// Indirect, hive-mind-free horde model. Sources (noise, combat, a seeing Echo) deposit a
	// scalar + weighted direction into a coarse cell; the field decays each tick; Echoes sample
	// their neighborhood to gain curiosity/pressure — never another Echo's exact target.
	//
	// Keyed by quantized (x,y) cell so it needs no world-size precomputation. Only agitated
	// cells consume memory; cells are pruned once they decay to nothing.
	TMap<FIntPoint, FATR_AgitationCell> AgitationField;

	float AgitationCellSize        = 2000.f; // cm per agitation cell
	float AgitationFieldDecayPerSec = 0.25f; // how fast deposited pressure fades
	float HordeCuriosityThreshold   = 0.20f; // >= → TurnTowardStimulus (curious)

	// Deposit agitation at a world location with an optional pressure direction. Public so
	// gameplay (gunshots, sprint noise, combat, scripted events) can drive the horde field.
	void AddWorldAgitation(const FVector& Location, float Amount, const FVector& Direction);

	// Decay + prune the agitation field. Runs once per server tick.
	void DecayAgitationField(float DeltaTime);

	// Sample the 3x3 neighborhood around a world location. Returns the blended agitation and
	// a normalized pressure direction (zero if no meaningful pressure). Pure read.
	void SampleAgitationField(const FVector& Location, float& OutAgitation, FVector& OutDirection) const;

	FORCEINLINE FIntPoint AgitationCellKey(const FVector& Location) const
	{
		return FIntPoint(
			FMath::FloorToInt(static_cast<float>(Location.X) / AgitationCellSize),
			FMath::FloorToInt(static_cast<float>(Location.Y) / AgitationCellSize));
	}

	// Intent tuning (defaults here; migrate to UATR_EchoSettings when values stabilise).
	float ConfidenceDecayPerSec        = 0.15f; // sight memory fade rate when not looking
	float UrgencyDecayPerSec           = 0.20f; // pursuit aggression fade rate
	float AgitationDecayPerSec         = 0.10f; // horde-pressure fade rate
	float LostSightMemoryThreshold     = 0.05f; // confidence below this → forget & idle
	float HeardInvestigateUrgency      = 0.40f; // >= → InvestigateLocation, else TurnTowardStimulus
	float HeardMemorySeconds           = 8.0f;  // how long a heard location stays actionable
	float AgitationJoinThreshold       = 0.50f; // >= → JoinHordePressure
	float ReachLocationRadius          = 120.f; // "arrived" tolerance for memory/search points
	float SightProjectionSeconds       = 2.0f;  // lead time for projected-direction search
	float MaxSightProjectionDistance   = 800.f; // clamp so prediction can't be supernatural

	// Frame-scope scratch for local entity indices; allocation persists across ticks.
	TArray<int32> LocalEntityScratch;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float LocalZoneRadius = 27500.f;  // must be >= FarRelevancyRange for replication queries

	float CoarseGridCellSize = 15000.f;

	float TickAccumulator = 0.f;
	bool  bGridReady      = false;
	bool  bInitialized    = false;

	// Replication scheduler config (read from settings at Initialize — never per-tick)
	float  ServerReplicationBudgetMs      = 1.5f;
	int32  MaxReplicationJobsPerFrame     = 8;
	int32  MaxSnapshotsPerClientPerFrame  = 256;
	int32  MaxNearReplicationJobsPerFrame = 8;
	int32  MaxMidReplicationJobsPerFrame  = 4;
	int32  MaxFarReplicationJobsPerFrame  = 2;

	// Round-robin cursor: advances by 1 each scheduler pass so clients are served fairly.
	int32 ReplicationClientCursor = 0;

	// Transient per-frame scratch — raw observer pointers, no ownership.
	TArray<UATR_EchoReplicationComponent*> ReplicationClientsScratch;

	void GatherReplicationClients();
	void TickReplicationScheduler(float DeltaTime);

private:
	void RegisterEntityToCoarseGrid(int32 EntityIndex);
	void UnregisterEntityFromCoarseGrid(int32 EntityIndex);
	void MoveEntityCoarseCell(int32 EntityIndex, int32 NewCellId);
	bool ShouldProcessEchoForLocalHorde(int32 Index) const;
};
