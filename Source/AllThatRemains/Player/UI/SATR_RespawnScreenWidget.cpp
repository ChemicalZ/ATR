// Fill out your copyright notice in the Description page of Project Settings.

#include "SATR_RespawnScreenWidget.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Fonts/SlateFontInfo.h"
#include "Brushes/SlateColorBrush.h"

#define LOCTEXT_NAMESPACE "ATR_RespawnScreen"

namespace
{
	// Human-readable cause-of-death line.
	FText DeathCauseText(EATR_DeathCause Cause)
	{
		switch (Cause)
		{
		case EATR_DeathCause::BrainFailure:        return LOCTEXT("BrainFailure", "Brain function failed");
		case EATR_DeathCause::CirculatoryCollapse: return LOCTEXT("Circulatory", "Bled out — circulatory collapse");
		case EATR_DeathCause::Hypoxia:             return LOCTEXT("Hypoxia", "Suffocated — oxygen starvation");
		case EATR_DeathCause::CardiacArrest:       return LOCTEXT("Cardiac", "Cardiac arrest");
		case EATR_DeathCause::Hypothermia:         return LOCTEXT("Hypothermia", "Froze to death");
		case EATR_DeathCause::Hyperthermia:        return LOCTEXT("Hyperthermia", "Overheated");
		case EATR_DeathCause::Sepsis:              return LOCTEXT("Sepsis", "Succumbed to infection");
		case EATR_DeathCause::Toxicity:            return LOCTEXT("Toxicity", "Poisoned");
		case EATR_DeathCause::CatastrophicTrauma:  return LOCTEXT("Trauma", "Catastrophic trauma");
		default:                                   return LOCTEXT("Unknown", "Cause unknown");
		}
	}
}

void SATR_RespawnScreenWidget::Construct(const FArguments& InArgs)
{
	OnRespawnRequested = InArgs._OnRespawnRequested;

	// Program-lifetime brushes — no editor style dependence in a package.
	static const FSlateColorBrush DimBrush(FLinearColor(0.0f, 0.0f, 0.0f, 0.78f));
	static const FSlateColorBrush PanelBrush(FLinearColor(0.08f, 0.02f, 0.02f, 0.92f));

	ChildSlot
	[
		// Full-screen dim.
		SNew(SBorder)
		.BorderImage(&DimBrush)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBorder)
			.BorderImage(&PanelBrush)
			.Padding(FMargin(48.f, 36.f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 8.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("YouDied", "YOU DIED"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 48))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.8f, 0.1f, 0.1f)))
				]

				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 0.f, 0.f, 28.f)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 16))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.8f, 0.78f)))
					.Text_Lambda([this]() { return DeathCauseText(DeathCause); })
				]

				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SBox).WidthOverride(260.f).HeightOverride(56.f)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked_Lambda([this]()
						{
							OnRespawnRequested.ExecuteIfBound();
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Respawn", "RESPAWN"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 22))
						]
					]
				]
			]
		]
	];
}

void SATR_RespawnScreenWidget::SetDeathCause(EATR_DeathCause Cause)
{
	DeathCause = Cause;
}

#undef LOCTEXT_NAMESPACE
