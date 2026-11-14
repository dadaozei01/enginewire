// Niagara simulation control for verifying effects step by step (UE 5.8):
//
// - set_niagara_sim_target (and enable_gpu_simulation's target) switches an emitter between CPU and GPU the way the
//   emitter's details panel does (PostEditChangeVersionedProperty on SimTarget), then compiles and reports per script.
//   A CPU emitter can be read with get_niagara_particle_data; a GPU one cannot.
// - step_niagara_simulation advances a placed component by whole frames and pauses it afterwards. A paused system is
//   skipped by the world tick, so the editor's realtime viewport no longer moves it between two calls: consecutive
//   steps are adjacent frames. The same call can take the screenshot, of the paused frame.
// - set_niagara_simulation_stage sets a simulation stage's iteration count and enabled state.
#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"

#if MCP_HAS_NIAGARA_INPUT_OVERRIDES && ENGINE_MINOR_VERSION >= 8

#include "Editor.h"
#include "EngineUtils.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "Safety/McpSafeOperations.h"

namespace McpNiagaraAuthoringHandlers
{
UNiagaraComponent* ResolveWorldNiagaraComponent(FActionContext& Context, const FString& ActorName, const FString& ComponentName,
    UWorld*& OutWorld, AActor*& OutActor)
{
    OutWorld = GEditor ? (GEditor->PlayWorld ? GEditor->PlayWorld.Get() : GEditor->GetEditorWorldContext().World()) : nullptr;
    OutActor = nullptr;
    if (!OutWorld || OutWorld->bIsTearingDown)
    {
        Context.SendError(TEXT("No usable editor/PIE world."), TEXT("WORLD_NOT_FOUND"));
        return nullptr;
    }
    TArray<FString> ActorNames;
    for (TActorIterator<AActor> It(OutWorld); It; ++It)
    {
        ActorNames.Add(It->GetActorLabel());
        if (It->GetName().Equals(ActorName, ESearchCase::IgnoreCase) || It->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase)
            || It->GetPathName().Equals(ActorName, ESearchCase::IgnoreCase))
        {
            if (OutActor)
            {
                Context.SendError(TEXT("actorName is ambiguous in the selected world; use the actor's unique object name or full object path."), TEXT("AMBIGUOUS_ACTOR"));
                return nullptr;
            }
            OutActor = *It;
        }
    }
    if (!OutActor)
    {
        Context.SendError(FString::Printf(TEXT("Actor '%s' not found in %s; available actor labels: %s."), *ActorName, *OutWorld->GetName(), *FString::Join(ActorNames, TEXT(", "))), TEXT("ACTOR_NOT_FOUND"));
        return nullptr;
    }
    TInlineComponentArray<UNiagaraComponent*> Components(OutActor);
    TArray<FString> Names;
    UNiagaraComponent* Found = nullptr;
    for (UNiagaraComponent* Each : Components)
    {
        Names.Add(Each->GetName());
        if (!ComponentName.IsEmpty() && Each->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
        {
            Found = Each;
        }
    }
    if (ComponentName.IsEmpty() && Components.Num() == 1)
    {
        Found = Components[0];
    }
    if (!Found)
    {
        const bool bNone = Components.IsEmpty();
        Context.SendError(FString::Printf(TEXT("%s on '%s'; its Niagara components: %s."),
            ComponentName.IsEmpty() ? TEXT("Expected exactly one Niagara component (or componentName)") : *FString::Printf(TEXT("No Niagara component '%s'"), *ComponentName),
            *ActorName, bNone ? TEXT("none") : *FString::Join(Names, TEXT(", "))),
            bNone || !ComponentName.IsEmpty() ? TEXT("COMPONENT_NOT_FOUND") : TEXT("AMBIGUOUS_COMPONENT"));
        return nullptr;
    }
    return Found;
}

static const TCHAR* SimTargetText(ENiagaraSimTarget Target)
{
    return Target == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU");
}

// Compiles the system, writes compileOk and compiledScripts, and saves only a clean compile (when save is on).
static bool CompileAndMaybeSave(FActionContext& Context, UNiagaraSystem* System)
{
    TArray<TSharedPtr<FJsonValue>> Reports;
    const bool bCompileOk = CompileSystemAndCollect(System, Reports, [](const FNiagaraCompileEvent&) {});
    Context.Result->SetBoolField(TEXT("compileOk"), bCompileOk);
    Context.Result->SetArrayField(TEXT("compiledScripts"), Reports);
    System->MarkPackageDirty();
    const bool bSaved = bCompileOk && Context.bSave && McpSafeOperations::McpSafeAssetSave(System);
    Context.Result->SetBoolField(TEXT("saved"), bSaved);
    McpHandlerUtils::AddVerification(Context.Result, System);
    return bCompileOk;
}

bool SetEmitterSimTarget(FActionContext& Context)
{
    FString TargetText = GetJsonStringField(Context.Payload, TEXT("target"), TEXT("GPU"));
    ENiagaraSimTarget Target;
    if (TargetText.Equals(TEXT("GPU"), ESearchCase::IgnoreCase) || TargetText.Equals(TEXT("GPUComputeSim"), ESearchCase::IgnoreCase))
    {
        Target = ENiagaraSimTarget::GPUComputeSim;
    }
    else if (TargetText.Equals(TEXT("CPU"), ESearchCase::IgnoreCase) || TargetText.Equals(TEXT("CPUSim"), ESearchCase::IgnoreCase))
    {
        Target = ENiagaraSimTarget::CPUSim;
    }
    else
    {
        Context.SendError(FString::Printf(TEXT("target must be CPU or GPU; got '%s'. Nothing was changed."), *TargetText), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!LoadSystemAndEmitter(Context, System, Handle))
    {
        return true;
    }
    const FVersionedNiagaraEmitter Versioned = Handle->GetInstance();
    FVersionedNiagaraEmitterData* EmitterData = Versioned.GetEmitterData();
    const FProperty* SimTargetProperty = FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(TEXT("SimTarget"));
    if (!Versioned.Emitter || !EmitterData || !SimTargetProperty)
    {
        Context.SendError(TEXT("This emitter has no editable emitter data (a stateless emitter has no CPU/GPU target). Nothing was changed."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    const ENiagaraSimTarget Previous = EmitterData->SimTarget;
    Context.Result->SetStringField(TEXT("previousSimTarget"), SimTargetText(Previous));

    // fixedBoundsEnabled/deterministicEnabled set the system's bFixedBounds/bDeterminism when given (by reflection:
    // their C++ access differs across engine versions); the reply reads both back.
    System->Modify();
    const auto ApplySystemFlag = [&](const TCHAR* Field, const TCHAR* PropertyName)
    {
        const FBoolProperty* Property = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), PropertyName);
        bool bValue = false;
        if (Property && Context.Payload->TryGetBoolField(Field, bValue))
        {
            Property->SetPropertyValue_InContainer(System, bValue);
        }
        Context.Result->SetBoolField(Field, Property && Property->GetPropertyValue_InContainer(System));
    };
    ApplySystemFlag(TEXT("fixedBoundsEnabled"), TEXT("bFixedBounds"));
    ApplySystemFlag(TEXT("deterministicEnabled"), TEXT("bDeterminism"));

    if (Previous != Target)
    {
        // As the details panel: the emitter's own change handler marks its graph unsynchronized, so the scripts are
        // rebuilt for the new target (a GPU compute script, or none) instead of reusing the old target's results.
        Versioned.Emitter->Modify();
        EmitterData->SimTarget = Target;
        FPropertyChangedEvent Event(const_cast<FProperty*>(SimTargetProperty), EPropertyChangeType::ValueSet);
        Versioned.Emitter->PostEditChangeVersionedProperty(Event, Versioned.Version);
    }
    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (Target == ENiagaraSimTarget::GPUComputeSim && !Context.Result->GetBoolField(TEXT("fixedBoundsEnabled"))
        && EmitterData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Dynamic)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("GPU emitters cannot compute dynamic bounds: set fixed bounds on the emitter or pass fixedBoundsEnabled true, or particles are culled when the bounds leave the view.")));
    }
    const bool bCompileOk = CompileAndMaybeSave(Context, System);
    Context.Result->SetStringField(TEXT("simTarget"), SimTargetText(EmitterData->SimTarget));
    Context.Result->SetBoolField(TEXT("gpuEnabled"), EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim);
    Context.Result->SetBoolField(TEXT("changed"), Previous != EmitterData->SimTarget);
    if (Warnings.Num() > 0)
    {
        Context.Result->SetArrayField(TEXT("warnings"), Warnings);
    }
    if (Target == ENiagaraSimTarget::CPUSim)
    {
        Context.Result->SetStringField(TEXT("next"), TEXT("Placed components restart on the new scripts; read particles with get_niagara_particle_data (advanceTicks > 0 for a component that has not ticked yet)."));
    }
    Context.SendSuccess(true, bCompileOk
        ? FString::Printf(TEXT("Emitter '%s' simulates on the %s now."), *Context.EmitterName, SimTargetText(EmitterData->SimTarget))
        : FString::Printf(TEXT("Emitter '%s' was switched to %s but the system compiled with errors (see compiledScripts); not saved."), *Context.EmitterName, SimTargetText(EmitterData->SimTarget)));
    return true;
}

