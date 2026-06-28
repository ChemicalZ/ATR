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
	HordeWalkSpeedVariation = FMath::Clamp(HordeWalkSpeedVariation, 0.f, 0.9f);

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
	ReacquireSightConfidence  = FMath::Clamp(ReacquireSightConfidence, 0.f, 1.f);

	// Sight — LoseSight >= Sight provides hysteresis
	ActiveSightRadius                  = FMath::Max(1.f, ActiveSightRadius);
	ActiveLoseSightRadius              = FMath::Max(ActiveSightRadius, ActiveLoseSightRadius);
	ActivePeripheralVisionAngleDegrees = FMath::Clamp(ActivePeripheralVisionAngleDegrees, 0.f, 180.f);
	ActiveSightMaxAgeSeconds           = FMath::Max(0.f, ActiveSightMaxAgeSeconds);
	LastSeenProjectionSeconds          = FMath::Max(0.f, LastSeenProjectionSeconds);
	MaxLastSeenProjectionDistance      = FMath::Max(0.f, MaxLastSeenProjectionDistance);
	LastSeenVelocitySmoothingAlpha     = FMath::Clamp(LastSeenVelocitySmoothingAlpha, 0.f, 1.f);

	// Acoustics — saturation must not fall below the hearing threshold
	AcousticReferenceDistanceCm   = FMath::Max(1.f, AcousticReferenceDistanceCm);
	EchoHearingThresholdDb        = FMath::Clamp(EchoHearingThresholdDb, 0.f, 194.f);
	EchoHearingSaturationDb       = FMath::Clamp(EchoHearingSaturationDb, EchoHearingThresholdDb, 194.f);
	AirAbsorptionDbPer100m        = FMath::Clamp(AirAbsorptionDbPer100m, 0.f, 10.f);
	MaxAudibleRangeCm             = FMath::Max(100.f, MaxAudibleRangeCm);
	DefaultPerceivedNoiseLoudnessDb = FMath::Clamp(DefaultPerceivedNoiseLoudnessDb, 0.f, 194.f);
	HearingMaxLocationErrorFraction = FMath::Clamp(HearingMaxLocationErrorFraction, 0.f, 1.f);

	// Hearing — weak threshold must not exceed strong threshold
	ActiveHearingRange              = FMath::Max(1.f, ActiveHearingRange);
	ActiveHearingMaxAgeSeconds      = FMath::Max(0.f, ActiveHearingMaxAgeSeconds);
	NoiseStrengthToUrgencyScale     = FMath::Max(0.f, NoiseStrengthToUrgencyScale);
	NoiseStrengthToAgitationScale   = FMath::Max(0.f, NoiseStrengthToAgitationScale);

	// Search
	ReachLocationRadius            = FMath::Max(0.f, ReachLocationRadius);
	SearchDefaultRadius            = FMath::Max(0.f, SearchDefaultRadius);
	SearchMaxDurationSeconds       = FMath::Max(0.f, SearchMaxDurationSeconds);
	SearchStepAcceptanceRadius     = FMath::Max(0.f, SearchStepAcceptanceRadius);
	SearchPointNavProjectionRadius = FMath::Max(0.f, SearchPointNavProjectionRadius);
	SearchRandomAngleDegrees       = FMath::Max(0.f, SearchRandomAngleDegrees);
	MaxSearchSteps                 = FMath::Max(1, MaxSearchSteps);
	SearchRadiusBaseScale          = FMath::Clamp(SearchRadiusBaseScale, 0.f, 2.f);
	SearchRadiusPerEchoVariation   = FMath::Clamp(SearchRadiusPerEchoVariation, 0.f, 2.f);
	SearchRadiusAggressionBonus    = FMath::Clamp(SearchRadiusAggressionBonus, 0.f, 2.f);
	SearchDurationBaseScale        = FMath::Clamp(SearchDurationBaseScale, 0.f, 2.f);
	SearchDurationPerEchoVariation = FMath::Clamp(SearchDurationPerEchoVariation, 0.f, 2.f);
	SearchDurationAggressionBonus  = FMath::Clamp(SearchDurationAggressionBonus, 0.f, 2.f);

	// Agitation — curiosity threshold must not exceed join threshold
	MomentumCellSize                    = FMath::Max(1.f, MomentumCellSize);
	AgitationFieldDecayPerSecond         = FMath::Max(0.f, AgitationFieldDecayPerSecond);
	EchoPersonalAgitationDecayPerSecond  = FMath::Max(0.f, EchoPersonalAgitationDecayPerSecond);
	HordeCuriosityThreshold              = FMath::Clamp(HordeCuriosityThreshold, 0.f, 1.f);
	AgitationJoinThreshold               = FMath::Clamp(AgitationJoinThreshold, HordeCuriosityThreshold, 1.f);
	NoiseAgitationAmountScale            = FMath::Max(0.f, NoiseAgitationAmountScale);
	CombatAgitationAmount                = FMath::Clamp(CombatAgitationAmount, 0.f, 1.f);
	HordePressureMoveDistance            = FMath::Max(0.f, HordePressureMoveDistance);
	HordePressureDirectionSmoothingAlpha = FMath::Clamp(HordePressureDirectionSmoothingAlpha, 0.f, 1.f);
	HordeOrientTargetDistanceCm          = FMath::Max(0.f, HordeOrientTargetDistanceCm);
	HeardSteerMinSpeedFraction           = FMath::Clamp(HeardSteerMinSpeedFraction, 0.f, 1.f);
	HordeSeparationOnlySpeedFraction     = FMath::Clamp(HordeSeparationOnlySpeedFraction, 0.f, 1.f);

	// Field diffusion + jitter shaping
	MomentumDiffusionRate      = FMath::Clamp(MomentumDiffusionRate, 0.f, 20.f);
	HordeDirectionJitterDegrees = FMath::Clamp(HordeDirectionJitterDegrees, 0.f, 180.f);

	// Crowd shaping (separation + approach jitter)
	HordeSeparationRadius       = FMath::Max(0.f, HordeSeparationRadius);
	HordeSeparationStrength     = FMath::Clamp(HordeSeparationStrength, 0.f, 4.f);
	HordeApproachJitterDegrees  = FMath::Clamp(HordeApproachJitterDegrees, 0.f, 180.f);
	HordeSeparationMaxNeighbors = FMath::Clamp(HordeSeparationMaxNeighbors, 1, 64);

	// Detachment
	DetachEdgeNeighborCount  = FMath::Clamp(DetachEdgeNeighborCount, 0, 32);
	DetachBackDot            = FMath::Clamp(DetachBackDot, -1.f, 1.f);
	DetachChanceEdgePerSec   = FMath::Clamp(DetachChanceEdgePerSec, 0.f, 5.f);
	DetachChanceBackPerSec   = FMath::Clamp(DetachChanceBackPerSec, 0.f, 5.f);
	DetachChanceRandomPerSec = FMath::Clamp(DetachChanceRandomPerSec, 0.f, 5.f);
	DetachDriftSpeed         = FMath::Max(0.f, DetachDriftSpeed);

	// Horde momentum
	MomentumBuildRate            = FMath::Clamp(MomentumBuildRate, 0.f, 10.f);
	MomentumDecayPerSecond       = FMath::Clamp(MomentumDecayPerSecond, 0.f, 10.f);
	MomentumPersistence          = FMath::Clamp(MomentumPersistence, 0.f, 1.f);
	MomentumMoverSpeedThreshold  = FMath::Max(0.f, MomentumMoverSpeedThreshold);
	MomentumRefMoverCount        = FMath::Clamp(MomentumRefMoverCount, 1.f, 200.f);
	MomentumAlignThreshold       = FMath::Clamp(MomentumAlignThreshold, 0.f, 1.f);
	MomentumCalmResistance       = FMath::Clamp(MomentumCalmResistance, 0.f, 10.f);
	MomentumMaxStrength          = FMath::Clamp(MomentumMaxStrength, 0.1f, 4.f);
	SoundImpulseRadius           = FMath::Max(0.f, SoundImpulseRadius);
	SoundImpulseSpeed            = FMath::Max(0.f, SoundImpulseSpeed);
	SoundImpulseStrengthScale    = FMath::Clamp(SoundImpulseStrengthScale, 0.f, 4.f);

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

	// Active pursuit (line-of-desire)
	ActivePursuitForwardSweepDistanceCm        = FMath::Max(0.f, ActivePursuitForwardSweepDistanceCm);
	ActivePursuitSweepRadiusCm                 = FMath::Max(0.f, ActivePursuitSweepRadiusCm);
	ActivePursuitGroundProjectionRadiusCm      = FMath::Max(0.f, ActivePursuitGroundProjectionRadiusCm);
	ActivePursuitStuckTimeSeconds              = FMath::Max(0.05f, ActivePursuitStuckTimeSeconds);
	ActivePursuitStuckProgressCm               = FMath::Max(0.f, ActivePursuitStuckProgressCm);
	ActivePursuitDirectInvestigateDistanceCm   = FMath::Max(0.f, ActivePursuitDirectInvestigateDistanceCm);
	ActivePursuitBlockerHeadOnDot              = FMath::Clamp(ActivePursuitBlockerHeadOnDot, 0.f, 1.f);

	// Barrier engagement
	BarrierEngageDistanceCm                = FMath::Max(0.f, BarrierEngageDistanceCm);
	BarrierReachThroughDistanceCm          = FMath::Max(0.f, BarrierReachThroughDistanceCm);
	BarrierAttackIntervalSeconds           = FMath::Max(0.05f, BarrierAttackIntervalSeconds);
	BarrierFirstAttackDelaySeconds         = FMath::Max(0.f, BarrierFirstAttackDelaySeconds);
	BarrierDamagePerHit                    = FMath::Max(0.f, BarrierDamagePerHit);
	BarrierPressurePerEcho                 = FMath::Max(0.f, BarrierPressurePerEcho);
	BarrierPressureDecayPerSecond          = FMath::Max(0.f, BarrierPressureDecayPerSecond);
	BarrierPressureDamageMultiplier        = FMath::Max(0.f, BarrierPressureDamageMultiplier);
	BarrierImpactLoudnessDb                = FMath::Clamp(BarrierImpactLoudnessDb, 0.f, 194.f);
	MaxBarrierEngageSecondsWithoutStimulus = FMath::Max(0.f, MaxBarrierEngageSecondsWithoutStimulus);
	FrustratedSearchDurationSeconds        = FMath::Max(0.f, FrustratedSearchDurationSeconds);
	FrustratedSearchRadiusCm               = FMath::Max(0.f, FrustratedSearchRadiusCm);
	FrustratedSearchHitIntervalSeconds     = FMath::Max(0.1f, FrustratedSearchHitIntervalSeconds);
	FrustratedSearchHitRangeMultiplier     = FMath::Clamp(FrustratedSearchHitRangeMultiplier, 1.f, 5.f);
	FrustratedShuffleMinRadiusFraction     = FMath::Clamp(FrustratedShuffleMinRadiusFraction, 0.f, 1.f);
	FrustratedShuffleAcceptRadiusCm        = FMath::Max(1.f, FrustratedShuffleAcceptRadiusCm);

	// Obstacle hooks
	ObstacleForwardTraceLength   = FMath::Max(0.f, ObstacleForwardTraceLength);
	ObstacleHandleTimeoutSeconds = FMath::Max(0.f, ObstacleHandleTimeoutSeconds);
	ObstacleSidestepDistance     = FMath::Max(0.f, ObstacleSidestepDistance);
	ObstacleForwardNudgeDistance = FMath::Max(0.f, ObstacleForwardNudgeDistance);

	// Combat — bite within grab range; release multiplier >= 1
	MeleeAttackRange       = FMath::Max(0.f, MeleeAttackRange);
	GrabRange              = FMath::Max(0.f, GrabRange);
	GrabCooldownSeconds    = FMath::Max(0.f, GrabCooldownSeconds);
	GrabReleaseMultiplier  = FMath::Max(1.f, GrabReleaseMultiplier);
	BiteRange              = FMath::Clamp(BiteRange, 0.f, GrabRange);
	BiteCooldownSeconds    = FMath::Max(0.f, BiteCooldownSeconds);
	MeleePullStrength      = FMath::Max(0.f, MeleePullStrength);
	MaxGrabHoldSeconds     = FMath::Clamp(MaxGrabHoldSeconds, 0.5f, 120.f);
	OrientTurnRateDegPerSec = FMath::Max(0.f, OrientTurnRateDegPerSec);

	// Combat resolution
	GrabFacingConeDegrees  = FMath::Clamp(GrabFacingConeDegrees, 0.f, 360.f);
	GrabMissChance         = FMath::Clamp(GrabMissChance, 0.f, 1.f);
	ScratchOnMissChance    = FMath::Clamp(ScratchOnMissChance, 0.f, 1.f);
	MeleePullSpeed         = FMath::Max(0.f, MeleePullSpeed);
	WeakGripPullScale      = FMath::Clamp(WeakGripPullScale, 0.f, 1.f);

	// Grab hold + struggle — weak break threshold must not exceed strong.
	GrabHoldSpeedScale           = FMath::Clamp(GrabHoldSpeedScale, 0.05f, 1.f);
	GrabPullInputScale           = FMath::Clamp(GrabPullInputScale, 0.f, 1.f);
	StruggleBreakThresholdStrong = FMath::Max(0.5f, StruggleBreakThresholdStrong);
	StruggleBreakThresholdWeak   = FMath::Clamp(StruggleBreakThresholdWeak, 0.5f, StruggleBreakThresholdStrong);
	StrugglePressGain            = FMath::Max(0.01f, StrugglePressGain);
	StruggleDecayPerSecond       = FMath::Max(0.f, StruggleDecayPerSecond);
	GrabBreakStaggerSeconds      = FMath::Max(0.f, GrabBreakStaggerSeconds);
	BiteLacerationChanceStrongGrip  = FMath::Clamp(BiteLacerationChanceStrongGrip, 0.f, 1.f);
	BiteDeepScratchChanceStrongGrip = FMath::Clamp(BiteDeepScratchChanceStrongGrip, 0.f, 1.f);
	BiteLacerationChanceWeakGrip    = FMath::Clamp(BiteLacerationChanceWeakGrip, 0.f, 1.f);
	BiteDeepScratchChanceWeakGrip   = FMath::Clamp(BiteDeepScratchChanceWeakGrip, 0.f, 1.f);
	ScratchTierScale       = FMath::Clamp(ScratchTierScale, 0.f, 1.f);
	DeepScratchTierScale   = FMath::Clamp(DeepScratchTierScale, 0.f, 1.f);
	LacerationTierScale    = FMath::Clamp(LacerationTierScale, 0.f, 1.f);
	LimpSpeedScale         = FMath::Clamp(LimpSpeedScale, 0.f, 1.f);
	CrawlSpeed             = FMath::Max(0.f, CrawlSpeed);

	// Shoulder barge
	BargeMinSpeed               = FMath::Max(0.f, BargeMinSpeed);
	BargeReferenceSpeed         = FMath::Max(1.f, BargeReferenceSpeed);
	BargeSuccessPowerThreshold  = FMath::Max(0.f, BargeSuccessPowerThreshold);
	BargeCenterEffectiveness    = FMath::Clamp(BargeCenterEffectiveness, 0.f, 1.f);
	BargeKnockbackSpeed         = FMath::Max(0.f, BargeKnockbackSpeed);
	BargeStaggerSeconds         = FMath::Max(0.f, BargeStaggerSeconds);
	BargePlayerSpeedLossAtCenter = FMath::Clamp(BargePlayerSpeedLossAtCenter, 0.f, 1.f);
	BargeCooldownSeconds        = FMath::Max(0.f, BargeCooldownSeconds);
	BargeMinApproachDot         = FMath::Clamp(BargeMinApproachDot, -1.f, 1.f);
	BargeKnockbackSideMix       = FMath::Clamp(BargeKnockbackSideMix, 0.f, 2.f);
	BargeKnockbackForwardMix    = FMath::Clamp(BargeKnockbackForwardMix, 0.f, 2.f);
	BargeKnockbackUpSpeed       = FMath::Max(0.f, BargeKnockbackUpSpeed);
	BargeMassRatioMin           = FMath::Clamp(BargeMassRatioMin, 0.01f, 1.f);
	BargeMassRatioMax           = FMath::Max(BargeMassRatioMin, BargeMassRatioMax);
	BargePowerCap               = FMath::Clamp(BargePowerCap, 1.f, 10.f);

	// Body condition — chances must sum to <= 1 (remainder = NoArms); strength min <= max.
	BodyHealthyChance        = FMath::Clamp(BodyHealthyChance, 0.f, 1.f);
	BodyMissingFingersChance = FMath::Clamp(BodyMissingFingersChance, 0.f, 1.f - BodyHealthyChance);
	BodyMissingHandChance    = FMath::Clamp(BodyMissingHandChance, 0.f, 1.f - BodyHealthyChance - BodyMissingFingersChance);
	BodyStrengthScalarMin    = FMath::Clamp(BodyStrengthScalarMin, 0.f, 2.f);
	BodyStrengthScalarMax    = FMath::Clamp(BodyStrengthScalarMax, BodyStrengthScalarMin, 2.f);

	// Grip / bite tables — deep-scratch threshold can't be below the scratch threshold.
	WeakGripPullTransmission     = FMath::Clamp(WeakGripPullTransmission, 0.f, 1.f);
	StrongGripChanceCap          = FMath::Clamp(StrongGripChanceCap, 0.f, 1.f);
	GrabMissingFingersMultiplier = FMath::Clamp(GrabMissingFingersMultiplier, 0.f, 1.f);
	GrabMissingHandMultiplier    = FMath::Clamp(GrabMissingHandMultiplier, 0.f, 1.f);
	ScratchFingerlessMultiplier  = FMath::Clamp(ScratchFingerlessMultiplier, 0.f, 1.f);
	BiteStrongScratchUpTo        = FMath::Clamp(BiteStrongScratchUpTo, 0.f, 1.f);
	BiteStrongDeepScratchUpTo    = FMath::Clamp(BiteStrongDeepScratchUpTo, BiteStrongScratchUpTo, 1.f);
	BiteWeakScratchUpTo          = FMath::Clamp(BiteWeakScratchUpTo, 0.f, 1.f);
	BiteWeakDeepScratchUpTo      = FMath::Clamp(BiteWeakDeepScratchUpTo, BiteWeakScratchUpTo, 1.f);

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

void UATR_EchoSettings::PostInitProperties()
{
	Super::PostInitProperties();
	// Clamp at CDO init so runtime callers can trust the CDO is valid without
	// re-validating from per-world BeginPlay. ValidateAndClamp is idempotent
	// (it only clamps to range), so the call is safe even if UE invokes
	// PostInitProperties more than once across the load pipeline.
	ValidateAndClamp();
}

#if WITH_EDITOR
void UATR_EchoSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ValidateAndClamp();
}
#endif
