// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// Dedicated log category for the Echo AI execution layer (AIController, StateTree
// evaluators/tasks, and the subsystem-side intent/awareness code added in later phases).
//
// Mirrors the existing LogATR_EchoNet / LogATR_EchoRender categories. Defined once in
// ATR_EchoAIController.cpp. Use VeryVerbose for per-tick/per-lifecycle spam so the
// default Log verbosity stays clean at horde scale.
DECLARE_LOG_CATEGORY_EXTERN(LogATR_EchoAI, Log, All);
