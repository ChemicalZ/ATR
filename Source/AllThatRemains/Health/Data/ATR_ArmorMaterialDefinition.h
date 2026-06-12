// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "../ATR_HealthTypes.h"
#include "ATR_ArmorMaterialDefinition.generated.h"

// Material/profile data for one protection layer (DA_ArmorMaterialDefinition).
// The clothing/equipment system composes worn layers per body region and
// answers IATR_DamageMitigationProvider queries from this data. Damage code
// itself never hardcodes armor logic.
//
// Resistances are 0..1 fractions of severity removed for that damage type.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_ArmorMaterialDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Armor")
	FText DisplayName;

	UPROPERTY(EditAnywhere, Category = "Armor|Resistance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlashResistance = 0.f;

	UPROPERTY(EditAnywhere, Category = "Armor|Resistance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PunctureResistance = 0.f;

	UPROPERTY(EditAnywhere, Category = "Armor|Resistance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BluntResistance = 0.f;

	UPROPERTY(EditAnywhere, Category = "Armor|Resistance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BiteResistance = 0.f;

	UPROPERTY(EditAnywhere, Category = "Armor|Resistance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BallisticResistance = 0.f;

	UPROPERTY(EditAnywhere, Category = "Armor|Resistance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BurnResistance = 0.f;

	// Fraction of contamination kept out of wounds under this layer.
	UPROPERTY(EditAnywhere, Category = "Armor|Resistance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ContaminationResistance = 0.f;

	// ── Environmental (read by the survival/temperature side) ──

	// Thermal insulation contribution, 0..1.
	UPROPERTY(EditAnywhere, Category = "Armor|Environment", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Insulation = 0.f;

	// How strongly the layer keeps water out (and how badly it holds it once
	// soaked is the clothing system's problem).
	UPROPERTY(EditAnywhere, Category = "Armor|Environment", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WaterResistance = 0.f;

	// Convenience lookup used by clothing-side mitigation composition.
	float GetResistance(const EATR_DamageType Type) const
	{
		switch (Type)
		{
		case EATR_DamageType::Slash:     return SlashResistance;
		case EATR_DamageType::Puncture:  return PunctureResistance;
		case EATR_DamageType::Blunt:
		case EATR_DamageType::Crush:
		case EATR_DamageType::Fall:      return BluntResistance;
		case EATR_DamageType::Bite:      return BiteResistance;
		case EATR_DamageType::Ballistic:
		case EATR_DamageType::Explosion: return BallisticResistance;
		case EATR_DamageType::Burn:      return BurnResistance;
		default:                         return 0.f;
		}
	}
};
