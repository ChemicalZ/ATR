// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "../ATR_HealthTypes.h"
#include "ATR_ConditionDefinition.generated.h"

// Tuning row for one condition type.
USTRUCT(BlueprintType)
struct FATR_ConditionRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Condition")
	EATR_ConditionType Type = EATR_ConditionType::None;

	UPROPERTY(EditAnywhere, Category = "Condition")
	FText DisplayName;

	// Derived status markers (Pain, Fever, WeakGrip, Limping, ...) mirror a
	// vital/stat threshold and apply NO penalties of their own — the source
	// value is the single truth (design amendment #3). Pathologies simulate.
	UPROPERTY(EditAnywhere, Category = "Condition")
	bool bIsDerivedStatusMarker = false;

	// Pathologies only: default severity progression per second when untreated.
	UPROPERTY(EditAnywhere, Category = "Condition", meta = (EditCondition = "!bIsDerivedStatusMarker"))
	float DefaultProgressionRate = 0.f;

	// Pathologies only: pain contributed to aggregate Pain01 per unit severity.
	UPROPERTY(EditAnywhere, Category = "Condition", meta = (EditCondition = "!bIsDerivedStatusMarker", ClampMin = "0.0", ClampMax = "1.0"))
	float PainPerSeverity = 0.f;
};

// All condition tuning in one asset (DA_ConditionDefinition). Code defaults
// apply when no asset is assigned in UATR_HealthSettings.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_ConditionDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Conditions", meta = (TitleProperty = "Type"))
	TArray<FATR_ConditionRow> Rows;

	const FATR_ConditionRow* FindRow(const EATR_ConditionType Type) const
	{
		return Rows.FindByPredicate([Type](const FATR_ConditionRow& R) { return R.Type == Type; });
	}
};
