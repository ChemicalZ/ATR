// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "../ATR_HealthTypes.h"
#include "ATR_DamageTypeDefinition.generated.h"

// How one damage TYPE shapes the wounds it creates — the data form of the
// design doc's "damage behavior" table (slash bleeds, puncture goes deep,
// blunt fractures, bite contaminates, burn loses fluid...).
USTRUCT(BlueprintType)
struct FATR_DamageTypeRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "DamageType")
	EATR_DamageType DamageType = EATR_DamageType::None;

	// Bleed rate per unit severity (slash/ballistic high, blunt near zero —
	// blunt bleeding is internal, see InternalBleedFactor).
	UPROPERTY(EditAnywhere, Category = "DamageType", meta = (ClampMin = "0.0"))
	float BleedFactor = 0.f;

	// Chance-pressure for internal bleeding per unit severity (blunt/crush).
	UPROPERTY(EditAnywhere, Category = "DamageType", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InternalBleedFactor = 0.f;

	// Pain per unit severity (burns high).
	UPROPERTY(EditAnywhere, Category = "DamageType", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float PainFactor = 1.f;

	// Structural (bone/tendon) damage per unit severity (crush/blunt high).
	UPROPERTY(EditAnywhere, Category = "DamageType", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float StructuralFactor = 0.f;

	// How much penetration converts severity into organ depth (puncture/
	// ballistic high, slash low).
	UPROPERTY(EditAnywhere, Category = "DamageType", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float DepthFactor = 0.f;

	// Baseline infection risk per unit contamination (bite very high).
	UPROPERTY(EditAnywhere, Category = "DamageType", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float InfectionFactor = 0.5f;

	// Burn only: hydration loss per unit severity (fluid loss through skin).
	UPROPERTY(EditAnywhere, Category = "DamageType", meta = (ClampMin = "0.0"))
	float FluidLossFactor = 0.f;
};

// All damage-type behavior rows in one asset (DA_DamageTypeDefinition).
// Code defaults are used when no asset is assigned in UATR_HealthSettings.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_DamageTypeDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "DamageTypes", meta = (TitleProperty = "DamageType"))
	TArray<FATR_DamageTypeRow> Rows;

	const FATR_DamageTypeRow* FindRow(const EATR_DamageType Type) const
	{
		return Rows.FindByPredicate([Type](const FATR_DamageTypeRow& R) { return R.DamageType == Type; });
	}
};
