// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "../ATR_HealthTypes.h"
#include "ATR_BodyRegionDefinition.generated.h"

// Per-region tuning row. How fragile/bloody/organ-bearing a region is.
USTRUCT(BlueprintType)
struct FATR_BodyRegionRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Region")
	EATR_BodyRegion Region = EATR_BodyRegion::None;

	// Scales bleed rate of wounds here (neck high, foot low).
	UPROPERTY(EditAnywhere, Category = "Region", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float BleedScale = 1.f;

	// Scales pain of wounds here (hands/face high).
	UPROPERTY(EditAnywhere, Category = "Region", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float PainScale = 1.f;

	// How much of a deep wound's severity transfers to organ function here.
	// Head -> BrainFunction, Chest -> Heart/Lungs. Zero for limbs.
	UPROPERTY(EditAnywhere, Category = "Region|Organs", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BrainDamageScale = 0.f;

	UPROPERTY(EditAnywhere, Category = "Region|Organs", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HeartDamageScale = 0.f;

	UPROPERTY(EditAnywhere, Category = "Region|Organs", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LungDamageScale = 0.f;

	// Severity at/above which a single wound here can kill outright
	// (CatastrophicTrauma). 1 disables (limbs can't one-shot kill).
	UPROPERTY(EditAnywhere, Category = "Region", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CatastrophicSeverityThreshold = 1.f;
};

// All body region tuning in one asset (DA_BodyRegionDefinition).
// The component falls back to sensible code defaults when no asset is
// assigned in UATR_HealthSettings, so the system runs before content exists.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_BodyRegionDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Regions", meta = (TitleProperty = "Region"))
	TArray<FATR_BodyRegionRow> Regions;

	const FATR_BodyRegionRow* FindRow(const EATR_BodyRegion Region) const
	{
		return Regions.FindByPredicate([Region](const FATR_BodyRegionRow& R) { return R.Region == Region; });
	}
};
