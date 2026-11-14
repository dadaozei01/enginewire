#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"

#if MCP_HAS_NIAGARA_INPUT_OVERRIDES && ENGINE_MINOR_VERSION >= 8

#include <initializer_list>

#include "Editor.h"
#include "EngineUtils.h"
#include "NiagaraDataSetAccessor.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraSimCache.h"
#include "NiagaraSimCacheFunctionLibrary.h"
#include "UObject/Package.h"

namespace McpNiagaraAuthoringHandlers
{
namespace
{
TSharedPtr<FJsonValue> ParticleNumbers(std::initializer_list<double> Numbers)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (double Number : Numbers)
    {
        if (!FMath::IsFinite(Number)) return nullptr;
        Values.Add(MakeShared<FJsonValueNumber>(Number));
    }
    return MakeShared<FJsonValueArray>(Values);
}

TSharedPtr<FJsonValue> ParticleValue(float Value, const FTransform&)
{
    return FMath::IsFinite(Value) ? MakeShared<FJsonValueNumber>(Value) : TSharedPtr<FJsonValue>();
}
TSharedPtr<FJsonValue> ParticleValue(int32 Value, const FTransform&) { return MakeShared<FJsonValueNumber>(Value); }
TSharedPtr<FJsonValue> ParticleValue(FNiagaraBool Value, const FTransform&) { return MakeShared<FJsonValueBoolean>(Value.GetValue()); }
TSharedPtr<FJsonValue> ParticleValue(FVector2f Value, const FTransform&) { return ParticleNumbers({Value.X, Value.Y}); }
TSharedPtr<FJsonValue> ParticleValue(FVector3f Value, const FTransform&) { return ParticleNumbers({Value.X, Value.Y, Value.Z}); }
TSharedPtr<FJsonValue> ParticleValue(FVector4f Value, const FTransform&) { return ParticleNumbers({Value.X, Value.Y, Value.Z, Value.W}); }
TSharedPtr<FJsonValue> ParticleValue(FLinearColor Value, const FTransform&) { return ParticleNumbers({Value.R, Value.G, Value.B, Value.A}); }
TSharedPtr<FJsonValue> ParticleValue(FQuat4f Value, const FTransform&) { return ParticleNumbers({Value.X, Value.Y, Value.Z, Value.W}); }
TSharedPtr<FJsonValue> ParticleValue(FNiagaraPosition Value, const FTransform& SimToWorld)
{
    const FVector World = SimToWorld.TransformPosition(FVector(Value));
    return ParticleNumbers({World.X, World.Y, World.Z});
}

template<typename T>
bool ReadParticleValues(const FNiagaraDataBuffer* Buffer, FName Name, int32 Count, const FTransform& SimToWorld, TArray<TSharedPtr<FJsonValue>>& Out)
{
    const auto Reader = FNiagaraDataSetAccessor<T>::CreateReader(Buffer, Name);
    if (!Reader.IsValid()) return false;
    Out.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        TSharedPtr<FJsonValue> Value = ParticleValue(Reader.Get(Index), SimToWorld);
        if (!Value) return false;
        Out.Add(Value);
    }
    return true;
}

bool ReadParticleAttribute(const FNiagaraDataBuffer* Buffer, const FNiagaraVariableBase& Variable, int32 Count,
    const FTransform& SimToWorld, TArray<TSharedPtr<FJsonValue>>& Values)
{
    const FNiagaraTypeDefinition& Type = Variable.GetType();
    const FName Name = Variable.GetName();
    if (Type == FNiagaraTypeDefinition::GetFloatDef()) return ReadParticleValues<float>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetIntDef()) return ReadParticleValues<int32>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetBoolDef()) return ReadParticleValues<FNiagaraBool>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetVec2Def()) return ReadParticleValues<FVector2f>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetVec3Def()) return ReadParticleValues<FVector3f>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetVec4Def()) return ReadParticleValues<FVector4f>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetColorDef()) return ReadParticleValues<FLinearColor>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetPositionDef()) return ReadParticleValues<FNiagaraPosition>(Buffer, Name, Count, SimToWorld, Values);
    if (Type == FNiagaraTypeDefinition::GetQuatDef()) return ReadParticleValues<FQuat4f>(Buffer, Name, Count, SimToWorld, Values);
    return false;
}

