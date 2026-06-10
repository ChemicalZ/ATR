// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateColorBrush.h"

class UATR_EchoSubsystem;

// ─────────────────────────────────────────────────────────────────────────────
// Schematic top-down debug map of the Echo world.
//
// Pure painter widget: each frame it reads the live UATR_EchoSubsystem and draws
//   - every active Echo as a dot (optionally colored by EATR_EchoIntent),
//   - the fine agitation field as a heat grid + weighted-direction arrows,
//   - the coarse Abstract population cells as outlined cells + pressure arrows.
//
// Supports mouse-wheel zoom (about the cursor) and click-drag pan. It never
// mutates subsystem state — it is a read-only visualization for tuning the horde
// agitation/pressure model at runtime.
// ─────────────────────────────────────────────────────────────────────────────
class SATR_EchoDebugMapView : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SATR_EchoDebugMapView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetSubsystem(UATR_EchoSubsystem* InSubsystem) { WeakSubsystem = InSubsystem; }

	// Layer toggles — driven by the toolbar checkboxes.
	bool bShowEchoes        = true;
	bool bShowIntentColors  = true;
	bool bShowAgitation     = true;
	bool bShowAbstractCells = true;
	bool bShowArrows        = true;  // per-cell pressure-flow (gradient) direction arrows
	bool bShowFlowField     = false; // dense gradient flow field sampled on a regular grid
	bool bShowPlayers       = true;  // player markers (triangles)
	bool bShowMoveArrows    = false; // per-echo current movement-direction arrow
	bool bShowTargetLines   = false; // line from echo to its current move target

	// Click-to-emit sound (debug map). When bEmitSoundMode is on, left-click emits a stimulus.
	bool  bEmitSoundMode = false;
	float SoundStrength  = 1.0f;
	float SoundRadius    = 4000.f;
	uint8 SoundType      = 0; // EATR_StimulusType ordinal (0 = Noise)

	// View controls invoked by the toolbar buttons.
	void ZoomBy(float Factor);                 // multiply zoom about the view center
	void ResetView();                          // frame the whole world extent
	void FocusOnEchoes();                      // frame the current active-echo AABB

	// SWidget interface
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(640.f, 480.f); }

	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	UATR_EchoSubsystem* GetSubsystem() const;

	// Emit a stimulus at a world XY using the current Sound* params (called on click in emit mode).
	void EmitSoundAtWorld(const FVector2D& WorldXY);

	// World(cm) ⇄ local-screen(px) transforms. +X world → screen right, +Y world → screen up.
	FVector2D WorldToScreen(const FVector2D& World, const FVector2D& LocalSize) const;
	FVector2D ScreenToWorld(const FVector2D& Screen, const FVector2D& LocalSize) const;

	void EnsureInitialized(const FVector2D& LocalSize) const;
	static FLinearColor IntentColor(uint8 Intent);
	static FLinearColor HeatColor(float T); // T in [0,1] → blue→green→yellow→red

	// View state (mutable so const OnPaint can lazy-init / cache nothing else mutates).
	mutable FVector2D ViewCenterWorld = FVector2D::ZeroVector; // cm
	mutable float     PixelsPerCm     = 0.001f;                // zoom
	mutable bool      bViewInitialized = false;

	// Pan tracking.
	bool      bPanning = false;
	FVector2D LastPanScreenPos = FVector2D::ZeroVector;

	UATR_EchoSubsystem* WeakSubsystem = nullptr;

	// Recent emit pulses for visual feedback (expanding fading rings).
	struct FEmitMarker { FVector2D World; double Time; float Radius; };
	mutable TArray<FEmitMarker> RecentEmits;

	// Solid white 1x1 brush; MakeBox tint supplies the actual color.
	FSlateColorBrush FillBrush = FSlateColorBrush(FLinearColor::White);
};
