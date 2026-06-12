// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ATR_EchoCombatTypes.generated.h"

// Dedicated log category for combat RESOLUTION (grabs, grips, bites, wounds, pulls).
// Structured (UE_LOGFMT) so log data can be mined for tuning. Defined in ATR_EchoCombatTypes.cpp.
// Per-tick spam (pull) is VeryVerbose; resolved outcomes are Log/Verbose.
DECLARE_LOG_CATEGORY_EXTERN(LogATR_EchoCombat, Log, All);

// Physical condition of an echo's arms/hands. Logically gates what combat is possible:
// no arms = no grip at all, missing fingers = weak grip only, etc.
UENUM(BlueprintType)
enum class EATR_EchoArmCondition : uint8
{
	Healthy        UMETA(ToolTip = "Both arms intact - full grip range available."),
	MissingFingers UMETA(ToolTip = "Fingers gone - can only manage a weak grip, scratches less often."),
	MissingHand    UMETA(ToolTip = "One hand gone - grabs less reliably, weak grip only."),
	NoArms         UMETA(ToolTip = "No arms - physically cannot grab or scratch."),
};

// How firmly the echo is holding its target. Scales pull force and bite severity.
UENUM(BlueprintType)
enum class EATR_EchoGripType : uint8
{
	None,
	Weak,
	Strong,
};

// Resolved result category of a grab attempt. Decided entirely in C++.
UENUM(BlueprintType)
enum class EATR_GrabOutcome : uint8
{
	NoGrip        UMETA(ToolTip = "Physically incapable (no arms)."),
	BadAngle      UMETA(ToolTip = "Target outside the facing cone."),
	Missed        UMETA(ToolTip = "Whiffed - speed, clumsy dead hands, or plain bad luck. May still scratch."),
	GrabbedWeak   UMETA(ToolTip = "Grab took with a weak grip."),
	GrabbedStrong UMETA(ToolTip = "Grab took with a strong grip."),
};

// Wound tier inflicted by a resolved bite (or a failed-grab scratch).
UENUM(BlueprintType)
enum class EATR_BiteWound : uint8
{
	Miss,
	Scratch,
	DeepScratch,
	Laceration,
};

// Per-echo physical state, rolled deterministically on promotion (see InitFromSoA).
// Replicated so client visuals (missing-arm mesh variants) can match the server's logic.
USTRUCT(BlueprintType)
struct FATR_EchoBodyCondition
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Combat")
	EATR_EchoArmCondition ArmCondition = EATR_EchoArmCondition::Healthy;

	// Overall muscle power. Scales strong-grip chance and pull force.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Echo|Combat", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float StrengthScalar = 1.f;
};

// Full result of one grab attempt, returned to the melee task and passed to BP cosmetic hooks.
USTRUCT(BlueprintType)
struct FATR_GrabResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Combat")
	EATR_GrabOutcome Outcome = EATR_GrabOutcome::Missed;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Combat")
	EATR_EchoGripType Grip = EATR_EchoGripType::None;

	// True when a failed grab still raked fingers across the target (scratch consequence
	// already applied/logged by C++ - this is informational for FX).
	UPROPERTY(BlueprintReadOnly, Category = "Echo|Combat")
	bool bScratchedTarget = false;

	bool IsGrabbed() const
	{
		return Outcome == EATR_GrabOutcome::GrabbedWeak || Outcome == EATR_GrabOutcome::GrabbedStrong;
	}
};