const FNiagaraVariableBase* FindParticleAttribute(const FNiagaraDataSet& Data, const FString& Name)
{
    // Compiled particle datasets normally omit the Particles namespace. Accept both spellings.
    FString ShortName = Name;
    ShortName.RemoveFromStart(TEXT("Particles."), ESearchCase::IgnoreCase);
    for (const FNiagaraVariableBase& Variable : Data.GetVariables())
    {
        if (Variable.GetName() == FName(*Name) || Variable.GetName() == FName(*ShortName)
            || Variable.GetName() == FName(*(TEXT("Particles.") + ShortName))) return &Variable;
    }
    return nullptr;
}

bool ParticleInteger(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field, int32 Default, int32 Min, int32 Max, int32& Out)
{
    Out = Default;
    if (!Payload->HasField(Field)) return true;
    double Number = 0;
    if (!Payload->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number) || Number < Min || Number > Max || Number != FMath::FloorToDouble(Number)) return false;
    Out = static_cast<int32>(Number);
    return true;
}

// No raw instance/buffer escapes this synchronous game-thread request. Waiting without finalization does not tick,
// activate or run completion callbacks. AdvanceSimulation does finalize and can release/recycle the component, so
// all identities are reacquired afterwards. Never read GPU render-thread storage through this CPU path.
FNiagaraSystemInstance* ResolveParticleInstance(FActionContext& Context, UNiagaraComponent* Component)
{
    const FNiagaraSystemInstanceControllerPtr Controller = Component ? Component->GetSystemInstanceController() : nullptr;
    if (!Component || !Controller.IsValid() || !Controller->IsValid())
    {
        Context.SendError(TEXT("The Niagara component is not active or has no system instance. Activate it in the intended editor/PIE world first."), TEXT("SYSTEM_NOT_ACTIVE"));
        return nullptr;
    }
    FNiagaraSystemInstance* Instance = Controller->GetSystemInstance_Unsafe();
    Instance->WaitForConcurrentTickDoNotFinalize();
    if (Instance->IsComplete())
    {
        Context.SendError(TEXT("The Niagara system instance is complete. Activate or reset it explicitly before reading."), TEXT("SYSTEM_COMPLETE"));
        return nullptr;
    }
    if (!Component->IsActive())
    {
        Context.SendError(TEXT("The Niagara component is not active. Activate it in the intended editor/PIE world first."), TEXT("SYSTEM_NOT_ACTIVE"));
        return nullptr;
    }
    return Instance;
}

FNiagaraEmitterInstance* FindParticleEmitter(FActionContext& Context, FNiagaraSystemInstance* Instance,
    const FString& EmitterName, bool bRequireBuffer)
{
    TArray<FString> Names;
    FNiagaraEmitterInstance* Found = nullptr;
    for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
    {
        const FString Name = Emitter->GetEmitterHandle().GetName().ToString();
        Names.Add(Name);
        if (Name.Equals(EmitterName, ESearchCase::IgnoreCase)) Found = &Emitter.Get();
    }
    if (!Found)
    {
        Context.SendError(FString::Printf(TEXT("No emitter '%s'; available emitter instances: %s."), *EmitterName, *FString::Join(Names, TEXT(", "))), TEXT("EMITTER_NOT_FOUND"));
        return nullptr;
    }
    if (Found->GetSimTarget() != ENiagaraSimTarget::CPUSim || !Found->AsStateful())
    {
        Context.SendError(TEXT("Only stateful CPU emitters expose a CPU particle dataset. GPU/stateless emitters are NOT_SUPPORTED; no readback or substitute data is returned."), TEXT("NOT_SUPPORTED"));
        return nullptr;
    }
    if (bRequireBuffer && !Found->GetParticleData().GetCurrentData())
    {
        Context.SendError(TEXT("The emitter has no current CPU particle buffer. Pass advanceTicks > 0 to simulate first, or read it in PIE after it has ticked."), TEXT("NO_SIMULATION_DATA"));
        return nullptr;
    }
    return Found;
}

