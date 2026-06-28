// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_HealthSettings.h"

void UATR_HealthSettings::ValidateAndClamp()
{
	// Tick buckets must stay ordered: fast >= medium frequency, slow is a period.
	FastTickHz = FMath::Clamp(FastTickHz, 0.5f, 10.f);
	MediumTickHz = FMath::Clamp(MediumTickHz, 0.1f, FastTickHz);
	SlowTickSeconds = FMath::Clamp(SlowTickSeconds, 1.f, 300.f);

	// Fatal thresholds.
	FatalBrainFunction = FMath::Clamp(FatalBrainFunction, 0.f, 0.5f);
	CollapseBloodPressure = FMath::Clamp(CollapseBloodPressure, 0.f, 0.9f);
	CollapseOxygenation = FMath::Clamp(CollapseOxygenation, 0.f, 0.9f);
	FatalHeartFunction = FMath::Clamp(FatalHeartFunction, 0.f, 0.5f);
	CirculatoryGraceSeconds = FMath::Max(CirculatoryGraceSeconds, 1.f);
	HypoxiaGraceSeconds = FMath::Max(HypoxiaGraceSeconds, 1.f);
	TemperatureGraceSeconds = FMath::Max(TemperatureGraceSeconds, 1.f);
	FatalInfection = FMath::Clamp(FatalInfection, 0.5f, 1.f);
	FatalToxicity = FMath::Clamp(FatalToxicity, 0.5f, 1.f);

	// Temperature bands must stay ordered: fatal-low < hypo-onset < hyper-onset < fatal-high.
	FatalCoreTempLowC = FMath::Clamp(FatalCoreTempLowC, 15.f, 35.f);
	HypothermiaOnsetC = FMath::Clamp(HypothermiaOnsetC, FatalCoreTempLowC + 1.f, 37.f);
	HyperthermiaOnsetC = FMath::Clamp(HyperthermiaOnsetC, 37.f, 43.f);
	FatalCoreTempHighC = FMath::Clamp(FatalCoreTempHighC, HyperthermiaOnsetC + 1.f, 46.f);

	// Consciousness hysteresis: wake threshold must sit above the blackout threshold.
	UnconsciousThreshold = FMath::Clamp(UnconsciousThreshold, 0.f, 0.5f);
	WakeThreshold = FMath::Clamp(WakeThreshold, UnconsciousThreshold + 0.05f, 0.9f);

	// Sepsis must trigger before it can kill.
	SepsisThreshold = FMath::Clamp(SepsisThreshold, 0.f, FatalInfection);

	MinFingersForStrongGrab = FMath::Clamp(MinFingersForStrongGrab, 0, 5);
	EchoBrainDamageScale = FMath::Clamp(EchoBrainDamageScale, 0.f, 1.f);
	EchoDismemberScale = FMath::Clamp(EchoDismemberScale, 0.f, 4.f);
	EchoSpineDestroySeverity = FMath::Clamp(EchoSpineDestroySeverity, 0.f, 1.f);
	MaxStructuralDeltasPerUpdate = FMath::Clamp(MaxStructuralDeltasPerUpdate, 1, 1024);
	MaxPendingDeltas = FMath::Clamp(MaxPendingDeltas, 16, 65536);

	// Debug melee ray. Damage payload lives on DebugMeleeProfile asset.
	DebugMeleeRange = FMath::Clamp(DebugMeleeRange, 10.f, 100000.f);
}

void UATR_HealthSettings::PostInitProperties()
{
	Super::PostInitProperties();
	// Idempotent clamp at CDO init — runtime callers no longer need to re-validate.
	ValidateAndClamp();
}

#if WITH_EDITOR
void UATR_HealthSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ValidateAndClamp();
}
#endif
