#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"
#include "EdGraphSchema_Niagara.h"
#include "Algo/Reverse.h"
#include "Domains/NiagaraAuthoring/McpNiagaraCreatePackage.h"
#include "NiagaraSpriteRendererProperties.h"

namespace McpNiagaraAuthoringHandlers
{
static bool CreateNiagaraSystem(FActionContext& Context)
{
    if (Context.Name.IsEmpty())
    {
        Context.SendError(TEXT("Missing 'name' parameter."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    FString PathError;
    const FString PackageName = McpNiagaraCreatePackageName(Context.Path, Context.Name, PathError);
    if (PackageName.IsEmpty())
    {
        Context.SendError(PathError, TEXT("INVALID_ARGUMENT"));
        return true;
    }
    // CreatePackage hands back a package already there, and NewObject under a taken name rebuilt the asset in place
    // (a system's emitters gone) or, for another class, stopped the editor with a fatal error: refuse it instead.
    FString CreateError, CreateCode;
    UPackage* Package = McpCreatePackageForNewAsset(PackageName, CreateError, CreateCode);
    if (!Package)
    {
        Context.SendError(CreateError, CreateCode);
        return true;
    }
    // RF_Transactional as the asset factory gives it: without it undo skipped the system and every Niagara stack
    // read reported "Object is not transctional, undo won't work for it!".
    UNiagaraSystem* NewSystem = NewObject<UNiagaraSystem>(Package, FName(*Context.Name), RF_Public | RF_Standalone | RF_Transactional);
    if (NewSystem)
    {
#if MCP_HAS_NIAGARA_SYSTEM_FACTORY_NEW
        FModuleManager::Get().LoadModule(TEXT("NiagaraEditor"));
        UNiagaraSystemFactoryNew::InitializeSystem(NewSystem, true);
#endif
        // The factory compiles a new system; built here without it, it was saved with no compiled scripts.
        NewSystem->RequestCompile(false);
    }
    if (!NewSystem)
    {
        Context.SendError(TEXT("Failed to create Niagara System."), TEXT("CREATE_FAILED"));
        return true;
    }
    FAssetRegistryModule::AssetCreated(NewSystem);
    if (Context.bSave)
    {
        McpSafeAssetSave(NewSystem);
    }
    // save false leaves nothing on disk by design: checked then too, every such create answered CREATE_FAILED for a
    // system it had made.
    if (Context.bSave && !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(NewSystem->GetPathName())))
    {
        Context.SendError(TEXT("Created system package does not exist on disk."), TEXT("CREATE_FAILED"));
        return true;
    }
    McpHandlerUtils::AddVerification(Context.Result, NewSystem);
    Context.Result->SetStringField(TEXT("systemPath"), NewSystem->GetPathName());
    Context.Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Created Niagara System: %s"), *Context.Name));
    Context.SendSuccess(true, TEXT("System created."));
    return true;
}

static bool CreateNiagaraEmitter(FActionContext& Context)
{
    if (Context.Name.IsEmpty())
    {
        Context.SendError(TEXT("Missing 'name' parameter."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    FString PathError;
    const FString PackageName = McpNiagaraCreatePackageName(Context.Path, Context.Name, PathError);
    if (PackageName.IsEmpty())
    {
        Context.SendError(PathError, TEXT("INVALID_ARGUMENT"));
        return true;
    }
    // CreatePackage hands back a package already there, and NewObject under a taken name rebuilt the asset in place
    // (a system's emitters gone) or, for another class, stopped the editor with a fatal error: refuse it instead.
    FString CreateError, CreateCode;
    UPackage* Package = McpCreatePackageForNewAsset(PackageName, CreateError, CreateCode);
    if (!Package)
    {
        Context.SendError(CreateError, CreateCode);
        return true;
    }
    const EObjectFlags EmitterFlags = RF_Public | RF_Standalone | RF_Transactional;
#if MCP_HAS_NIAGARA_EMITTER_FACTORY_NEW && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 3
    // InitializeEmitter is exported only from UE 5.3, so 5.1 and 5.2 go through the factory: its
    // virtual FactoryCreateNew makes the emitter and runs InitializeEmitter inside NiagaraEditor.
    FModuleManager::Get().LoadModule(TEXT("NiagaraEditor"));
    UNiagaraEmitterFactoryNew* Factory = NewObject<UNiagaraEmitterFactoryNew>();
    Factory->bAddDefaultModulesAndRenderersToEmptyEmitter = true;
    UNiagaraEmitter* NewEmitter = Cast<UNiagaraEmitter>(static_cast<UFactory*>(Factory)->FactoryCreateNew(
        UNiagaraEmitter::StaticClass(), Package, FName(*Context.Name), EmitterFlags, nullptr, GWarn));
    if (NewEmitter) { NewEmitter->SetUniqueEmitterName(Context.Name); }
#else
    UNiagaraEmitter* NewEmitter = NewObject<UNiagaraEmitter>(Package, FName(*Context.Name), EmitterFlags);
#endif
    if (!NewEmitter)
    {
        Context.SendError(TEXT("Failed to create Niagara Emitter."), TEXT("CREATE_FAILED"));
        return true;
    }
#if MCP_HAS_NIAGARA_EMITTER_FACTORY_NEW && (ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 3)
    FModuleManager::Get().LoadModule(TEXT("NiagaraEditor"));
    UNiagaraEmitterFactoryNew::InitializeEmitter(NewEmitter, true);
    NewEmitter->SetUniqueEmitterName(Context.Name);
#endif
    // The default sprite renderer comes with an empty material slot, which draws the engine's checker fallback
    // and hides particle colour. Fill only empty sprite slots with the engine's own default sprite material.
    int32 DefaultMaterialsApplied = 0;
    if (MCP_NIAGARA_EMITTER_DATA_TYPE* EmitterData = MCP_GET_LATEST_EMITTER_DATA(NewEmitter))
    {
        UMaterialInterface* DefaultSprite = LoadObject<UMaterialInterface>(nullptr, TEXT("/Niagara/DefaultAssets/DefaultSpriteMaterial.DefaultSpriteMaterial"));
        for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
        {
            UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer);
            if (Sprite && DefaultSprite && !Sprite->Material)
            {
                Sprite->Material = DefaultSprite;
                ++DefaultMaterialsApplied;
            }
        }
    }
    Context.Result->SetNumberField(TEXT("defaultSpriteMaterialsApplied"), DefaultMaterialsApplied);
    FAssetRegistryModule::AssetCreated(NewEmitter);
    if (Context.bSave)
    {
        McpSafeAssetSave(NewEmitter);
    }
    McpHandlerUtils::AddVerification(Context.Result, NewEmitter);
    Context.Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Created Niagara Emitter: %s"), *Context.Name));
    Context.SendSuccess(true, TEXT("Emitter created."));
    return true;
}

static bool AddEmitterToSystem(FActionContext& Context)
{
    if (Context.Payload->HasField(TEXT("sourceSystemPath")))
    {
        return CopyEmitterFromSystem(Context);
    }
    // Naming both fields when only one is absent sends the caller looking at the
    // one they already supplied. Name the missing one, and say what emitterPath
    // is for: this instances an EXISTING emitter asset, it does not create one.
    if (Context.SystemPath.IsEmpty() || Context.EmitterPath.IsEmpty())
    {
        const bool bNoSystem = Context.SystemPath.IsEmpty();
        const bool bNoEmitter = Context.EmitterPath.IsEmpty();
        FString Missing;
        if (bNoSystem && bNoEmitter) { Missing = TEXT("'systemPath' and 'emitterPath'"); }
        else if (bNoSystem) { Missing = TEXT("'systemPath'"); }
        else { Missing = TEXT("'emitterPath'"); }
        Context.SendError(
            FString::Printf(TEXT("Missing %s. add_emitter instances an existing Niagara emitter "
                                 "asset into a system: emitterPath must point at a UNiagaraEmitter "
                                 "asset (create one with create_effect kind=niagara_emitter), or "
                                 "sourceSystemPath + sourceEmitterName copy another system's emitter."),
                            *Missing),
            TEXT("INVALID_ARGUMENT"));
        return true;
    }
    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Context.SystemPath);
    UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *Context.EmitterPath);
    if (!System || !Emitter)
    {
        Context.SendError(!System ? TEXT("Could not load Niagara System.") : TEXT("Could not load Niagara Emitter."), TEXT("ASSET_NOT_FOUND"));
        return true;
    }
    FString AddedEmitterName;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
    FModuleManager::Get().LoadModule(TEXT("NiagaraEditor"));
    System->Modify();
    Emitter->CheckVersionDataAvailable();
    const FGuid EmitterVersion = Emitter->GetExposedVersion().VersionGuid;
    FVersionedNiagaraEmitterData* EmitterData = Emitter->GetEmitterData(EmitterVersion);
    if (!EmitterData || !EmitterData->GraphSource)
    {
        Context.SendError(TEXT("Emitter graph source is not initialized."), TEXT("NIAGARA_EMITTER_INIT_FAILED"));
        return true;
    }
    const FGuid NewEmitterHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Emitter, EmitterVersion, false);
    FNiagaraEmitterHandle* AddedHandle = nullptr;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        if (Handle.GetId() == NewEmitterHandleId)
        {
            AddedHandle = const_cast<FNiagaraEmitterHandle*>(&Handle);
            break;
        }
    }
    if (!AddedHandle)
    {
        Context.SendError(TEXT("Failed to add emitter to Niagara system."), TEXT("CREATE_FAILED"));
        return true;
    }
    // Honor a caller-supplied 'emitterName' for the handle. Upstream derives the handle name
    // from the source emitter asset, silently ignoring the requested name; renaming here also
    // keeps the handle name consistent with the 'emitterName' that module actions resolve
    // against (the dispatch layer sends the same default for both add-emitter and modules).
    if (!Context.EmitterName.IsEmpty())
    {
        AddedHandle->SetName(FName(*Context.EmitterName), *System);
    }
    AddedEmitterName = AddedHandle->GetName().ToString();
#else
    AddedEmitterName = System->AddEmitterHandle(*Emitter, Context.EmitterName.IsEmpty() ? FName(*Emitter->GetName()) : FName(*Context.EmitterName)).GetName().ToString();
#endif
    // The engine's add leaves the system uncompiled; compile it as the other stack edits do.
    System->RequestCompile(false);
    MarkDirtyAndVerify(Context, System);
    Context.Result->SetStringField(TEXT("emitterName"), AddedEmitterName);
    Context.Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Added emitter '%s' to system."), *AddedEmitterName));
    Context.SendSuccess(true, TEXT("Emitter added to system."));
    return true;
}

static bool SetEmitterProperties(FActionContext& Context)
{
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!LoadSystemAndEmitter(Context, System, Handle))
    {
        return true;
    }
    // enabled turns the emitter on or off; moduleEnabled {Module: bool} turns stack modules on or off by the names
    // get_niagara_info lists, so a template module that fights the values set (a size-over-life curve, wind) can go.
    // Any other key used to be ignored under an "updated" reply.
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    const TSharedPtr<FJsonObject>* ModulesObj = nullptr;
    bool bEnabled = false, bLocalSpace = false, bDeterminism = false;
    const bool bHasProps = Context.Payload->TryGetObjectField(TEXT("emitterProperties"), PropsObj) && PropsObj->IsValid();
    const bool bHasEnabled = bHasProps && (*PropsObj)->TryGetBoolField(TEXT("enabled"), bEnabled);
    const bool bHasModules = bHasProps && (*PropsObj)->TryGetObjectField(TEXT("moduleEnabled"), ModulesObj) && ModulesObj->IsValid();
    // localSpace and determinism change the compiled scripts, so they recompile like moduleEnabled.
    const bool bHasLocalSpace = bHasProps && (*PropsObj)->TryGetBoolField(TEXT("localSpace"), bLocalSpace);
    const bool bHasDeterminism = bHasProps && (*PropsObj)->TryGetBoolField(TEXT("determinism"), bDeterminism);
    const int32 Recognized = int32(bHasEnabled) + int32(bHasModules) + int32(bHasLocalSpace) + int32(bHasDeterminism);
    if (Recognized == 0 || (*PropsObj)->Values.Num() != Recognized)
    {
        Context.SendError(TEXT("emitterProperties takes enabled, localSpace and determinism (booleans) and/or moduleEnabled ({ModuleName: boolean}, names as get_niagara_info lists them). Set module inputs with set_parameter_value."), TEXT("UNSUPPORTED_PROPERTY"));
        return true;
    }
    MCP_NIAGARA_EMITTER_DATA_TYPE* EmitterData = (bHasLocalSpace || bHasDeterminism) ? MCP_GET_EMITTER_DATA(*Handle) : nullptr;
    if ((bHasLocalSpace || bHasDeterminism) && (!EmitterData || !Handle->GetInstance().Emitter))
    {
        Context.SendError(TEXT("This emitter has no editable emitter data (stateless emitter?). Nothing was changed."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    UNiagaraScriptSource* Source = bHasModules ? GetEmitterScriptSource(Handle) : nullptr;
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    TArray<FString> Missing;
    if (bHasModules)
    {
        for (const auto& Pair : (*ModulesObj)->Values)
        {
            const FString ModuleName(*Pair.Key);
            bool bOn = true;
            int32 Matched = 0;
            if (Graph && Pair.Value.IsValid() && Pair.Value->TryGetBool(bOn))
            {
                for (UEdGraphNode* GraphNode : Graph->Nodes)
                {
                    UNiagaraNodeFunctionCall* Module = Cast<UNiagaraNodeFunctionCall>(GraphNode);
                    if (Module && Module->GetCalledUsage() == ENiagaraScriptUsage::Module &&
                        Module->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
                    {
                        Module->Modify();
#if MCP_HAS_NIAGARA_STACK_GRAPH_UTILITIES
                        FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Module, bOn);
#else
                        Module->SetEnabledState(bOn ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled, false);
#endif
                        Module->MarkNodeRequiresSynchronization(TEXT("Module enabled state changed"), true);
                        ++Matched;
                    }
                }
            }
            if (Matched == 0)
            {
                Missing.Add(ModuleName);
            }
        }
    }
    if (Missing.Num() > 0)
    {
        Context.SendError(FString::Printf(TEXT("No module named %s on emitter '%s' (or its value is not a boolean); get_niagara_info lists each emitter's modules."), *FString::Join(Missing, TEXT(", ")), *Context.EmitterName), TEXT("MODULE_NOT_FOUND"));
        return true;
    }
    if (bHasEnabled)
    {
        Handle->SetIsEnabled(bEnabled, *System, false);
    }
    if (EmitterData)
    {
        System->Modify();
        Handle->GetInstance().Emitter->Modify();
        if (bHasLocalSpace) EmitterData->bLocalSpace = bLocalSpace;
        if (bHasDeterminism) EmitterData->bDeterminism = bDeterminism;
        Handle->GetInstance().Emitter->PostEditChange();
        Context.Result->SetBoolField(TEXT("localSpace"), EmitterData->bLocalSpace);
        Context.Result->SetBoolField(TEXT("determinism"), EmitterData->bDeterminism);
    }
    if (bHasModules || EmitterData)
    {
        System->RequestCompile(false);
    }
    MarkDirtyAndVerify(Context, System);
    Context.Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Updated properties for emitter '%s'."), *Context.EmitterName));
    Context.SendSuccess(true, TEXT("Emitter properties updated."));
    return true;
}

namespace
{
struct FNiagaraStackBlock
{
    UNiagaraNodeFunctionCall* Module = nullptr;
    UEdGraphNode* HeadNode = nullptr; // NiagaraNodeParameterMapSet if present, else Module
    UEdGraphPin* HeadInPin = nullptr;
    UEdGraphPin* TailOutPin = nullptr;
};

UEdGraphPin* StackMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == Direction && !Pin->bOrphanedPin
            && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
        {
            return Pin;
        }
    }
    return nullptr;
}

const TCHAR* StackStageName(ENiagaraScriptUsage Usage)
{
    switch (Usage)
    {
    case ENiagaraScriptUsage::ParticleSpawnScript:
    case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("ParticleSpawn");
    case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("ParticleUpdate");
    case ENiagaraScriptUsage::ParticleEventScript: return TEXT("ParticleEvent");
    case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("ParticleSimulationStage");
    case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("EmitterSpawn");
    case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("EmitterUpdate");
    case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("SystemSpawn");
    case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("SystemUpdate");
    default: return TEXT("Other");
    }
}

bool CollectStageBlocks(UNiagaraNodeOutput* Output, TArray<FNiagaraStackBlock>& OutBlocks)
{
    OutBlocks.Reset();
    UEdGraphPin* In = StackMapPin(Output, EGPD_Input);
    TSet<UEdGraphNode*> Seen;
    while (In && In->LinkedTo.Num() == 1 && In->LinkedTo[0])
    {
        UEdGraphNode* Prev = In->LinkedTo[0]->GetOwningNode();
        if (!Prev || Seen.Contains(Prev)) break;
        Seen.Add(Prev);
        UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Prev);
        if (Call && Call->FunctionScript && Call->GetCalledUsage() == ENiagaraScriptUsage::Module)
        {
            FNiagaraStackBlock Block;
            Block.Module = Call;
            Block.TailOutPin = StackMapPin(Call, EGPD_Output);
            Block.HeadNode = Call;
            Block.HeadInPin = StackMapPin(Call, EGPD_Input);
            if (Block.HeadInPin && Block.HeadInPin->LinkedTo.Num() == 1 && Block.HeadInPin->LinkedTo[0])
            {
                UEdGraphNode* MaybeSet = Block.HeadInPin->LinkedTo[0]->GetOwningNode();
                if (MaybeSet && MaybeSet->GetClass()->GetName() == TEXT("NiagaraNodeParameterMapSet"))
                {
                    UEdGraphPin* SetIn = StackMapPin(MaybeSet, EGPD_Input);
                    UEdGraphPin* SetOut = StackMapPin(MaybeSet, EGPD_Output);
                    if (SetIn && SetIn->LinkedTo.Num() == 1 && SetOut && SetOut->LinkedTo.Num() == 1)
                    {
                        Block.HeadNode = MaybeSet;
                        Block.HeadInPin = SetIn;
                    }
                }
            }
            OutBlocks.Add(Block);
            In = Block.HeadInPin;
        }
        else
        {
            In = StackMapPin(Prev, EGPD_Input);
        }
    }
    Algo::Reverse(OutBlocks);
    return OutBlocks.Num() > 0;
}

bool FindModuleBlock(UNiagaraGraph* Graph, const FString& ModuleName, const FString& StageFilter,
    UNiagaraNodeOutput*& OutOutput, TArray<FNiagaraStackBlock>& OutBlocks, int32& OutIndex)
{
    TArray<UNiagaraNodeOutput*> Outputs;
    Graph->GetNodesOfClass(Outputs);
    for (UNiagaraNodeOutput* Output : Outputs)
    {
        if (!StageFilter.IsEmpty() && !FString(StackStageName(Output->GetUsage())).Equals(StageFilter, ESearchCase::IgnoreCase))
        {
            continue;
        }
        TArray<FNiagaraStackBlock> Blocks;
        CollectStageBlocks(Output, Blocks);
        for (int32 Idx = 0; Idx < Blocks.Num(); ++Idx)
        {
            if (Blocks[Idx].Module->GetFunctionName().Equals(ModuleName, ESearchCase::IgnoreCase))
            {
                OutOutput = Output;
                OutBlocks = MoveTemp(Blocks);
                OutIndex = Idx;
                return true;
            }
        }
    }
    return false;
}
} // namespace