FNiagaraEmitterInstance* ResolveParticleEmitter(FActionContext& Context, UNiagaraComponent* Component,
    const FString& EmitterName, FNiagaraSystemInstance*& OutInstance)
{
    OutInstance = ResolveParticleInstance(Context, Component);
    if (!OutInstance) return nullptr;
    if (OutInstance->IsPendingSpawn())
    {
        Context.SendError(TEXT("The Niagara system is still pending its first simulation tick. Pass advanceTicks > 0 to simulate first, or read it in PIE after it has ticked."), TEXT("NO_SIMULATION_DATA"));
        return nullptr;
    }
    return FindParticleEmitter(Context, OutInstance, EmitterName, true);
}

bool ValidateParticleAttributes(FActionContext& Context, FNiagaraEmitterInstance* Emitter, const TArray<FString>& Attributes)
{
    TArray<FString> Available;
    for (const FNiagaraVariableBase& Variable : Emitter->GetParticleData().GetVariables()) Available.Add(Variable.GetName().ToString());
    for (const FString& Name : Attributes)
    {
        const FNiagaraVariableBase* Variable = FindParticleAttribute(Emitter->GetParticleData(), Name);
        if (!Variable)
        {
            Context.SendError(FString::Printf(TEXT("Attribute '%s' not found; available attributes: %s. Unused attributes may be removed by compilation."), *Name, *FString::Join(Available, TEXT(", "))), TEXT("ATTRIBUTE_NOT_FOUND"));
            return false;
        }
        TArray<TSharedPtr<FJsonValue>> Empty;
        if (!ReadParticleAttribute(Emitter->GetParticleData().GetCurrentData(), *Variable, 0, FTransform::Identity, Empty))
        {
            Context.SendError(FString::Printf(TEXT("Attribute '%s' has unsupported type/layout %s. Supported: Float, Int, Bool, Vector2, Vector, Vector4, Color, Position, Quat."), *Name, *Variable->GetType().GetName()), TEXT("NOT_SUPPORTED"));
            return false;
        }
    }
    return true;
}


// GPU emitters keep their particles in GPU buffers, which this module cannot read back directly (the engine's dataset
// readback classes are not exported). A Niagara sim cache can: CaptureNiagaraSimCacheImmediate flushes the GPU
// simulation and copies the current frame of every emitter, GPU ones included, into a transient cache, whose exported
// readers return each attribute. Synchronous: the call waits for the render thread, and its time is reported.
template<typename T, typename F>
void AddGpuValues(TArray<TSharedPtr<FJsonValue>>& Out, const TArray<T>& Values, int32 Count, F&& ToJson)
{
    for (int32 Index = 0; Index < FMath::Min(Count, Values.Num()); ++Index)
    {
        Out.Add(ToJson(Values[Index]));
    }
}