static bool SetSimulationStage(FActionContext& Context)
{
    const FString StageName = GetJsonStringField(Context.Payload, TEXT("stageName"));
    bool bEnabled = true;
    const bool bHasEnabled = Context.Payload->TryGetBoolField(TEXT("enabled"), bEnabled);
    double IterationsNumber = 0;
    const bool bHasIterations = Context.Payload->TryGetNumberField(TEXT("numIterations"), IterationsNumber);
    if (StageName.IsEmpty() || (!bHasEnabled && !bHasIterations)
        || (bHasIterations && (IterationsNumber < 1 || IterationsNumber > 1000 || IterationsNumber != FMath::FloorToDouble(IterationsNumber))))
    {
        Context.SendError(TEXT("Require stageName and at least one of numIterations (integer 1..1000) or enabled (boolean). Nothing was changed."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!LoadSystemAndEmitter(Context, System, Handle))
    {
        return true;
    }
    const FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
    UNiagaraSimulationStageBase* Stage = nullptr;
    TArray<FString> StageNames;
    if (Data)
    {
        for (UNiagaraSimulationStageBase* Each : Data->GetSimulationStages())
        {
            if (Each)
            {
                StageNames.Add(Each->SimulationStageName.ToString());
                if (Each->SimulationStageName.ToString().Equals(StageName, ESearchCase::IgnoreCase))
                {
                    Stage = Each;
                }
            }
        }
    }
    if (!Stage)
    {
        Context.SendError(FString::Printf(TEXT("Emitter '%s' has no simulation stage '%s'; its stages: [%s]. Add one with add_niagara_module moduleKind=simulation_stage. Nothing was changed."),
            *Context.EmitterName, *StageName, *FString::Join(StageNames, TEXT(", "))), TEXT("STAGE_NOT_FOUND"));
        return true;
    }
    UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
    if (bHasIterations && !Generic)
    {
        Context.SendError(FString::Printf(TEXT("'%s' is a %s, which has no iteration count. Nothing was changed."), *StageName, *Stage->GetClass()->GetName()), TEXT("NOT_SUPPORTED"));
        return true;
    }
    if (bHasIterations && !Generic->NumIterations.ResolvedParameter.GetName().IsNone())
    {
        Context.SendError(FString::Printf(TEXT("'%s' takes its iteration count from %s, so a fixed number would be ignored. Unbind Num Iterations in the stage's details first. Nothing was changed."),
            *StageName, *Generic->NumIterations.ResolvedParameter.GetName().ToString()), TEXT("NUM_ITERATIONS_BOUND"));
        return true;
    }

    System->Modify();
    Stage->Modify();
    if (bHasIterations)
    {
        const int32 Iterations = static_cast<int32>(IterationsNumber);
        Generic->NumIterations.SetDefaultValueEditorOnly(Iterations);
        // The stage's own change handler requests the emitter recompile for NumIterations, as the details panel does.
        FPropertyChangedEvent Event(UNiagaraSimulationStageGeneric::StaticClass()->FindPropertyByName(TEXT("NumIterations")), EPropertyChangeType::ValueSet);
        Stage->PostEditChangeProperty(Event);
    }
    if (bHasEnabled)
    {
        Stage->SetEnabled(bEnabled);
    }
    const bool bCompileOk = CompileAndMaybeSave(Context, System);
    Context.Result->SetStringField(TEXT("stageName"), Stage->SimulationStageName.ToString());
    Context.Result->SetBoolField(TEXT("enabled"), Stage->bEnabled);
    if (Generic && Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
    {
        Context.Result->SetNumberField(TEXT("numIterations"), Generic->NumIterations.GetDefaultValue<int32>());
    }
    Context.SendSuccess(true, bCompileOk ? TEXT("Simulation stage updated.") : TEXT("Simulation stage updated, but the system compiled with errors (see compiledScripts); not saved."));
    return true;
}

static void AddEmitterCounts(const TSharedPtr<FJsonObject>& Result, UNiagaraComponent* Component)
{
    const FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
    const FNiagaraSystemInstance* Instance = Controller.IsValid() && Controller->IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
    if (!Instance)
    {
        return;
    }
    TArray<TSharedPtr<FJsonValue>> Emitters;
    for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Emitter->GetEmitterHandle().GetName().ToString());
        Entry->SetStringField(TEXT("simTarget"), SimTargetText(Emitter->GetSimTarget()));
        // A GPU emitter's count is the last one read back from the GPU: it can trail the simulation by a frame or two.
        Entry->SetNumberField(TEXT("numParticles"), Emitter->GetNumParticles());
        Emitters.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("emitters"), Emitters);
}

static bool StepSimulation(FActionContext& Context)
{
    if (!IsInGameThread())
    {
        Context.SendError(TEXT("Stepping must run on the game thread."), TEXT("INVALID_THREAD"));
        return true;
    }
    const FString ActorName = GetJsonStringField(Context.Payload, TEXT("actorName"));
    const FString ComponentName = GetJsonStringField(Context.Payload, TEXT("componentName"));
    double FramesNumber = 1, Delta = 1.0 / 60.0;
    Context.Payload->TryGetNumberField(TEXT("frames"), FramesNumber);
    Context.Payload->TryGetNumberField(TEXT("deltaTime"), Delta);
    const bool bPause = GetJsonBoolField(Context.Payload, TEXT("pause"), true);
    const bool bReset = GetJsonBoolField(Context.Payload, TEXT("reset"), false);
    if (ActorName.IsEmpty() || !FMath::IsFinite(FramesNumber) || FramesNumber < 0 || FramesNumber > 10000 || FramesNumber != FMath::FloorToDouble(FramesNumber)
        || !FMath::IsFinite(Delta) || Delta <= SMALL_NUMBER || Delta > 1.0)
    {
        Context.SendError(TEXT("Require actorName; frames: integer 0..10000 (default 1); deltaTime: > 0 and <= 1 second (default 1/60). Nothing was changed."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    const int32 Frames = static_cast<int32>(FramesNumber);
    TSharedPtr<FJsonObject> ShotOptions;
    if (const TSharedPtr<FJsonValue> Shot = Context.Payload->TryGetField(TEXT("screenshot")))
    {
        const TSharedPtr<FJsonObject>* ShotObject = nullptr;
        bool bShot = false;
        if (Shot->TryGetObject(ShotObject) && ShotObject)
        {
            ShotOptions = MakeShared<FJsonObject>();
            ShotOptions->Values = (*ShotObject)->Values;
        }
        else if (Shot->TryGetBool(bShot))
        {
            if (bShot)
            {
                ShotOptions = MakeShared<FJsonObject>();
            }
        }
        else
        {
            Context.SendError(TEXT("screenshot is true or an object of control_editor screenshot options (mode, resolution, location, rotation, filename, keepFile, returnBase64). Nothing was changed."), TEXT("INVALID_ARGUMENT"));
            return true;
        }
    }

    UWorld* World = nullptr;
    AActor* Actor = nullptr;
    UNiagaraComponent* Component = ResolveWorldNiagaraComponent(Context, ActorName, ComponentName, World, Actor);
    if (!Component)
    {
        return true;
    }
    if (bReset)
    {
        // Restarts from age 0; a reset instance is active and unpaused.
        Component->ResetSystem();
    }
    FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
    if (!Controller.IsValid() || !Controller->IsValid())
    {
        Context.SendError(TEXT("The Niagara component is not running (never activated, finished, or its system still compiling). Pass reset true to start it."), TEXT("SYSTEM_NOT_ACTIVE"));
        return true;
    }
    const bool bWasPaused = Controller->IsPaused();
    const double AgeBefore = Controller->GetAge();
    if (Frames > 0)
    {
        // AdvanceSimulation skips a paused instance, so a component paused by the last step is let go for these
        // frames only: nothing ticks in between, the call is synchronous.
        if (bWasPaused)
        {
            Component->SetPaused(false);
        }
        Component->AdvanceSimulation(Frames, static_cast<float>(Delta));
    }
    Controller = Component->GetSystemInstanceController();
    const bool bRunning = Controller.IsValid() && Controller->IsValid();
    const double Age = bRunning ? Controller->GetAge() : 0.0;
    const bool bComplete = Component->IsComplete();
    if (bRunning)
    {
        Component->SetPaused(bPause);
    }
    const bool bPaused = bRunning && Controller->IsPaused();

    Context.Result->SetStringField(TEXT("world"), World->GetPathName());
    Context.Result->SetStringField(TEXT("worldType"), World->WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("Editor"));
    Context.Result->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Context.Result->SetStringField(TEXT("componentName"), Component->GetName());
    Context.Result->SetNumberField(TEXT("frames"), Frames);
    Context.Result->SetNumberField(TEXT("deltaTime"), Delta);
    Context.Result->SetNumberField(TEXT("ageBefore"), AgeBefore);
    Context.Result->SetNumberField(TEXT("age"), Age);
    Context.Result->SetBoolField(TEXT("paused"), bPaused);
    Context.Result->SetBoolField(TEXT("complete"), bComplete);
    AddEmitterCounts(Context.Result, Component);
    if (Frames > 0 && Age <= AgeBefore)
    {
        Context.Subsystem->SendAutomationResponse(Context.RequestingSocket, Context.RequestId, false,
            bComplete ? TEXT("The Niagara system did not advance: it has finished. Pass reset true to play it again from age 0.")
                      : TEXT("The Niagara system did not advance (not running, or its system still compiling). Pass reset true, or step again in a moment."),
            Context.Result, TEXT("NOT_ADVANCED"));
        return true;
    }
    if (bPause && !bPaused)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("The component could not be paused (it is not fully active, e.g. finished or still compiling): the editor tick can move it before the next call.")));
        Context.Result->SetArrayField(TEXT("warnings"), Warnings);
    }
    if (ShotOptions.IsValid())
    {
        // The screenshot handler answers this request; the step result rides along in its reply.
        ShotOptions->SetObjectField(TEXT("_mcpCarry"), Context.Result);
        return FMcpNiagaraHandlerAccess::Screenshot(Context.Subsystem, Context.RequestId, ShotOptions, Context.RequestingSocket);
    }
    Context.SendSuccess(true, FString::Printf(TEXT("Advanced %d frame(s) of %.4f s to age %.4f%s."), Frames, Delta, Age, bPaused ? TEXT("; paused until the next step") : TEXT("")));
    return true;
}

bool HandleSimulationControlAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("step_niagara_simulation")) return StepSimulation(Context);
    return false;
}

bool HandleSimulationSettingsAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("set_niagara_sim_target")) return SetEmitterSimTarget(Context);
    if (SubAction == TEXT("set_niagara_simulation_stage")) return SetSimulationStage(Context);
    return false;
}
}

#else

namespace McpNiagaraAuthoringHandlers
{
bool HandleSimulationControlAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("step_niagara_simulation"))
    {
        Context.SendError(TEXT("Niagara simulation stepping is built for UE 5.8 only."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    return false;
}

bool HandleSimulationSettingsAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("set_niagara_sim_target") || SubAction == TEXT("set_niagara_simulation_stage"))
    {
        Context.SendError(TEXT("These Niagara simulation settings are built for UE 5.8 only."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    return false;
}
}

#endif
