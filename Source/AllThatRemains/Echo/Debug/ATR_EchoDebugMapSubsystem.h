// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ATR_EchoDebugMapSubsystem.generated.h"

class SATR_EchoDebugMapWidget;
class FATR_EchoDebugMapInputProcessor;
class IConsoleObject;

// Owns the Echo debug map overlay lifecycle. Created per game instance (non-shipping only).
//
// Three ways to open it, all routing to ToggleMap():
//   - Console command  ATR.EchoDebugMap
//   - Keybind          F8 (global Slate input pre-processor — no input asset wiring needed)
//   - Toolbar          the in-overlay top toolbar (toggles/zoom/focus/close once open)
UCLASS()
class ALLTHATREMAINS_API UATR_EchoDebugMapSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void ToggleMap();
	void OpenMap();
	void CloseMap();
	bool IsMapOpen() const { return MapWidget.IsValid(); }

private:
	// Static console-command sink — routes to the invoking world's subsystem instance.
	static void ToggleFromWorld(UWorld* World);

	TSharedPtr<SATR_EchoDebugMapWidget>          MapWidget;
	TSharedPtr<FATR_EchoDebugMapInputProcessor>  InputProcessor;
	IConsoleObject*                              ToggleCommand = nullptr; // non-null only if this instance registered it
};
