// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ATR_TreatmentDefinition.generated.h"

// Wound-level treatment categories. Systemic items (antibiotics, painkillers)
// are SUBSTANCES, not treatments — they go through UATR_SubstanceDefinition.
UENUM(BlueprintType)
enum class EATR_TreatmentType : uint8
{
	Bandage,      // reduces bleeding; dressing degrades and can turn dirty
	Tourniquet,   // stops limb bleeding outright; tissue risk over time
	Disinfectant, // lowers contamination/infection risk
	Suture,       // closes the wound: better healing, much lower reopen risk
	Splint        // restores partial function of fracture/sprain regions
};

// Data for one treatment item/technique (DA_TreatmentDefinition).
// Treatment modifies wound progression — it never instantly erases injuries.
UCLASS(BlueprintType)
class ALLTHATREMAINS_API UATR_TreatmentDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Treatment")
	EATR_TreatmentType Type = EATR_TreatmentType::Bandage;

	UPROPERTY(EditAnywhere, Category = "Treatment")
	FText DisplayName;

	// Quality of the item itself (improvised rag 0.2, sterile kit 1.0).
	// Combines with applier skill into the wound's TreatmentQuality01.
	UPROPERTY(EditAnywhere, Category = "Treatment", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BaseQuality01 = 0.5f;

	// Bandage/tourniquet: fraction of bleed rate removed at quality 1.
	UPROPERTY(EditAnywhere, Category = "Treatment", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BleedReduction01 = 0.f;

	// Disinfectant: fraction of contamination removed on application.
	UPROPERTY(EditAnywhere, Category = "Treatment", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ContaminationReduction01 = 0.f;

	// Suture/splint: multiplier on the wound's healing rate while applied.
	UPROPERTY(EditAnywhere, Category = "Treatment", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float HealRateMult = 1.f;

	// Bandage: cleanliness lost per second (dirty dressings stop protecting
	// and start contaminating).
	UPROPERTY(EditAnywhere, Category = "Treatment", meta = (ClampMin = "0.0"))
	float CleanlinessDecayRate = 0.f;
};
