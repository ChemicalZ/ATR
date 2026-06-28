// Fill out your copyright notice in the Description page of Project Settings.

#include "SATR_StatusOverlayWidget.h"

#include "../ATR_Player.h"
#include "../../Health/ATR_HealthTypes.h"
#include "../../Health/Human/ATR_HumanHealthComponent.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Fonts/SlateFontInfo.h"
#include "Brushes/SlateColorBrush.h"

#define LOCTEXT_NAMESPACE "ATR_StatusOverlay"

namespace
{
	FSlateFontInfo LabelFont()   { return FCoreStyle::GetDefaultFontStyle("Regular", 10); }
	FSlateFontInfo HeadingFont() { return FCoreStyle::GetDefaultFontStyle("Bold", 11); }

	// Human-readable name for a condition enum value.
	FText ConditionName(EATR_ConditionType Type)
	{
		if (const UEnum* E = StaticEnum<EATR_ConditionType>())
		{
			return E->GetDisplayNameTextByValue(static_cast<int64>(Type));
		}
		return FText::AsNumber(static_cast<int32>(Type));
	}
}

bool SATR_StatusOverlayWidget::HasHealth() const
{
	return Health.IsValid();
}

TSharedRef<SWidget> SATR_StatusOverlayWidget::MakeHeading(const FString& Label)
{
	return SNew(STextBlock)
		.Text(FText::FromString(Label))
		.Font(HeadingFont())
		.ColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.85f, 1.f)));
}

TSharedRef<SWidget> SATR_StatusOverlayWidget::MakeBar(const FString& Label, TFunction<float()> Get01, const FLinearColor& Color)
{
	return SNew(SHorizontalBox)
		// Label.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 1.f)
		[
			SNew(SBox).WidthOverride(90.f)
			[
				SNew(STextBlock).Text(FText::FromString(Label)).Font(LabelFont())
			]
		]
		// Bar.
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(4.f, 1.f)
		[
			SNew(SBox).WidthOverride(140.f).HeightOverride(12.f)
			[
				SNew(SProgressBar)
				.Percent_Lambda([Get01]() { return FMath::Clamp(Get01(), 0.f, 1.f); })
				.FillColorAndOpacity(FSlateColor(Color))
			]
		]
		// Numeric readout.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 1.f)
		[
			SNew(SBox).WidthOverride(38.f)
			[
				SNew(STextBlock)
				.Font(LabelFont())
				.Justification(ETextJustify::Right)
				.Text_Lambda([this, Get01]()
				{
					if (!HasHealth()) { return FText::FromString(TEXT("--")); }
					return FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(FMath::Clamp(Get01(), 0.f, 1.f) * 100.f)));
				})
			]
		];
}

