// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ATR_SubstanceDefinition.generated.h"

// One timed-substance description — the SAME asset class covers medicine,
// stimulants, sedatives, alcohol, and poison (poison is just a substance whose
// numbers hurt). Effect fields are "at strength 1 per unit dose"; runtime
// scales by FATR_SubstanceEffect::EvaluateStrength() * Dose.
//
// Examples: painkiller = PainRelief + small ReactionPenalty; antibiotics =
// InfectionSuppression; stimulant = Alertness + HydrationDrain + sleep harm;
// alcohol = Sedation + BalancePenalty + HydrationDrain + ToxicityPerDose.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_SubstanceDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Substance")
	FName SubstanceId;

	UPROPERTY(EditAnywhere, Category = "Substance")
	FText DisplayName;

	// ── Pharmacokinetics (seconds, at metabolism 1) ──

	UPROPERTY(EditAnywhere, Category = "Substance|Timing", meta = (ClampMin = "0.0"))
	float OnsetTime = 30.f;

	UPROPERTY(EditAnywhere, Category = "Substance|Timing", meta = (ClampMin = "1.0"))
	float PeakTime = 120.f;

	UPROPERTY(EditAnywhere, Category = "Substance|Timing", meta = (ClampMin = "1.0"))
	float Duration = 600.f;

	// ── Effects at strength 1, dose 1 ──

	// Subtracted from aggregate Pain01.
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PainRelief = 0.f;

	// Subtracted from Fever01 (antipyretics).
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FeverReduction = 0.f;

	// Scales DOWN infection growth and disease progression (antibiotics).
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InfectionSuppression = 0.f;

	// Counteracts sleep-debt penalties while active (stimulants).
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Alertness = 0.f;

	// Lowers panic/pain response and consciousness (sedatives, alcohol).
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Sedation = 0.f;

	// Reaction-time penalty while active (painkillers, sedatives, alcohol).
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ReactionPenalty = 0.f;

	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PerceptionPenalty = 0.f;

	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BalancePenalty = 0.f;

	// Extra hydration drain per second while active.
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0"))
	float HydrationDrain = 0.f;

	// Toxicity01 added per unit dose on administration. This is the overdose
	// path: stacking doses pushes Toxicity toward the fatal threshold.
	UPROPERTY(EditAnywhere, Category = "Substance|Effects", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ToxicityPerDose = 0.f;

	// ── Tolerance/dependency hooks (stored, NOT simulated in first pass) ──

	UPROPERTY(EditAnywhere, Category = "Substance|Dependency", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ToleranceGainPerDose = 0.f;

	UPROPERTY(EditAnywhere, Category = "Substance|Dependency", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DependencyGainPerDose = 0.f;
};
