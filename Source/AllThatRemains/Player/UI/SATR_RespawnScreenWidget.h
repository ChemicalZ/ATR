// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "../../Health/ATR_HealthTypes.h"

// Full-screen death overlay: "YOU DIED", the cause of death, and a Respawn
// button. The button fires OnRespawnRequested; the owning PlayerController turns
// that into a server RPC. Added to / removed from the viewport by the controller.
class SATR_RespawnScreenWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SATR_RespawnScreenWidget) {}
		// Clicked the Respawn button.
		SLATE_EVENT(FSimpleDelegate, OnRespawnRequested)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Set the death-cause line shown above the button.
	void SetDeathCause(EATR_DeathCause Cause);

	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	FSimpleDelegate OnRespawnRequested;
	EATR_DeathCause DeathCause = EATR_DeathCause::None;
};