bool ReadGpuParticleAttribute(const UNiagaraSimCache& Cache, const FNiagaraVariableBase& Variable, FName Emitter, int32 Count, TArray<TSharedPtr<FJsonValue>>& Out)
{
    const FNiagaraTypeDefinition& Type = Variable.GetType();
    const FName Name = Variable.GetName();
    const auto Num = [](double V) { return MakeShared<FJsonValueNumber>(V); };
    const auto Arr = [](std::initializer_list<double> V) { return ParticleNumbers(V); };
    if (Type == FNiagaraTypeDefinition::GetFloatDef()) { TArray<float> V; Cache.ReadFloatAttribute(V, Name, Emitter); AddGpuValues(Out, V, Count, [&](float X) -> TSharedPtr<FJsonValue> { return Num(X); }); return true; }
    if (Type == FNiagaraTypeDefinition::GetIntDef()) { TArray<int32> V; Cache.ReadIntAttribute(V, Name, Emitter); AddGpuValues(Out, V, Count, [&](int32 X) -> TSharedPtr<FJsonValue> { return Num(X); }); return true; }
    // Bool attributes come back empty from the sim cache readers (tested: 0 of 9 values), so they are refused.
    if (Type == FNiagaraTypeDefinition::GetVec2Def()) { TArray<FVector2D> V; Cache.ReadVector2Attribute(V, Name, Emitter); AddGpuValues(Out, V, Count, [&](const FVector2D& X) { return Arr({X.X, X.Y}); }); return true; }
    if (Type == FNiagaraTypeDefinition::GetVec3Def()) { TArray<FVector> V; Cache.ReadVectorAttribute(V, Name, Emitter); AddGpuValues(Out, V, Count, [&](const FVector& X) { return Arr({X.X, X.Y, X.Z}); }); return true; }
    if (Type == FNiagaraTypeDefinition::GetVec4Def()) { TArray<FVector4> V; Cache.ReadVector4Attribute(V, Name, Emitter); AddGpuValues(Out, V, Count, [&](const FVector4& X) { return Arr({X.X, X.Y, X.Z, X.W}); }); return true; }
    if (Type == FNiagaraTypeDefinition::GetColorDef()) { TArray<FLinearColor> V; Cache.ReadColorAttribute(V, Name, Emitter); AddGpuValues(Out, V, Count, [&](const FLinearColor& X) { return Arr({X.R, X.G, X.B, X.A}); }); return true; }
    if (Type == FNiagaraTypeDefinition::GetPositionDef()) { TArray<FVector> V; Cache.ReadPositionAttribute(V, Name, Emitter, true); AddGpuValues(Out, V, Count, [&](const FVector& X) { return Arr({X.X, X.Y, X.Z}); }); return true; }
    if (Type == FNiagaraTypeDefinition::GetQuatDef()) { TArray<FQuat> V; Cache.ReadQuatAttribute(V, Name, Emitter, false); AddGpuValues(Out, V, Count, [&](const FQuat& X) { return Arr({X.X, X.Y, X.Z, X.W}); }); return true; }
    return false;
}

