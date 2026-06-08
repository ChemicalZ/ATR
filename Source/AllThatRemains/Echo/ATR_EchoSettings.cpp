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

	// Awareness — decays non-negative, thresholds in [0,1]
	ConfidenceDecayPerSecond  = FMath::Max(0.f, ConfidenceDecayPerSecond);
	UrgencyDecayPerSecond     = FMath::Max(0.f, UrgencyDecayPerSecond);
	LostSightMemoryThreshold  = FMath::Clamp(LostSightMemoryThreshold, 0.f, 1.f);
	HeardMemorySeconds        = FMath::Max(0.f, HeardMemorySeconds);
	HeardInvestigateUrgency   = FMath::Clamp(HeardInvestigateUrgency, 0.f, 1.f);
	SmellMemorySeconds        = FMath::Max(0.f, SmellMemorySeconds);
	SmellInvestigateUrgency   = FMath::Clamp(SmellInvestigateUrgency, 0.f, 1.f);
	ReacquireSightConfidence  = FMath::Clamp(ReacquireSightConfidence, 0.f, 1.f);
	SightLossGraceSeconds     = FMath::Max(0.f, SightLossGraceSeconds);

	// Sight — LoseSight >= Sight provides hysteresis
	ActiveSightRadius                  = FMath::Max(1.f, ActiveSightRadius);
	ActiveLoseSightRadius              = FMath::Max(ActiveSightRadius, ActiveLoseSightRadius);
	ActivePeripheralVisionAngleDegrees = FMath::Clamp(ActivePeripheralVisionAngleDegrees, 0.f, 180.f);
	ActiveSightMaxAgeSeconds           = FMath::Max(0.f, ActiveSightMaxAgeSeconds);
	LastSeenProjectionSeconds          = FMath::Max(0.f, LastSeenProjectionSeconds);
	MaxLastSeenProjectionDistance      = FMath::Max(0.f, MaxLastSeenProjectionDistance);
	LastSeenVelocitySmoothingAlpha     = FMath::Clamp(LastSeenVelocitySmoothingAlpha, 0.f, 1.f);

	// Hearing — weak threshold must not exceed strong threshold
	ActiveHearingRange              = FMath::Max(1.f, ActiveHearingRange);
	ActiveHearingMaxAgeSeconds      = FMath::Max(0.f, ActiveHearingMaxAgeSeconds);
	NoiseStrengthToUrgencyScale     = FMath::Max(0.f, NoiseStrengthToUrgencyScale);
	NoiseStrengthToAgitationScale   = FMath::Max(0.f, NoiseStrengthToAgitationScale);
	WeakNoiseTurnOnlyThreshold      = FMath::Clamp(WeakNoiseTurnOnlyThreshold, 0.f, 1.f);
	StrongNoiseInvestigateThreshold = FMath::Clamp(StrongNoiseInvestigateThreshold, WeakNoiseTurnOnlyThreshold, 1.f);

	// Search
	ReachLocationRadius            = FMath::Max(0.f, ReachLocationRadius);
	SearchDefaultRadius            = FMath::Max(0.f, SearchDefaultRadius);
	SearchMaxDurationSeconds       = FMath::Max(0.f, SearchMaxDurationSeconds);
	SearchStepAcceptanceRadius     = FMath::Max(0.f, SearchStepAcceptanceRadius);
	SearchPointNavProjectionRadius = FMath::Max(0.f, SearchPointNavProjectionRadius);
	SearchRandomAngleDegrees       = FMath::Max(0.f, SearchRandomAngleDegrees);
	MaxSearchSteps                 = FMath::Max(1, MaxSearchSteps);

	// Agitation — curiosity threshold must not exceed join threshold
	AgitationCellSize                    = FMath::Max(1.f, AgitationCellSize);
	AgitationFieldDecayPerSecond         = FMath::Max(0.f, AgitationFieldDecayPerSecond);
	EchoPersonalAgitationDecayPerSecond  = FMath::Max(0.f, EchoPersonalAgitationDecayPerSecond);
	HordeCuriosityThreshold              = FMath::Clamp(HordeCuriosityThreshold, 0.f, 1.f);
	AgitationJoinThreshold               = FMath::Clamp(AgitationJoinThreshold, HordeCuriosityThreshold, 1.f);
	SightAgitationAmount                 = FMath::Clamp(SightAgitationAmount, 0.f, 1.f);
	NoiseAgitationAmountScale            = FMath::Max(0.f, NoiseAgitationAmountScale);
	CombatAgitationAmount                = FMath::Clamp(CombatAgitationAmount, 0.f, 1.f);
	EchoAgitationSpreadRadius            = FMath::Max(0.f, EchoAgitationSpreadRadius);
	HordePressureMoveDistance            = FMath::Max(0.f, HordePressureMoveDistance);
	HordePressureDirectionSmoothingAlpha = FMath::Clamp(HordePressureDirectionSmoothingAlpha, 0.f, 1.f);

	// Lower-tier simulation
	LowDetailUpdateHz                = FMath::Clamp(LowDetailUpdateHz, 0.1f, 60.f);
	AbstractUpdateHz                 = FMath::Clamp(AbstractUpdateHz, 0.1f, 60.f);
	LowDetailMaxUpdatesPerTick       = FMath::Max(1, LowDetailMaxUpdatesPerTick);
	AbstractMaxCellsPerTick          = FMath::Max(1, AbstractMaxCellsPerTick);
	LowDetailStimulusQueryRadius     = FMath::Max(0.f, LowDetailStimulusQueryRadius);
	LowDetailInvestigateSpeed        = FMath::Max(0.f, LowDetailInvestigateSpeed);
	LowDetailWanderSpeed             = FMath::Max(0.f, LowDetailWanderSpeed);
	LowDetailSearchSpeed             = FMath::Max(0.f, LowDetailSearchSpeed);
	LowDetailSearchRadius            = FMath::Max(0.f, LowDetailSearchRadius);
	LowDetailSearchDurationSeconds   = FMath::Max(0.f, LowDetailSearchDurationSeconds);
	LowDetailPromotionUrgencyBoost   = FMath::Max(0.f, LowDetailPromotionUrgencyBoost);
	LowDetailPromotionAgitationBoost = FMath::Max(0.f, LowDetailPromotionAgitationBoost);
	AbstractCellMigrationRate            = FMath::Clamp(AbstractCellMigrationRate, 0.f, 1.f);
	AbstractCellNoiseAttractionScale     = FMath::Max(0.f, AbstractCellNoiseAttractionScale);
	AbstractCellAgitationAttractionScale = FMath::Max(0.f, AbstractCellAgitationAttractionScale);

	// Obstacle hooks
	ObstacleForwardTraceLength   = FMath::Max(0.f, ObstacleForwardTraceLength);
	ObstacleTraceRadius          = FMath::Max(0.f, ObstacleTraceRadius);
	ObstacleHandleTimeoutSeconds = FMath::Max(0.f, ObstacleHandleTimeoutSeconds);
	ObstacleSidestepDistance     = FMath::Max(0.f, ObstacleSidestepDistance);
	ObstacleForwardNudgeDistance = FMath::Max(0.f, ObstacleForwardNudgeDistance);
	ObstacleRetryCooldownSeconds = FMath::Max(0.f, ObstacleRetryCooldownSeconds);

	// Demotion
	DemotionConfidenceBlockThreshold = FMath::Clamp(DemotionConfidenceBlockThreshold, 0.f, 1.f);
	DemotionUrgencyBlockThreshold    = FMath::Clamp(DemotionUrgencyBlockThreshold, 0.f, 1.f);
	DemotionSearchBlockSeconds       = FMath::Max(0.f, DemotionSearchBlockSeconds);

	// Promotion scoring weights
	MustPromoteScoreBonus      = FMath::Max(0.f, MustPromoteScoreBonus);
	PromotionUrgencyBoost      = FMath::Max(0.f, PromotionUrgencyBoost);
	PromotionConfidenceBoost   = FMath::Max(0.f, PromotionConfidenceBoost);
	PromotionAgitationBoost    = FMath::Max(0.f, PromotionAgitationBoost);
	PromotionPlayerFacingBoost = FMath::Max(0.f, PromotionPlayerFacingBoost);
	RecentlyDemotedPenalty     = FMath::Max(0.f, RecentlyDemotedPenalty);
	RecentlyDemotedSeconds     = FMath::Max(0.f, RecentlyDemotedSeconds);
}

#if WITH_EDITOR
void UATR_EchoSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ValidateAndClamp();
}
#endif
