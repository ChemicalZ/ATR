// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SATR_EchoDebugMapView;
class SCheckBox;
class UATR_EchoSubsystem;

// Full-screen debug overlay: a top toolbar of layer toggles + view buttons over a
// schematic top-down Echo map (SATR_EchoDebugMapView). Created/destroyed by
// UATR_EchoDebugMapSubsystem. Escape (or the Close button) requests dismissal.
class SATR_EchoDebugMapWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SATR_EchoDebugMapWidget) {}
		// Fired when the user asks to close the overlay (Close button / Escape).
		SLATE_EVENT(FSimpleDelegate, OnCloseRequested)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetSubsystem(UATR_EchoSubsystem* InSubsystem);

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	TSharedRef<SWidget> MakeToggle(const FString& Label, bool SATR_EchoDebugMapView::* Member, const FString& Tooltip = FString());
	TSharedRef<SWidget> MakeButton(const FString& Label, TFunction<void()> OnClick, const FString& Tooltip = FString());

	TSharedPtr<SATR_EchoDebugMapView> MapView;
	FSimpleDelegate OnCloseRequested;
};