bool ReadGpuParticles(FActionContext& Context, UWorld* World, AActor* Actor, UNiagaraComponent* Component, FNiagaraEmitterInstance* Emitter,
    const FString& EmitterName, const TArray<FString>& Attributes, int32 Limit, int32 AdvanceTicks, double Delta)
{
    // Attribute names and types come from the emitter's compiled dataset layout, which a GPU emitter has too.
    TArray<FString> Available;
    for (const FNiagaraVariableBase& Variable : Emitter->GetParticleData().GetVariables()) Available.Add(Variable.GetName().ToString());
    TArray<const FNiagaraVariableBase*> Variables;
    for (const FString& Name : Attributes)
    {
        const FNiagaraVariableBase* Variable = FindParticleAttribute(Emitter->GetParticleData(), Name);
        if (!Variable)
        {
            Context.SendError(FString::Printf(TEXT("Attribute '%s' not found; available attributes: %s. Unused attributes may be removed by compilation."), *Name, *FString::Join(Available, TEXT(", "))), TEXT("ATTRIBUTE_NOT_FOUND"));
            return true;
        }
        Variables.Add(Variable);
    }
    const FName HandleName = Emitter->GetEmitterHandle().GetName();
    TWeakObjectPtr<UNiagaraComponent> WeakComponent = Component;
    if (AdvanceTicks > 0)
    {
        const FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
        if (Controller.IsValid() && Controller->IsPaused())
        {
            Context.SendError(TEXT("AdvanceSimulation does not advance a paused Niagara system. Unpause it (step_niagara_simulation pause=false) or step it with step_niagara_simulation first."), TEXT("SYSTEM_PAUSED"));
            return true;
        }
        Component->AdvanceSimulation(AdvanceTicks, static_cast<float>(Delta));
        if (!WeakComponent.IsValid())
        {
            Context.SendError(TEXT("The component went away during AdvanceSimulation (completion/pooling)."), TEXT("INSTANCE_CHANGED"));
            return true;
        }
    }
    UNiagaraSimCache* Cache = NewObject<UNiagaraSimCache>(GetTransientPackage(), NAME_None, RF_Transient);
    UNiagaraSimCache* Captured = nullptr;
    FNiagaraSimCacheCreateParameters Parameters;
    Parameters.AttributeCaptureMode = ENiagaraSimCacheAttributeCaptureMode::All;
    // The cache writes only a running instance (a paused one is skipped without a word), so a component paused by
    // step_niagara_simulation is let go for the capture and paused again; nothing ticks in between.
    const FNiagaraSystemInstanceControllerPtr PausedController = Component->GetSystemInstanceController();
    const bool bWasPaused = PausedController.IsValid() && PausedController->IsValid() && PausedController->IsPaused();
    if (bWasPaused)
    {
        Component->SetPaused(false);
    }
    const double Start = FPlatformTime::Seconds();
    const bool bCaptured = UNiagaraSimCacheFunctionLibrary::CaptureNiagaraSimCacheImmediate(Cache, Parameters, Component, Captured, false, 1.0f / 60.0f);
    const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
    if (bWasPaused && WeakComponent.IsValid())
    {
        Component->SetPaused(true);
    }
    const int32 EmitterIndex = Captured ? Captured->GetEmitterIndex(HandleName) : INDEX_NONE;
    if (!bCaptured || !Captured || Captured->IsEmpty() || EmitterIndex == INDEX_NONE)
    {
        Context.SendError(FString::Printf(TEXT("The GPU readback (sim cache capture) returned no frame for '%s' after %.0f ms; the system may not have run on the GPU yet (step it a few frames first) or the platform has no GPU compute."), *EmitterName, ElapsedMs), TEXT("GPU_READBACK_FAILED"));
        return true;
    }
    const int32 Total = Captured->GetEmitterNumInstances(EmitterIndex, 0);
    const int32 Count = FMath::Min(Limit, Total);
    TArray<TSharedPtr<FJsonValue>> Output;
    for (const FNiagaraVariableBase* Variable : Variables)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        if (!ReadGpuParticleAttribute(*Captured, *Variable, HandleName, Count, Values) || Values.Num() != Count)
        {
            Context.SendError(FString::Printf(TEXT("Cannot read GPU attribute '%s' (type %s, %d of %d values). GPU readback supports Float, Int, Vector2, Vector, Vector4, Color, Position, Quat (not Bool: write an Int instead)."),
                *Variable->GetName().ToString(), *Variable->GetType().GetName(), Values.Num(), Count), TEXT("PARTICLE_DATA_UNAVAILABLE"));
            return true;
        }
        TSharedPtr<FJsonObject> Attribute = MakeShared<FJsonObject>();
        Attribute->SetStringField(TEXT("name"), Variable->GetName().ToString());
        Attribute->SetStringField(TEXT("type"), Variable->GetType().GetName());
        Attribute->SetArrayField(TEXT("values"), Values);
        if (Variable->GetType() == FNiagaraTypeDefinition::GetPositionDef()) Attribute->SetStringField(TEXT("coordinateSpace"), TEXT("world"));
        Output.Add(MakeShared<FJsonValueObject>(Attribute));
    }
    const FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
    Context.Result->SetNumberField(TEXT("particleCount"), Total);
    Context.Result->SetNumberField(TEXT("returnedCount"), Count);
    Context.Result->SetStringField(TEXT("simTarget"), TEXT("GPUComputeSim"));
    Context.Result->SetStringField(TEXT("readback"), TEXT("simCache"));
    Context.Result->SetNumberField(TEXT("readbackMs"), ElapsedMs);
    Context.Result->SetStringField(TEXT("world"), World->GetPathName());
    Context.Result->SetStringField(TEXT("worldType"), World->WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("Editor"));
    Context.Result->SetStringField(TEXT("actorName"), Actor->GetName());
    Context.Result->SetStringField(TEXT("componentName"), Component->GetName());
    Context.Result->SetStringField(TEXT("emitterName"), EmitterName);
    Context.Result->SetBoolField(TEXT("localSpaceEmitter"), Emitter->IsLocalSpace());
    Context.Result->SetNumberField(TEXT("advanceTicksRequested"), AdvanceTicks);
    Context.Result->SetNumberField(TEXT("age"), Controller.IsValid() && Controller->IsValid() ? Controller->GetAge() : 0.0);
    Context.Result->SetArrayField(TEXT("attributes"), Output);
    Context.SendSuccess(true, TEXT("GPU particle snapshot read back through a transient sim cache capture. Position is in world space; other vectors remain in simulation space. No assets saved."));
    return true;
}

