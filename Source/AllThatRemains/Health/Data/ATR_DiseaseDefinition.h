// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ATR_DiseaseDefinition.generated.h"

// A disease as DATA. The disease framework in UATR_HumanHealthComponent runs
// any number of these; there is no hardcoded illness logic.
//
// Symptom fields are "at full severity" magnitudes; actual application scales
// by the instance's Severity01 each slow tick.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_DiseaseDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// Stable id matched against FATR_DiseaseState::DiseaseId.
	UPROPERTY(EditAnywhere, Category = "Disease")
	FName DiseaseId;

	UPROPERTY(EditAnywhere, Category = "Disease")
	FText DisplayName;

	// Silent period before any symptom, seconds.
	UPROPERTY(EditAnywhere, Category = "Disease", meta = (ClampMin = "0.0"))
	float IncubationSeconds = 3600.f;

	// Stage01 progression per second, before immune/treatment modifiers.
	UPROPERTY(EditAnywhere, Category = "Disease", meta = (ClampMin = "0.0"))
	float BaseProgressionRate = 0.0001f;

	// How strongly immune strength pushes back (severity reduction per second
	// at ImmuneStrength 1).
	UPROPERTY(EditAnywhere, Category = "Disease", meta = (ClampMin = "0.0"))
	float ImmuneFightRate = 0.0002f;

	// ── Symptoms at full severity ──

	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Fever = 0.f;

	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Pain = 0.f;

	// General weakness: scales down strength/endurance-derived stats.
	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Weakness = 0.f;

	// Extra hydration drain per second (vomiting/sweating).
	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0"))
	float DehydrationRate = 0.f;

	// Suppression of ImmuneStrength while active.
	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ImmuneSuppression = 0.f;

	// Audible coughing: noise events for the stealth/Echo stimulus system
	// (linked later; stored here so the data is ready).
	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CoughNoise = 0.f;

	// Confusion: consciousness/perception penalty at full severity.
	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Confusion = 0.f;

	// Severity contribution to systemic Infection01 (sepsis pressure).
	// Zero for non-septic illnesses (colds, flu).
	UPROPERTY(EditAnywhere, Category = "Disease|Symptoms", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SepsisPressure = 0.f;
};
