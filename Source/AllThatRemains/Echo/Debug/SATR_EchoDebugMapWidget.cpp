// Fill out your copyright notice in the Description page of Project Settings.

#include "SATR_EchoDebugMapWidget.h"
#include "SATR_EchoDebugMapView.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Fonts/SlateFontInfo.h"
#include "Brushes/SlateColorBrush.h"

#define LOCTEXT_NAMESPACE "ATR_EchoDebugMap"

namespace
{
	FSlateFontInfo ToolbarFont() { return FCoreStyle::GetDefaultFontStyle("Regular", 12); }
	FSlateFontInfo TitleFont()   { return FCoreStyle::GetDefaultFontStyle("Bold", 15); }
}

TSharedRef<SWidget> SATR_EchoDebugMapWidget::MakeToggle(const FString& Label, bool SATR_EchoDebugMapView::* Member, const FString& Tooltip)
{
	return SNew(SCheckBox)
		.ToolTipText(Tooltip.IsEmpty() ? FText::GetEmpty() : FText::FromString(Tooltip))
		.IsChecked_Lambda([this, Member]()
		{
			return (MapView.IsValid() && (MapView.Get()->*Member)) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Member](ECheckBoxState NewState)
		{
			if (MapView.IsValid())
				(MapView.Get()->*Member) = (NewState == ECheckBoxState::Checked);
		})
		.Padding(FMargin(6.f, 4.f))
		[
			SNew(STextBlock).Text(FText::FromString(Label)).Font(ToolbarFont())
		];
}

TSharedRef<SWidget> SATR_EchoDebugMapWidget::MakeButton(const FString& Label, TFunction<void()> OnClick, const FString& Tooltip)
{
	return SNew(SButton)
		.ToolTipText(Tooltip.IsEmpty() ? FText::GetEmpty() : FText::FromString(Tooltip))
		.ContentPadding(FMargin(10.f, 5.f))
		.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
		[
			SNew(STextBlock).Text(FText::FromString(Label)).Font(ToolbarFont())
		];
}

void SATR_EchoDebugMapWidget::Construct(const FArguments& InArgs)
{
	OnCloseRequested = InArgs._OnCloseRequested;

	SAssignNew(MapView, SATR_EchoDebugMapView);

	auto Sep = []() { return SNew(SSpacer).Size(FVector2D(14.f, 1.f)); };

	// Program-lifetime solid brush for the toolbar background (avoids dependence on a
	// named editor/Starship style brush that may be absent in a packaged game).
	static const FSlateColorBrush PanelBrush(FLinearColor(0.06f, 0.06f, 0.08f, 0.96f));

	const FString AgitationTip =
		TEXT("Horde momentum field (built from echoes MOVING, not from sight).\n")
		TEXT("Heat = momentum strength: blue = weak, green/yellow = building, red = strong.\n")
		TEXT("Cyan arrows = the direction the local horde is flowing.");
	const FString AbstractTip =
		TEXT("Coarse Abstract-tier cells (far population simulation).\n")
		TEXT("Outlined cells with 'Pop N' = population; faint fill heat = cell agitation.\n")
		TEXT("Green arrows = migration pressure direction.");
	const FString IntentTip   = TEXT("Color each echo dot by its current EATR_EchoIntent (see legend).");
	const FString ArrowsTip   = TEXT("Per-cell momentum arrows: the direction the local horde is moving (what nearby echoes align to), plus abstract-cell migration arrows.");
	const FString FlowTip      = TEXT("Dense momentum field: samples horde momentum on a regular grid so you can see the whole flow, including cells momentum has spread into via diffusion.");
	const FString MoveTip     = TEXT("Show each echo's current movement-direction arrow (from velocity).");
	const FString TargetTip   = TEXT("Draw a line from each echo to its move target.\nRed = currently sees the player; orange = remembered/heard location.");
	const FString PlayersTip  = TEXT("Draw players as cyan triangles pointing in their facing direction.");

	ChildSlot
	[
		SNew(SVerticalBox)

		// ── Top toolbar ──
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(&PanelBrush)
			.Padding(FMargin(10.f, 8.f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2.f, 0.f, 16.f, 0.f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "ECHO DEBUG MAP"))
					.Font(TitleFont())
					.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.9f, 1.f)))
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Echoes"),    &SATR_EchoDebugMapView::bShowEchoes) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Intent"),    &SATR_EchoDebugMapView::bShowIntentColors, IntentTip) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Momentum"), &SATR_EchoDebugMapView::bShowAgitation, AgitationTip) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Abstract"),  &SATR_EchoDebugMapView::bShowAbstractCells, AbstractTip) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Arrows"),    &SATR_EchoDebugMapView::bShowArrows, ArrowsTip) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Flow"),      &SATR_EchoDebugMapView::bShowFlowField, FlowTip) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Move Dir"),  &SATR_EchoDebugMapView::bShowMoveArrows, MoveTip) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Targets"),   &SATR_EchoDebugMapView::bShowTargetLines, TargetTip) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeToggle(TEXT("Players"),   &SATR_EchoDebugMapView::bShowPlayers, PlayersTip) ]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ Sep() ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeButton(TEXT("Zoom +"), [this]() { if (MapView.IsValid()) MapView->ZoomBy(1.25f); }) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeButton(TEXT("Zoom -"), [this]() { if (MapView.IsValid()) MapView->ZoomBy(0.8f); }) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeButton(TEXT("Focus"),  [this]() { if (MapView.IsValid()) MapView->FocusOnEchoes(); }, TEXT("Frame the current active-echo cluster.")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)[ MakeButton(TEXT("Reset"),  [this]() { if (MapView.IsValid()) MapView->ResetView(); }, TEXT("Reset to the full world extent.")) ]

				+ SHorizontalBox::Slot().FillWidth(1.f)[ SNew(SSpacer) ]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f)
				[
					MakeButton(TEXT("Close [Esc]"), [this]() { OnCloseRequested.ExecuteIfBound(); })
				]
			]
		]

		// ── Map ──
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			MapView.ToSharedRef()
		]
	];
}

void SATR_EchoDebugMapWidget::SetSubsystem(UATR_EchoSubsystem* InSubsystem)
{
	if (MapView.IsValid())
		MapView->SetSubsystem(InSubsystem);
}

FReply SATR_EchoDebugMapWidget::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Escape)
	{
		OnCloseRequested.ExecuteIfBound();
		return FReply::Handled();
	}
	if (MapView.IsValid())
	{
		if (Key == EKeys::Add || Key == EKeys::Equals)      { MapView->ZoomBy(1.25f); return FReply::Handled(); }
		if (Key == EKeys::Subtract || Key == EKeys::Hyphen) { MapView->ZoomBy(0.8f);  return FReply::Handled(); }
		if (Key == EKeys::F)                                { MapView->FocusOnEchoes(); return FReply::Handled(); }
	}
	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
