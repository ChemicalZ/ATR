// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_HealthTypes.h"

#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY(LogATR_Health);

namespace ATR_Health
{
	bool IsBloodCompatible(const EATR_BloodType Donor, const EATR_BloodType Recipient)
	{
		// Encode ABO as bits: A = 1, B = 2 (O = 0, AB = 3); Rh+ = 4.
		const auto Encode = [](const EATR_BloodType Type) -> uint8
		{
			switch (Type)
			{
			case EATR_BloodType::ONeg:  return 0;
			case EATR_BloodType::OPos:  return 4;
			case EATR_BloodType::ANeg:  return 1;
			case EATR_BloodType::APos:  return 1 | 4;
			case EATR_BloodType::BNeg:  return 2;
			case EATR_BloodType::BPos:  return 2 | 4;
			case EATR_BloodType::ABNeg: return 1 | 2;
			case EATR_BloodType::ABPos: return 1 | 2 | 4;
			}
			return 0;
		};

		const uint8 D = Encode(Donor);
		const uint8 R = Encode(Recipient);

		// Compatible when the donor introduces no antigen (ABO or Rh) the
		// recipient lacks: donor bits must be a subset of recipient bits.
		return (D & ~R) == 0;
	}

	bool IsLimbRegion(const EATR_BodyRegion Region)
	{
		switch (Region)
		{
		case EATR_BodyRegion::LeftUpperArm:
		case EATR_BodyRegion::LeftForearm:
		case EATR_BodyRegion::LeftHand:
		case EATR_BodyRegion::RightUpperArm:
		case EATR_BodyRegion::RightForearm:
		case EATR_BodyRegion::RightHand:
		case EATR_BodyRegion::LeftThigh:
		case EATR_BodyRegion::LeftShin:
		case EATR_BodyRegion::LeftFoot:
		case EATR_BodyRegion::RightThigh:
		case EATR_BodyRegion::RightShin:
		case EATR_BodyRegion::RightFoot:
			return true;
		default:
			return false;
		}
	}

	EATR_BodyRegion RegionFromHitLocation(const AActor* Victim, const FVector& WorldHitLocation)
	{
		if (!Victim)
		{
			return EATR_BodyRegion::None;
		}

		// Resolve in actor space so facing doesn't matter. Height is normalized
		// over the actor's collision bounds (origin = center, extent = half size).
		FVector BoundsOrigin, BoundsExtent;
		Victim->GetActorBounds(true, BoundsOrigin, BoundsExtent);
		const float HalfHeight = FMath::Max(BoundsExtent.Z, 1.f);

		// 0 = feet, 1 = top of head.
		const float Height01 = FMath::Clamp(
			(WorldHitLocation.Z - (BoundsOrigin.Z - HalfHeight)) / (2.f * HalfHeight), 0.f, 1.f);

		// Left/right from the victim's local Y (his left = negative Y).
		const FVector Local = Victim->GetActorTransform().InverseTransformPosition(WorldHitLocation);
		const bool bLeft = Local.Y < 0.f;

		// Lateral hits on the torso band resolve to arms instead of the trunk.
		// "Wide" = beyond ~55% of the lateral extent.
		const float LateralExtent = FMath::Max(FMath::Max(BoundsExtent.X, BoundsExtent.Y), 1.f);
		const bool bWide = FMath::Abs(Local.Y) > LateralExtent * 0.55f;

		if (Height01 >= 0.88f) { return EATR_BodyRegion::Head; }
		if (Height01 >= 0.82f) { return EATR_BodyRegion::Neck; }
		if (Height01 >= 0.62f)
		{
			if (bWide) { return bLeft ? EATR_BodyRegion::LeftUpperArm : EATR_BodyRegion::RightUpperArm; }
			return EATR_BodyRegion::Chest;
		}
		if (Height01 >= 0.50f)
		{
			if (bWide) { return bLeft ? EATR_BodyRegion::LeftForearm : EATR_BodyRegion::RightForearm; }
			return EATR_BodyRegion::Abdomen;
		}
		if (Height01 >= 0.42f)
		{
			if (bWide) { return bLeft ? EATR_BodyRegion::LeftHand : EATR_BodyRegion::RightHand; }
			return EATR_BodyRegion::Pelvis;
		}
		if (Height01 >= 0.22f) { return bLeft ? EATR_BodyRegion::LeftThigh : EATR_BodyRegion::RightThigh; }
		if (Height01 >= 0.06f) { return bLeft ? EATR_BodyRegion::LeftShin : EATR_BodyRegion::RightShin; }
		return bLeft ? EATR_BodyRegion::LeftFoot : EATR_BodyRegion::RightFoot;
	}
}
