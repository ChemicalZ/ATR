// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_HealthTypes.h"

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
}