void SATR_StatusOverlayWidget::Construct(const FArguments& InArgs)
{
	// Program-lifetime brush (no dependence on editor/Starship styles in a package).
	static const FSlateColorBrush PanelBrush(FLinearColor(0.04f, 0.05f, 0.07f, 0.78f));

	// Color-coded getters resolve through the weak health ptr; null = 0 (empty bar).
	const auto V = [this](TFunction<float(const FATR_HumanVitals&)> Pick) -> TFunction<float()>
	{
		return [this, Pick]() { return Health.IsValid() ? Pick(Health->GetVitalsRef()) : 0.f; };
	};
	const auto Surv = [this](TFunction<float(const FATR_HumanSurvivalStats&)> Pick) -> TFunction<float()>
	{
		return [this, Pick]() { return Health.IsValid() ? Pick(Health->GetSurvivalStatsRef()) : 0.f; };
	};

	const FLinearColor Red(0.85f, 0.2f, 0.2f);
	const FLinearColor Blue(0.35f, 0.55f, 0.95f);
	const FLinearColor Amber(0.95f, 0.7f, 0.2f);
	const FLinearColor Green(0.4f, 0.8f, 0.4f);
	const FLinearColor Purple(0.7f, 0.45f, 0.85f);

	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Bottom)
	.Padding(FMargin(16.f, 16.f, 16.f, 24.f))
	[
		SNew(SBorder)
		.BorderImage(&PanelBrush)
		.Padding(FMargin(12.f, 10.f))
		[
			SNew(SVerticalBox)

			// Title — name + alive/conscious flag.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
				.Text_Lambda([this]()
				{
					if (!Health.IsValid()) { return LOCTEXT("NoHealth", "STATUS — (no body)"); }
					if (!Health->IsAlive())
					{
						return LOCTEXT("Dead", "STATUS — DECEASED");
					}
					return Health->IsConscious()
						? LOCTEXT("Alive", "STATUS — Conscious")
						: LOCTEXT("Unconscious", "STATUS — UNCONSCIOUS");
				})
			]

			// ── Vitals ──
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 2.f) [ MakeHeading(TEXT("VITALS")) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Blood"),         V([](const FATR_HumanVitals& X){ return X.BloodVolume01;   }), Red) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Pressure"),      V([](const FATR_HumanVitals& X){ return X.BloodPressure01; }), Red) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Oxygen"),        V([](const FATR_HumanVitals& X){ return X.Oxygenation01;   }), Blue) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Consciousness"), V([](const FATR_HumanVitals& X){ return X.Consciousness01; }), Blue) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Heart"),         V([](const FATR_HumanVitals& X){ return X.HeartFunction01; }), Red) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Brain"),         V([](const FATR_HumanVitals& X){ return X.BrainFunction01; }), Purple) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Pain"),          V([](const FATR_HumanVitals& X){ return X.Pain01;          }), Amber) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Shock"),         V([](const FATR_HumanVitals& X){ return X.Shock01;         }), Amber) ]

			// Core temperature isn't a 0..1 value — show as a degrees readout.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
			[
				SNew(STextBlock).Font(LabelFont())
				.Text_Lambda([this]()
				{
					if (!Health.IsValid()) { return FText::FromString(TEXT("Temp  --")); }
					return FText::FromString(FString::Printf(TEXT("Temp  %.1f°C"), Health->GetVitalsRef().CoreTemperatureC));
				})
			]

			// ── Survival ──
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 2.f) [ MakeHeading(TEXT("SURVIVAL")) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Hydration"), Surv([](const FATR_HumanSurvivalStats& X){ return X.Hydration01; }), Blue) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Calories"),  Surv([](const FATR_HumanSurvivalStats& X){ return X.Calories01;  }), Green) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Fatigue"),   Surv([](const FATR_HumanSurvivalStats& X){ return X.Fatigue01;   }), Amber) ]
			+ SVerticalBox::Slot().AutoHeight() [ MakeBar(TEXT("Immune"),    Surv([](const FATR_HumanSurvivalStats& X){ return X.ImmuneStrength01; }), Green) ]

			// ── Conditions (live list) ──
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 2.f) [ MakeHeading(TEXT("CONDITIONS")) ]
			+ SVerticalBox::Slot().AutoHeight().MaxHeight(120.f)
			[
				SNew(SBox).WidthOverride(280.f)
				[
					SNew(STextBlock)
					.Font(LabelFont())
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.8f, 0.6f)))
					.Text_Lambda([this]()
					{
						if (!Health.IsValid()) { return FText::FromString(TEXT("--")); }
						const TArray<FATR_Condition>& Conditions = Health->GetConditionsRef();
						if (Conditions.Num() == 0) { return LOCTEXT("None", "(none)"); }

						TArray<FString> Parts;
						Parts.Reserve(Conditions.Num());
						for (const FATR_Condition& C : Conditions)
						{
							Parts.Add(FString::Printf(TEXT("%s %d%%"),
								*ConditionName(C.Type).ToString(), FMath::RoundToInt(C.Severity01 * 100.f)));
						}
						return FText::FromString(FString::Join(Parts, TEXT("  •  ")));
					})
				]
			]

			// ── Grab / struggle ──
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Font(HeadingFont())
				.ColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.3f)))
				.Visibility_Lambda([this]()
				{
					return (Player.IsValid() && Player->IsGrabbed()) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					if (!Player.IsValid()) { return FText::GetEmpty(); }
					return FText::FromString(FString::Printf(TEXT("GRABBED x%d  —  STRUGGLE %d%%  (MASH Sprint)"),
						Player->GetGrabCount(), FMath::RoundToInt(Player->GetStruggleProgress01() * 100.f)));
				})
			]
		]
	];
}

void SATR_StatusOverlayWidget::SetSources(AATR_Player* InPlayer, UATR_HumanHealthComponent* InHealth)
{
	Player = InPlayer;
	Health = InHealth;
}

#undef LOCTEXT_NAMESPACE
