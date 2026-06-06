// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoSettings.h"

// Defensive clamp of every setting to its required invariant. Ordering matters:
// dependent values are resolved after their dependencies so chained invariants
// (e.g. MustPromote < Promote < Demote) settle in a single pass.
void UATR_EchoSettings::ValidateAndClamp()
{
	// Population
	InitializeCount = FMath::Max(1, InitializeCount);
	SpawnCount      = FMath::Max(0, SpawnCount);
	SpawnRadius     = FMath::Max(0.f, SpawnRadius);

	// Simulation
	SimHz          = FMath::Clamp(SimHz, 1, 120);
	HordeWalkSpeed = FMath::Max(0.f, HordeWalkSpeed);

	// Spatial
	GridCellSize       = FMath::Max(100.f, GridCellSize);
	WorldHalfExtent    = FMath::Max(1000.f, WorldHalfExtent);
	CoarseGridCellSize = FMath::Max(GridCellSize, CoarseGridCellSize);

	// Promotion — MustPromote < Promote < Demote
	MustPromoteRadius = FMath::Max(100.f, MustPromoteRadius);
	PromoteRadius     = FMath::Max(MustPromoteRadius + 1.f, PromoteRadius);
	DemoteRadius      = FMath::Max(PromoteRadius + 1.f, DemoteRadius);
	MinTimeInTierSeconds = FMath::Max(0.f, MinTimeInTierSeconds);

	// Rendering — Near <= Mid <= Far
	VisualNearDistance = FMath::Max(100.f, VisualNearDistance);
	VisualMidDistance  = FMath::Max(VisualNearDistance, VisualMidDistance);
	VisualFarDistance  = FMath::Max(VisualMidDistance, VisualFarDistance);

	// Networking — Near <= Mid <= Far
	NearRelevancyRange = FMath::Max(100.f, NearRelevancyRange);
	MidRelevancyRange  = FMath::Max(NearRelevancyRange, MidRelevancyRange);
	FarRelevancyRange  = FMath::Max(MidRelevancyRange, FarRelevancyRange);

	// LocalZoneRadius must cover every range that queries the fine grid.
	LocalZoneRadius = FMath::Max(LocalZoneRadius, DemoteRadius);
	LocalZoneRadius = FMath::Max(LocalZoneRadius, VisualFarDistance);
	LocalZoneRadius = FMath::Max(LocalZoneRadius, FarRelevancyRange);

	// Networking snapshot rates — Near >= Mid >= Far
	NearSnapshotHz = FMath::Clamp(NearSnapshotHz, 1.f, 60.f);
	MidSnapshotHz  = FMath::Clamp(MidSnapshotHz, 0.1f, NearSnapshotHz);
	FarSnapshotHz  = FMath::Clamp(FarSnapshotHz, 0.1f, MidSnapshotHz);

	MaxSnapshotsPerChunk            = FMath::Clamp(MaxSnapshotsPerChunk, 1, 512);
	FullResyncCooldownSeconds       = FMath::Max(0.f, FullResyncCooldownSeconds);
	MaxMissingSequencesBeforeResync = FMath::Max(1, MaxMissingSequencesBeforeResync);

	ServerReplicationBudgetMs        = FMath::Clamp(ServerReplicationBudgetMs, 0.1f, 10.f);
	MaxReplicationJobsPerFrame       = FMath::Clamp(MaxReplicationJobsPerFrame, 1, 64);
	MaxSnapshotsPerClientPerFrame    = FMath::Clamp(MaxSnapshotsPerClientPerFrame, 1, 2048);
	MaxNearReplicationJobsPerFrame   = FMath::Clamp(MaxNearReplicationJobsPerFrame, 1, MaxReplicationJobsPerFrame);
	MaxMidReplicationJobsPerFrame    = FMath::Clamp(MaxMidReplicationJobsPerFrame, 0, MaxReplicationJobsPerFrame);
	MaxFarReplicationJobsPerFrame    = FMath::Clamp(MaxFarReplicationJobsPerFrame, 0, MaxReplicationJobsPerFrame);

	// Dirty State
	PositionDirtyThreshold   = FMath::Max(0.f, PositionDirtyThreshold);
	YawDirtyThresholdDegrees = FMath::Max(0.f, YawDirtyThresholdDegrees);
}

#if WITH_EDITOR
void UATR_EchoSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ValidateAndClamp();
}
#endif
