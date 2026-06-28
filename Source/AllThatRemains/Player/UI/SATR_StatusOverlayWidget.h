// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class UATR_HumanHealthComponent;
class AATR_Player;
class SVerticalBox;

// Always-on player HUD overlay (bottom-left). Reads observable, replicated state
// off the locally controlled pawn's UATR_HumanHealthComponent every paint via
// TAttribute lambdas — no per-frame widget rebuilds, no ticking. Pure read; never
// mutates simulation. Rebind via SetSources when the pawn changes (respawn).
class SATR_StatusOverlayWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SATR_StatusOverlayWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Point the overlay at a (pawn, health) pair. Either may be null (shows dashes).
	void SetSources(AATR_Player* InPlayer, UATR_HumanHealthComponent* InHealth);

private:
	// One labelled progress bar bound to a 0..1 getter, with a live value readout.
	TSharedRef<class SWidget> MakeBar(const FString& Label, TFunction<float()> Get01, const FLinearColor& Color);

	// Section heading row.
	TSharedRef<class SWidget> MakeHeading(const FString& Label);

	bool HasHealth() const;

	TWeakObjectPtr<UATR_HumanHealthComponent> Health;
	TWeakObjectPtr<AATR_Player> Player;
};