static bool RemoveNiagaraModule(FActionContext& Context)
{
    const FString ModuleName = GetJsonStringField(Context.Payload, TEXT("moduleName"));
    const FString StageFilter = GetJsonStringField(Context.Payload, TEXT("stage"));
    if (ModuleName.IsEmpty())
    {
        Context.SendError(TEXT("Missing 'moduleName'."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!LoadSystemAndEmitter(Context, System, Handle)) return true;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Handle);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    if (!Graph)
    {
        Context.SendError(TEXT("Emitter has no Niagara graph source."), TEXT("NIAGARA_GRAPH_MISSING"));
        return true;
    }
    UNiagaraNodeOutput* Output = nullptr;
    TArray<FNiagaraStackBlock> Blocks;
    int32 Index = INDEX_NONE;
    if (!FindModuleBlock(Graph, ModuleName, StageFilter, Output, Blocks, Index))
    {
        Context.SendError(FString::Printf(TEXT("Module '%s' not found on emitter '%s'%s."), *ModuleName, *Context.EmitterName,
            StageFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" in stage '%s'"), *StageFilter)), TEXT("MODULE_NOT_FOUND"));
        return true;
    }
    FNiagaraStackBlock& Block = Blocks[Index];
    if (!Block.HeadInPin || Block.HeadInPin->LinkedTo.Num() != 1 || !Block.TailOutPin || Block.TailOutPin->LinkedTo.Num() != 1)
    {
        Context.SendError(TEXT("Module parameter-map links are not a simple chain; cannot safely remove."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    UEdGraphPin* PrevOut = Block.HeadInPin->LinkedTo[0];
    UEdGraphPin* NextIn = Block.TailOutPin->LinkedTo[0];
    Graph->Modify();
    Block.HeadInPin->BreakLinkTo(PrevOut);
    Block.TailOutPin->BreakLinkTo(NextIn);
    PrevOut->MakeLinkTo(NextIn);

    if (Block.HeadNode && Block.HeadNode != Block.Module)
    {
        for (UEdGraphPin* Pin : Block.HeadNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin != Block.HeadInPin)
            {
                TArray<UEdGraphNode*> Feeders;
                for (UEdGraphPin* Linked : Pin->LinkedTo) Feeders.AddUnique(Linked->GetOwningNode());
                Pin->BreakAllPinLinks();
                for (UEdGraphNode* Feeder : Feeders)
                {
                    if (Feeder && !Feeder->Pins.ContainsByPredicate([](const UEdGraphPin* P) { return P->Direction == EGPD_Output && P->LinkedTo.Num() > 0; }))
                    {
                        Graph->RemoveNode(Feeder);
                    }
                }
            }
        }
        Graph->RemoveNode(Block.HeadNode);
    }
    Graph->RemoveNode(Block.Module);
    Graph->NotifyGraphChanged();
    System->RequestCompile(false);
    MarkDirtyAndVerify(Context, System);
    Context.Result->SetStringField(TEXT("moduleName"), ModuleName);
    Context.Result->SetStringField(TEXT("stage"), StackStageName(Output->GetUsage()));
    Context.Result->SetBoolField(TEXT("removed"), true);
    Context.SendSuccess(true, FString::Printf(TEXT("Removed module '%s' from %s."), *ModuleName, StackStageName(Output->GetUsage())));
    return true;
}

static bool ReorderNiagaraModule(FActionContext& Context)
{
    const FString ModuleName = GetJsonStringField(Context.Payload, TEXT("moduleName"));
    const FString StageFilter = GetJsonStringField(Context.Payload, TEXT("stage"));
    double TargetIndexNum = -1.0;
    if (ModuleName.IsEmpty() || !Context.Payload->TryGetNumberField(TEXT("targetIndex"), TargetIndexNum)
        || TargetIndexNum < 0.0 || TargetIndexNum != FMath::FloorToDouble(TargetIndexNum))
    {
        Context.SendError(TEXT("Missing 'moduleName' or non-negative integer 'targetIndex'."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    const int32 TargetIndex = static_cast<int32>(TargetIndexNum);
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!LoadSystemAndEmitter(Context, System, Handle)) return true;
    UNiagaraScriptSource* Source = GetEmitterScriptSource(Handle);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    if (!Graph)
    {
        Context.SendError(TEXT("Emitter has no Niagara graph source."), TEXT("NIAGARA_GRAPH_MISSING"));
        return true;
    }
    UNiagaraNodeOutput* Output = nullptr;
    TArray<FNiagaraStackBlock> Blocks;
    int32 CurrentIndex = INDEX_NONE;
    if (!FindModuleBlock(Graph, ModuleName, StageFilter, Output, Blocks, CurrentIndex))
    {
        Context.SendError(FString::Printf(TEXT("Module '%s' not found on emitter '%s'."), *ModuleName, *Context.EmitterName), TEXT("MODULE_NOT_FOUND"));
        return true;
    }
    if (TargetIndex >= Blocks.Num())
    {
        Context.SendError(FString::Printf(TEXT("targetIndex %d out of range [0, %d]."), TargetIndex, Blocks.Num() - 1), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    if (CurrentIndex == TargetIndex)
    {
        Context.Result->SetStringField(TEXT("moduleName"), ModuleName);
        Context.Result->SetNumberField(TEXT("previousIndex"), CurrentIndex);
        Context.Result->SetNumberField(TEXT("targetIndex"), TargetIndex);
        Context.Result->SetBoolField(TEXT("moved"), false);
        Context.SendSuccess(true, TEXT("Module is already at targetIndex."));
        return true;
    }

    FNiagaraStackBlock Moved = Blocks[CurrentIndex];
    if (!Moved.HeadInPin || Moved.HeadInPin->LinkedTo.Num() != 1 || !Moved.TailOutPin || Moved.TailOutPin->LinkedTo.Num() != 1)
    {
        Context.SendError(TEXT("Module parameter-map links are not a simple chain; cannot safely reorder."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    Graph->Modify();
    UEdGraphPin* OldPrevOut = Moved.HeadInPin->LinkedTo[0];
    UEdGraphPin* OldNextIn = Moved.TailOutPin->LinkedTo[0];
    Moved.HeadInPin->BreakLinkTo(OldPrevOut);
    Moved.TailOutPin->BreakLinkTo(OldNextIn);
    OldPrevOut->MakeLinkTo(OldNextIn);

    Blocks.RemoveAt(CurrentIndex);
    UEdGraphPin* DestInPin = (TargetIndex == Blocks.Num()) ? StackMapPin(Output, EGPD_Input) : Blocks[TargetIndex].HeadInPin;
    UEdGraphPin* DestOutPin = (DestInPin && DestInPin->LinkedTo.Num() == 1) ? DestInPin->LinkedTo[0] : nullptr;
    if (!DestInPin || !DestOutPin)
    {
        Context.SendError(TEXT("Destination stack link is invalid."), TEXT("NIAGARA_GRAPH_INVALID"));
        return true;
    }
    DestOutPin->BreakLinkTo(DestInPin);
    DestOutPin->MakeLinkTo(Moved.HeadInPin);
    Moved.TailOutPin->MakeLinkTo(DestInPin);

    Graph->NotifyGraphChanged();
    System->RequestCompile(false);
    MarkDirtyAndVerify(Context, System);
    Context.Result->SetStringField(TEXT("moduleName"), ModuleName);
    Context.Result->SetStringField(TEXT("stage"), StackStageName(Output->GetUsage()));
    Context.Result->SetNumberField(TEXT("previousIndex"), CurrentIndex);
    Context.Result->SetNumberField(TEXT("targetIndex"), TargetIndex);
    Context.Result->SetBoolField(TEXT("moved"), true);
    Context.SendSuccess(true, FString::Printf(TEXT("Moved '%s' from index %d to %d in %s."), *ModuleName, CurrentIndex, TargetIndex, StackStageName(Output->GetUsage())));
    return true;
}

bool HandleSystemEmitterAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("create_niagara_system")) return CreateNiagaraSystem(Context);
    if (SubAction == TEXT("create_niagara_emitter")) return CreateNiagaraEmitter(Context);
    if (SubAction == TEXT("add_emitter_to_system")) return AddEmitterToSystem(Context);
    if (SubAction == TEXT("set_emitter_properties")) return SetEmitterProperties(Context);
    if (SubAction == TEXT("remove_niagara_module")) return RemoveNiagaraModule(Context);
    if (SubAction == TEXT("reorder_niagara_module")) return ReorderNiagaraModule(Context);
    return false;
}
}
