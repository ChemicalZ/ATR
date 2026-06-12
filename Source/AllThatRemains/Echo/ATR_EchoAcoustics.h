// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ATR_EchoRuntimeTypes.h"

// ─────────────────────────────────────────────────────────────────────────────
// Echo sound propagation math.
//
// Sound is authored in REAL units: a stimulus carries LoudnessDb — sound pressure
// level (dB SPL) measured at the acoustic reference distance (default 1 m), like a
// spec sheet ("gunshot ≈ 140 dB @ 1 m", "footstep ≈ 45 dB", "door pounding ≈ 85 dB").
//
// Propagation model (free field):
//   ReceivedDb(d) = LoudnessDb
//                   - 20·log10(d / RefDistance)        (inverse-square spreading, −6 dB
//                                                       per doubling of distance)
//                   - AirAbsorption · (d / 100 m)      (atmospheric absorption, linear dB)
//
// An Echo hears the sound when ReceivedDb exceeds its hearing threshold; perceived
// intensity is normalized between the threshold and a saturation level for the
// existing urgency/agitation math.
//
// This file is the SINGLE seam for sound math. Occlusion/diffraction via sound ray
// tracing slots in here later: replace the free-field ComputeReceivedDb with a traced
// path-loss query, and every caller (world stimuli, perception, barrier impacts,
// debug emit) inherits it.
// ─────────────────────────────────────────────────────────────────────────────

namespace ATR_EchoAcoustics
{
	// Sound-typed stimuli propagate as dB SPL; smell/blood/agitation keep scalar fields.
	FORCEINLINE bool IsSoundType(EATR_StimulusType Type)
	{
		switch (Type)
		{
			case EATR_StimulusType::Noise:
			case EATR_StimulusType::DoorImpact:
			case EATR_StimulusType::WindowImpact:
			case EATR_StimulusType::Combat:
			case EATR_StimulusType::Scripted:
				return true;
			default:
				return false;
		}
	}

	// Free-field received level at DistanceCm from a source of SourceDb (SPL @ RefDistanceCm).
	FORCEINLINE float ComputeReceivedDb(float SourceDb, float DistanceCm, float RefDistanceCm, float AirAbsorptionDbPer100m)
	{
		const float Ref = FMath::Max(RefDistanceCm, 1.f);
		const float D   = FMath::Max(DistanceCm, Ref); // inside the reference distance, SPL = source level
		const float Spreading  = 20.f * FMath::LogX(10.f, D / Ref);
		const float Absorption = FMath::Max(0.f, AirAbsorptionDbPer100m) * (D / 10000.f); // 10000 cm = 100 m
		return SourceDb - Spreading - Absorption;
	}

	// Map a received level onto [0,1] for the urgency/agitation pipeline:
	// 0 at/below the hearing threshold, 1 at/above saturation.
	FORCEINLINE float DbToNormalizedStrength(float ReceivedDb, float ThresholdDb, float SaturationDb)
	{
		if (SaturationDb <= ThresholdDb)
			return (ReceivedDb >= ThresholdDb) ? 1.f : 0.f;
		return FMath::Clamp((ReceivedDb - ThresholdDb) / (SaturationDb - ThresholdDb), 0.f, 1.f);
	}

	// Maximum distance (cm) at which SourceDb is still audible above ThresholdDb.
	// Closed-form for pure spreading, then a few fixed-point refinements to account for
	// air absorption (no closed form exists once the linear term is added).
	FORCEINLINE float ComputeAudibleRadiusCm(float SourceDb, float ThresholdDb, float RefDistanceCm,
	                                         float AirAbsorptionDbPer100m, float MaxRangeCm)
	{
		if (SourceDb <= ThresholdDb) return 0.f;

		const float Ref = FMath::Max(RefDistanceCm, 1.f);
		float R = Ref * FMath::Pow(10.f, (SourceDb - ThresholdDb) / 20.f);
		R = FMath::Min(R, MaxRangeCm);

		if (AirAbsorptionDbPer100m > 0.f)
		{
			for (int32 It = 0; It < 4; ++It)
			{
				const float Budget = SourceDb - ThresholdDb - AirAbsorptionDbPer100m * (R / 10000.f);
				if (Budget <= 0.f) { R *= 0.5f; continue; }
				R = FMath::Min(Ref * FMath::Pow(10.f, Budget / 20.f), MaxRangeCm);
			}
		}
		return R;
	}

	// Legacy bridge: convert an old normalized [0,1] strength into a source dB level so
	// existing call sites that never set LoudnessDb keep working during migration.
	FORCEINLINE float LegacyStrengthToDb(float Strength01, float ThresholdDb, float SaturationDb)
	{
		return ThresholdDb + FMath::Clamp(Strength01, 0.f, 1.f) * (FMath::Max(SaturationDb, ThresholdDb) - ThresholdDb);
	}
}