bool GetParticleData(FActionContext& Context)
{
    if (!IsInGameThread())
    {
        Context.SendError(TEXT("Particle snapshots must run on the game thread."), TEXT("INVALID_THREAD"));
        return true;
    }
    const FString ActorName = GetJsonStringField(Context.Payload, TEXT("actorName"));
    const FString EmitterName = GetJsonStringField(Context.Payload, TEXT("emitterName"));
    const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
    int32 Limit = 100, AdvanceTicks = 0;
    double Delta = 1.0 / 60.0;
    if (ActorName.IsEmpty() || EmitterName.IsEmpty() || !Context.Payload->TryGetArrayField(TEXT("attributes"), Requested) || Requested->IsEmpty()
        || !ParticleInteger(Context.Payload, TEXT("maxParticles"), 100, 1, 2000, Limit)
        || !ParticleInteger(Context.Payload, TEXT("advanceTicks"), 0, 0, 10000, AdvanceTicks)
        || (Context.Payload->HasField(TEXT("tickDeltaSeconds")) && !Context.Payload->TryGetNumberField(TEXT("tickDeltaSeconds"), Delta))
        || !FMath::IsFinite(Delta) || Delta <= SMALL_NUMBER || Delta > 1.0
        || (Context.Payload->HasField(TEXT("tickDeltaSeconds")) && !Context.Payload->HasField(TEXT("advanceTicks"))))
    {
        Context.SendError(TEXT("Require actorName, emitterName and nonempty attributes (strings). maxParticles: integer 1..2000 (default 100); advanceTicks: integer 0..10000; tickDeltaSeconds: > SMALL_NUMBER and <= 1 (default 1/60), only with advanceTicks."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    TArray<FString> Attributes;
    for (const TSharedPtr<FJsonValue>& Value : *Requested)
    {
        FString Name;
        if (!Value.IsValid() || !Value->TryGetString(Name) || Name.IsEmpty())
        {
            Context.SendError(TEXT("Every attributes entry must be a nonempty string."), TEXT("INVALID_ARGUMENT"));
            return true;
        }
        Attributes.AddUnique(Name);
    }
    UWorld* World = GEditor ? (GEditor->PlayWorld ? GEditor->PlayWorld.Get() : GEditor->GetEditorWorldContext().World()) : nullptr;
    if (!World || World->bIsTearingDown)
    {
        Context.SendError(TEXT("No usable editor/PIE world."), TEXT("WORLD_NOT_FOUND"));
        return true;
    }
    AActor* Actor = nullptr;
    TArray<FString> ActorNames;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        ActorNames.Add(It->GetActorLabel());
        if (It->GetName().Equals(ActorName, ESearchCase::IgnoreCase) || It->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase)
            || It->GetPathName().Equals(ActorName, ESearchCase::IgnoreCase))
        {
            if (Actor)
            {
                Context.SendError(TEXT("actorName is ambiguous in the selected world; use the actor's unique object name or full object path."), TEXT("AMBIGUOUS_ACTOR"));
                return true;
            }
            Actor = *It;
        }
    }
    if (!Actor)
    {
        Context.SendError(FString::Printf(TEXT("Actor '%s' not found in %s; available actor labels: %s."), *ActorName, *World->GetName(), *FString::Join(ActorNames, TEXT(", "))), TEXT("ACTOR_NOT_FOUND"));
        return true;
    }
    TInlineComponentArray<UNiagaraComponent*> Components(Actor);
    if (Components.Num() != 1)
    {
        TArray<FString> Names;
        for (UNiagaraComponent* Each : Components) Names.Add(Each->GetName());
        Context.SendError(FString::Printf(TEXT("Expected exactly one Niagara component on '%s'; available components: %s. Use an actor with one Niagara component."), *ActorName,
            Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", "))), Components.IsEmpty() ? TEXT("COMPONENT_NOT_FOUND") : TEXT("AMBIGUOUS_COMPONENT"));
        return true;
    }
    TWeakObjectPtr<UNiagaraComponent> Component = Components[0];
    FNiagaraSystemInstance* Instance = ResolveParticleInstance(Context, Component.Get());
    if (!Instance) return true;
    for (const FNiagaraEmitterInstanceRef& Each : Instance->GetEmitters())
    {
        if (Each->GetSimTarget() == ENiagaraSimTarget::GPUComputeSim && Each->GetEmitterHandle().GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
        {
            return ReadGpuParticles(Context, World, Actor, Component.Get(), &Each.Get(), EmitterName, Attributes, Limit, AdvanceTicks, Delta);
        }
    }
    FNiagaraEmitterInstance* Emitter = nullptr;
    if (AdvanceTicks > 0)
    {
        if (Instance->IsPaused())
        {
            Context.SendError(TEXT("AdvanceSimulation does not advance a paused Niagara system. Unpause it explicitly first."), TEXT("SYSTEM_PAUSED"));
            return true;
        }
        // A pending instance may have no emitters/buffer yet. AdvanceSimulation's solo tick performs its first spawn.
        // If a buffer is already available, preserve validation before advancing; otherwise defer it until after.
        bool bHasCurrentBuffer = false;
        for (const FNiagaraEmitterInstanceRef& Each : Instance->GetEmitters())
        {
            if (Each->AsStateful() && Each->GetParticleData().GetCurrentData())
            {
                bHasCurrentBuffer = true;
                break;
            }
        }
        if (bHasCurrentBuffer)
        {
            Emitter = FindParticleEmitter(Context, Instance, EmitterName, false);
            if (!Emitter) return true;
            if (Emitter->GetParticleData().GetCurrentData() && !ValidateParticleAttributes(Context, Emitter, Attributes)) return true;
        }
        const FNiagaraSystemInstanceID OriginalId = Instance->GetId();
        const float AgeBefore = Instance->GetAge();
        Component->AdvanceSimulation(AdvanceTicks, static_cast<float>(Delta));
        Emitter = ResolveParticleEmitter(Context, Component.Get(), EmitterName, Instance);
        if (!Emitter) return true;
        if (Instance->GetId() != OriginalId)
        {
            Context.SendError(TEXT("The system instance changed during AdvanceSimulation (completion/pooling). No snapshot of a replacement instance was returned."), TEXT("INSTANCE_CHANGED"));
            return true;
        }
        if (Instance->GetAge() <= AgeBefore)
        {
            Context.SendError(TEXT("AdvanceSimulation did not increase the system age. No successful advance is claimed; check compilation, activation and tick state."), TEXT("NOT_ADVANCED"));
            return true;
        }
    }
    else
    {
        Emitter = ResolveParticleEmitter(Context, Component.Get(), EmitterName, Instance);
        if (!Emitter) return true;
    }
    if (!ValidateParticleAttributes(Context, Emitter, Attributes)) return true;
    const FNiagaraDataSet& Data = Emitter->GetParticleData();
    const FNiagaraDataBuffer* Buffer = Data.GetCurrentData();
    const int32 Total = static_cast<int32>(Buffer->GetNumInstances());
    const int32 Count = FMath::Min(Limit, Total);
    const FTransform SimToWorld = Instance->GetLWCSimToWorld(Emitter->IsLocalSpace());
    TArray<TSharedPtr<FJsonValue>> Output;
    for (const FString& Name : Attributes)
    {
        const FNiagaraVariableBase* Variable = FindParticleAttribute(Data, Name);
        TArray<TSharedPtr<FJsonValue>> Values;
        if (!Variable || !ReadParticleAttribute(Buffer, *Variable, Count, SimToWorld, Values))
        {
            Context.SendError(FString::Printf(TEXT("Cannot read attribute '%s': its layout changed, reader is invalid or a value is non-finite. No substitute values returned."), *Name), TEXT("PARTICLE_DATA_UNAVAILABLE"));
            return true;
        }
        TSharedPtr<FJsonObject> Attribute = MakeShared<FJsonObject>();
        Attribute->SetStringField(TEXT("name"), Variable->GetName().ToString());
        Attribute->SetStringField(TEXT("type"), Variable->GetType().GetName());
        Attribute->SetArrayField(TEXT("values"), Values);
        if (Variable->GetType() == FNiagaraTypeDefinition::GetPositionDef()) Attribute->SetStringField(TEXT("coordinateSpace"), TEXT("world"));
        Output.Add(MakeShared<FJsonValueObject>(Attribute));
    }
    Context.Result->SetNumberField(TEXT("particleCount"), Total);
    Context.Result->SetNumberField(TEXT("returnedCount"), Count);
    Context.Result->SetStringField(TEXT("simTarget"), TEXT("CPUSim"));
    Context.Result->SetStringField(TEXT("world"), World->GetPathName());
    Context.Result->SetStringField(TEXT("worldType"), World->WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("Editor"));
    Context.Result->SetStringField(TEXT("actorName"), Actor->GetName());
    Context.Result->SetStringField(TEXT("componentName"), Component->GetName());
    Context.Result->SetStringField(TEXT("emitterName"), EmitterName);
    Context.Result->SetBoolField(TEXT("localSpaceEmitter"), Emitter->IsLocalSpace());
    Context.Result->SetNumberField(TEXT("advanceTicksRequested"), AdvanceTicks);
    Context.Result->SetNumberField(TEXT("age"), Instance->GetAge());
    Context.Result->SetArrayField(TEXT("attributes"), Output);
    Context.SendSuccess(true, TEXT("CPU particle snapshot. Position is in world space (Unreal units), with Niagara LWC tile and local-emitter transform applied. Other vectors remain in simulation space. No assets saved."));
    return true;
}
} // namespace
}
#endif

namespace McpNiagaraAuthoringHandlers
{
bool HandleParticleDataAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction != TEXT("get_niagara_particle_data")) return false;
#if MCP_HAS_NIAGARA_INPUT_OVERRIDES && ENGINE_MINOR_VERSION >= 8
    return GetParticleData(Context);
#else
    Context.SendError(TEXT("Niagara particle snapshots are built for UE 5.8 only."), TEXT("NOT_SUPPORTED"));
    return true;
#endif
}
}
