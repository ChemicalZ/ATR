// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoBarrierInterface.h"

// Native default implementations — implementing classes override the
// *_Implementation functions (C++) or the events (Blueprint).

UATR_EchoBarrierDataAsset* IATR_EchoBarrier::GetEchoBarrierData_Implementation() const
{
	return nullptr; // per-type defaults apply
}

bool IATR_EchoBarrier::IsEchoPassable_Implementation() const
{
	return false; // still blocking
}

void IATR_EchoBarrier::OnEchoBarrierImpact_Implementation(AActor* /*EchoInstigator*/, float /*Damage*/, float /*Pressure*/)
{
	// No-op default. The barrier actor applies damage / FX / replication itself.
}
