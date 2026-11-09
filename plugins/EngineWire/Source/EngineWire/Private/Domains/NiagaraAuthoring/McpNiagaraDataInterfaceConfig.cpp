#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"

#if MCP_HAS_NIAGARA_INPUT_OVERRIDES && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8
#include "EdGraphSchema_Niagara.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Editor/TransBuffer.h"
#include "UObject/UObjectHash.h"
#include "Foundation/BridgeHelpers/Properties/McpPropApply.h"
#include "NiagaraScriptRuntimeData.h"
#include "NiagaraSystemImpl.h"
#include "Safety/McpSafeOperations.h"
#include "ScopedTransaction.h"
#include "UObject/StrongObjectPtr.h"

namespace McpNiagaraAuthoringHandlers
{
namespace
{
bool IsEditableDIProperty(const FProperty* Property)
{
    return Property && Property->HasAnyPropertyFlags(CPF_Edit)
        && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient);
}

TArray<FProperty*> EditableDIProperties(UClass* Class)
{
    TArray<FProperty*> Properties;
    for (TFieldIterator<FProperty> It(Class); It; ++It)
    {
        if (IsEditableDIProperty(*It)) Properties.Add(*It);
    }
    return Properties;
}

FString EditableDINames(UClass* Class)
{
    TArray<FString> Names;
    for (FProperty* Property : EditableDIProperties(Class)) Names.Add(Property->GetName());
    return FString::Join(Names, TEXT(", "));
}

// The editor node's getter is not exported by NiagaraEditor.
UNiagaraDataInterface* NodeDataInterface(UEdGraphNode* Node)
{
    if (!Cast<UNiagaraNodeInput>(Node)) return nullptr;
    FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Node->GetClass(), TEXT("DataInterface"));
    return Property ? Cast<UNiagaraDataInterface>(Property->GetObjectPropertyValue_InContainer(Node)) : nullptr;
}

UNiagaraDataInterface* UserDataInterface(UNiagaraSystem* System, const FName Name)
{
    for (const FNiagaraVariableWithOffset& Variable : System->GetExposedParameters().ReadParameterVariables())
    {
        if (Variable.GetName() == Name && Variable.GetType().IsDataInterface())
            return System->GetExposedParameters().GetDataInterface(Variable);
    }
    return nullptr;
}

struct FDITarget
{
    UNiagaraSystem* System = nullptr;
    UNiagaraGraph* Graph = nullptr;
    UNiagaraNodeFunctionCall* Function = nullptr;
    FNiagaraVariable Input;
    FNiagaraParameterHandle Handle;
    UEdGraphPin* OverridePin = nullptr;
    UNiagaraDataInterface* Instance = nullptr;
    UClass* Class = nullptr;
    FName UserName;
    FString Mode = TEXT("default");
    FString LinkedParameter;
    bool bUserExists = false;
};

// Same value as UEdGraphSchema_Niagara::PinCategoryType. Importing that static FName from NiagaraEditor
// breaks /DELAYLOAD on Win64 (LNK1194: data symbols cannot be delay-loaded).
static const FName NiagaraDIConfigPinCategoryType(TEXT("Type"));

// Read the override without the mutating GetOrCreate helper or the unexported GetStackFunctionInputOverridePin.
UEdGraphPin* FindOverridePin(const FDITarget& Target)
{
    for (UEdGraphPin* Pin : Target.Function->Pins)
    {
        if (Pin->Direction != EGPD_Input || Pin->PinType.PinCategory != NiagaraDIConfigPinCategoryType
            || Pin->PinType.PinSubCategoryObject != FNiagaraTypeDefinition::GetParameterMapDef().GetStruct()) continue;
        if (Pin->LinkedTo.Num() != 1) continue;
        UEdGraphNode* Upstream = Pin->LinkedTo[0]->GetOwningNode();
        if (Upstream->GetClass()->GetFName() != TEXT("NiagaraNodeParameterMapSet")) continue;
        for (UEdGraphPin* Candidate : Upstream->Pins)
        {
            if (Candidate->Direction == EGPD_Input && Candidate->PinName == Target.Handle.GetParameterHandleString()) return Candidate;
        }
    }
    return nullptr;
}

void ReadDIConnection(FDITarget& Target, UEdGraphPin* Pin, bool bOverride)
{
    if (!Pin || Pin->LinkedTo.IsEmpty()) return;
    if (Pin->LinkedTo.Num() != 1) return;
    UEdGraphPin* Source = Pin->LinkedTo[0];
    if (UNiagaraDataInterface* Instance = NodeDataInterface(Source->GetOwningNode()))
    {
        Target.Instance = Instance;
        Target.Mode = bOverride ? TEXT("inline") : TEXT("default");
    }
    else
    {
        Target.Mode = TEXT("linked");
        Target.LinkedParameter = Source->PinName.ToString();
        if (Source->GetOwningNode()->GetClass()->GetFName() == TEXT("NiagaraNodeParameterMapGet"))
            Target.Instance = UserDataInterface(Target.System, Source->PinName);
    }
}

void ReadDefaultDI(FDITarget& Target)
{
    UNiagaraGraph* CalledGraph = Target.Function->GetCalledGraph();
    if (!CalledGraph) return;
    if (const UNiagaraScriptVariable* Variable = CalledGraph->GetScriptVariable(Target.Input.GetName()))
    {
        if (Variable->DefaultMode == ENiagaraDefaultMode::Binding)
        {
            Target.Mode = TEXT("linked");
            Target.LinkedParameter = Variable->DefaultBinding.GetName().ToString();
            Target.Instance = UserDataInterface(Target.System, Variable->DefaultBinding.GetName());
            return;
        }
    }
    // MapGet::GetDefaultPin is not exported. Its reflected persistent-ID map identifies each output's default pin.
    bool bFoundDefault = false;
    for (UEdGraphNode* Node : CalledGraph->Nodes)
    {
        if (Node->GetClass()->GetFName() != TEXT("NiagaraNodeParameterMapGet")) continue;
        FMapProperty* MapProperty = FindFProperty<FMapProperty>(Node->GetClass(), TEXT("PinOutputToPinDefaultPersistentId"));
        if (!MapProperty) continue;
        const FStructProperty* Key = CastField<FStructProperty>(MapProperty->KeyProp);
        const FStructProperty* Value = CastField<FStructProperty>(MapProperty->ValueProp);
        if (!Key || !Value || Key->Struct != TBaseStructure<FGuid>::Get() || Value->Struct != TBaseStructure<FGuid>::Get()) continue;
        FScriptMapHelper Map(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(Node));
        for (UEdGraphPin* Output : Node->Pins)
        {
            if (Output->Direction != EGPD_Output || Output->PinName != Target.Input.GetName()) continue;
            for (int32 Index = 0; Index < Map.GetMaxIndex(); ++Index)
            {
                if (!Map.IsValidIndex(Index) || *reinterpret_cast<const FGuid*>(Map.GetKeyPtr(Index)) != Output->PersistentGuid) continue;
                const FGuid DefaultId = *reinterpret_cast<const FGuid*>(Map.GetValuePtr(Index));
                for (UEdGraphPin* Default : Node->Pins)
                {
                    if (Default->PersistentGuid != DefaultId) continue;
                    FDITarget Candidate = Target;
                    Candidate.Instance = nullptr;
                    Candidate.Mode = TEXT("default");
                    Candidate.LinkedParameter.Reset();
                    ReadDIConnection(Candidate, Default, false);
                    // An unconnected DI default uses the type CDO, exactly as the stack's default-value helper does.
                    if (Default->LinkedTo.IsEmpty()) Candidate.Instance = Target.Class->GetDefaultObject<UNiagaraDataInterface>();
                    if (bFoundDefault && (Target.Instance != Candidate.Instance || Target.LinkedParameter != Candidate.LinkedParameter))
                    {
                        Target.Instance = nullptr;
                        return;
                    }
                    Target.Instance = Candidate.Instance;
                    Target.Mode = Candidate.Mode;
                    Target.LinkedParameter = Candidate.LinkedParameter;
                    bFoundDefault = true;
                }
            }
        }
    }
}

// Match the engine's output traversal without calling NiagaraEditor's unexported stack-node helper.
bool IsInEmitterStack(UNiagaraNodeFunctionCall* Function)
{
    TArray<UEdGraphNode*> Pending{Function};
    TSet<UEdGraphNode*> Seen;
    for (int32 Index = 0; Index < Pending.Num(); ++Index)
    {
        UEdGraphNode* Node = Pending[Index];
        if (Seen.Contains(Node)) continue;
        Seen.Add(Node);
        if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node))
        {
            const ENiagaraScriptUsage Usage = Output->GetUsage();
            return Usage == ENiagaraScriptUsage::EmitterSpawnScript || Usage == ENiagaraScriptUsage::EmitterUpdateScript
                || Usage == ENiagaraScriptUsage::ParticleSpawnScript || Usage == ENiagaraScriptUsage::ParticleUpdateScript
                || Usage == ENiagaraScriptUsage::ParticleEventScript || Usage == ENiagaraScriptUsage::ParticleSimulationStageScript;
        }
        for (UEdGraphPin* Pin : Node->Pins)
            if (Pin->Direction == EGPD_Output)
                for (UEdGraphPin* Link : Pin->LinkedTo) if (Link) Pending.Add(Link->GetOwningNode());
    }
    return false;
}

bool ResolveDITarget(FActionContext& Context, bool bWrite, FDITarget& Target)
{
    const FString ModuleInput = GetJsonStringField(Context.Payload, TEXT("moduleInput"));
    FString UserParameter = GetJsonStringField(Context.Payload, TEXT("userParameter"));
    if (Context.SystemPath.IsEmpty() || (ModuleInput.IsEmpty() == UserParameter.IsEmpty()))
    {
        Context.SendError(TEXT("Provide systemPath and exactly one of moduleInput or userParameter."), TEXT("INVALID_ARGUMENT"));
        return false;
    }
    Target.System = LoadObject<UNiagaraSystem>(nullptr, *Context.SystemPath);
    if (!Target.System)
    {
        Context.SendError(TEXT("Niagara system not found: ") + Context.SystemPath, TEXT("SYSTEM_NOT_FOUND"));
        return false;
    }
    if (RefuseWhileEditorOpen(Context, Target.System)) return false;
    const FString TypeName = GetJsonStringField(Context.Payload, TEXT("dataInterfaceType"));
    UClass* RequestedClass = nullptr;
    if (!TypeName.IsEmpty())
    {
        const FNiagaraTypeDefinition Type = ResolveScratchType(TypeName);
        if (!Type.IsValid() || !Type.IsDataInterface())
        {
            Context.SendError(TEXT("dataInterfaceType must name a loaded concrete Niagara data-interface class, e.g. ArrayFloat or NiagaraDataInterfaceArrayFloat."), TEXT("INVALID_TYPE"));
            return false;
        }
        RequestedClass = Type.GetClass();
    }
    if (!UserParameter.IsEmpty())
    {
        if (!UserParameter.StartsWith(TEXT("User."), ESearchCase::IgnoreCase)) UserParameter = TEXT("User.") + UserParameter;
        else UserParameter = TEXT("User.") + UserParameter.Mid(5);
        Target.UserName = FName(*UserParameter);
        for (const FNiagaraVariableWithOffset& Variable : Target.System->GetExposedParameters().ReadParameterVariables())
        {
            if (Variable.GetName() != Target.UserName && Variable.GetName() != FName(*UserParameter.Mid(5))) continue;
            Target.bUserExists = true;
            Target.UserName = Variable.GetName();
            if (!Variable.GetType().IsDataInterface() || (RequestedClass && RequestedClass != Variable.GetType().GetClass()))
            {
                Context.SendError(TEXT("Existing user parameter has a different type; it will not be replaced."), TEXT("PARAM_TYPE_MISMATCH"));
                return false;
            }
            Target.Class = Variable.GetType().GetClass();
            Target.Instance = Target.System->GetExposedParameters().GetDataInterface(Variable);
            Target.Mode = TEXT("inline");
            break;
        }
        if (!Target.bUserExists)
        {
            if (!bWrite || !RequestedClass)
            {
                Context.SendError(TEXT("User parameter does not exist. Creating it requires dataInterfaceType."), TEXT("PARAM_NOT_FOUND"));
                return false;
            }
            if (!ValidateNiagaraIdentifier(Context, UserParameter.Mid(5), TEXT("userParameter"), true)) return false;
            Target.Class = RequestedClass;
        }
    }
    else
    {
        if (Context.EmitterName.IsEmpty())
        {
            Context.SendError(TEXT("emitterName is required for moduleInput."), TEXT("INVALID_ARGUMENT"));
            return false;
        }
        FNiagaraEmitterHandle* Emitter = ResolveEmitterHandle(Context, Target.System);
        if (!Emitter) return false;
        UNiagaraScriptSource* Source = GetEmitterScriptSource(Emitter);
        Target.Graph = Source ? Source->NodeGraph : nullptr;
        if (!Target.Graph)
        {
            Context.SendError(TEXT("Emitter has no Niagara graph source."), TEXT("NIAGARA_GRAPH_MISSING"));
            return false;
        }
        FString Error, Code;
        Target.Function = ResolveDynamicInputTargetNode(Context, Target.Graph, FString(), ModuleInput, Error, Code);
        if (!Target.Function) { Context.SendError(Error, Code); return false; }
        if (!IsInEmitterStack(Target.Function))
        {
            Context.SendError(TEXT("Module is not connected to a supported emitter stack output."), TEXT("NOT_SUPPORTED"));
            return false;
        }
        if (!FindModuleInput(Target.Function, ModuleInput, Target.Input))
        {
            Context.SendError(TEXT("Input not found. Available: ") + ListModuleInputs(Target.Function), TEXT("INPUT_NOT_FOUND"));
            return false;
        }
        if (!Target.Input.GetType().IsDataInterface())
        {
            Context.SendError(TEXT("Module input is not a data interface."), TEXT("PARAM_TYPE_MISMATCH"));
            return false;
        }
        Target.Class = Target.Input.GetType().GetClass();
        if (RequestedClass && !RequestedClass->IsChildOf(Target.Class))
        {
            Context.SendError(TEXT("dataInterfaceType must be a subclass of the module input type."), TEXT("PARAM_TYPE_MISMATCH"));
            return false;
        }
        const FNiagaraParameterHandle InputHandle(Target.Input.GetName());
        Target.Handle = InputHandle.IsModuleHandle()
            ? FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, Target.Function)
            : FNiagaraParameterHandle(FName(*Target.Function->GetFunctionName()), Target.Input.GetName());
        Target.OverridePin = FindOverridePin(Target);
        if (Target.OverridePin && !Target.OverridePin->LinkedTo.IsEmpty()) ReadDIConnection(Target, Target.OverridePin, true);
        else ReadDefaultDI(Target);
        if (RequestedClass) Target.Class = RequestedClass;
    }
    if (!bWrite && Target.Instance) Target.Class = Target.Instance->GetClass();
    if (!Target.Class || Target.Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        Context.SendError(TEXT("The target requires a concrete data-interface class; supply dataInterfaceType."), TEXT("INVALID_TYPE"));
        return false;
    }
    return true;
}

// Maps/sets use the shared exporter; bound any JSON arrays nested in that representation as well.
TSharedPtr<FJsonValue> BoundDIJson(const TSharedPtr<FJsonValue>& Value, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Lengths)
{
    if (!Value.IsValid()) return MakeShared<FJsonValueNull>();
    if (Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Source = Value->AsArray();
        const int32 Returned = FMath::Min(Source.Num(), 256);
        TSharedPtr<FJsonObject> Length = MakeShared<FJsonObject>();
        Length->SetStringField(TEXT("path"), Path);
        Length->SetNumberField(TEXT("length"), Source.Num());
        Length->SetNumberField(TEXT("returnedLength"), Returned);
        Length->SetBoolField(TEXT("truncated"), Returned < Source.Num());
        Lengths.Add(MakeShared<FJsonValueObject>(Length));
        TArray<TSharedPtr<FJsonValue>> Result;
        for (int32 Index = 0; Index < Returned; ++Index)
            Result.Add(BoundDIJson(Source[Index], FString::Printf(TEXT("%s[%d]"), *Path, Index), Lengths));
        return MakeShared<FJsonValueArray>(Result);
    }
    if (Value->Type == EJson::Object)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        for (const auto& Pair : Value->AsObject()->Values)
            Result->SetField(Pair.Key, BoundDIJson(Pair.Value, Path + TEXT(".") + Pair.Key, Lengths));
        return MakeShared<FJsonValueObject>(Result);
    }
    return Value;
}

// Bound reflected arrays before exporting their elements, including arrays nested in editable structs.
TSharedPtr<FJsonValue> ReadDIProperty(void* Container, FProperty* Property, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Lengths)
{
    if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Container));
        const int32 Returned = FMath::Min(Helper.Num(), 256);
        TSharedPtr<FJsonObject> Length = MakeShared<FJsonObject>();
        Length->SetStringField(TEXT("path"), Path);
        Length->SetNumberField(TEXT("length"), Helper.Num());
        Length->SetNumberField(TEXT("returnedLength"), Returned);
        Length->SetBoolField(TEXT("truncated"), Returned < Helper.Num());
        Lengths.Add(MakeShared<FJsonValueObject>(Length));
        TArray<TSharedPtr<FJsonValue>> Values;
        for (int32 Index = 0; Index < Returned; ++Index)
            Values.Add(ReadDIProperty(Helper.GetRawPtr(Index), Array->Inner, FString::Printf(TEXT("%s[%d]"), *Path, Index), Lengths));
        return MakeShared<FJsonValueArray>(Values);
    }
    if (FStructProperty* Struct = CastField<FStructProperty>(Property))
    {
        // Keep the reflection exporter's vector/color shapes. Other structs expose only editable members.
        const FString Name = Struct->Struct->GetName();
        if (Name != TEXT("Vector") && Name != TEXT("Vector2D") && Name != TEXT("Vector4") && Name != TEXT("LinearColor")
            && Name != TEXT("Color") && Name != TEXT("Quat") && Name != TEXT("Rotator"))
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            void* Data = Struct->ContainerPtrToValuePtr<void>(Container);
            for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
                if (IsEditableDIProperty(*It)) Object->SetField(It->GetName(), ReadDIProperty(Data, *It, Path + TEXT(".") + It->GetName(), Lengths));
            return MakeShared<FJsonValueObject>(Object);
        }
    }
    return BoundDIJson(McpPropertyReflection::ExportPropertyToJsonValueOrText(Container, Property), Path, Lengths);
}

void AddDIReadback(FActionContext& Context, const FDITarget& Target)
{
    UClass* Class = Target.Instance ? Target.Instance->GetClass() : Target.Class;
    Context.Result->SetStringField(TEXT("mode"), Target.Mode);
    if (!Target.LinkedParameter.IsEmpty()) Context.Result->SetStringField(TEXT("linkedParameter"), Target.LinkedParameter);
    Context.Result->SetStringField(TEXT("className"), Class->GetName());
    Context.Result->SetBoolField(TEXT("valuesAvailable"), Target.Instance != nullptr);
    if (!Target.Instance) Context.Result->SetStringField(TEXT("valueNote"), TEXT("This binding/default cannot be resolved to a concrete editor instance; null values are not runtime values."));
    TArray<TSharedPtr<FJsonValue>> Properties;
    for (FProperty* Property : EditableDIProperties(Class))
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Property->GetName());
        Entry->SetStringField(TEXT("type"), McpPropertyReflection::GetPropertyTypeName(Property));
        TArray<TSharedPtr<FJsonValue>> Lengths;
        Entry->SetField(TEXT("value"), Target.Instance ? ReadDIProperty(Target.Instance, Property, Property->GetName(), Lengths) : MakeShared<FJsonValueNull>());
        if (!Lengths.IsEmpty()) Entry->SetArrayField(TEXT("arrayLengths"), Lengths);
        Properties.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Context.Result->SetArrayField(TEXT("properties"), Properties);
}

bool CompileDIChange(FActionContext& Context, UNiagaraSystem* System, bool bForce = false)
{
    System->RequestCompile(bForce);
    System->WaitForCompilationComplete(false, false);
    bool bOk = true;
    TArray<TSharedPtr<FJsonValue>> Reports;
    for (UNiagaraScript* Script : GatherModuleInputScripts(System))
    {
        if (!Script) continue;
        const FNiagaraVMExecutableData& Data = Script->GetVMExecutableData();
        TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
        for (const FNiagaraCompileEvent& Event : Data.LastCompileEvents)
        {
            if (Event.Severity == FNiagaraCompileEventSeverity::Error) Errors.Add(MakeShared<FJsonValueString>(Event.Message));
            if (Event.Severity == FNiagaraCompileEventSeverity::Warning) Warnings.Add(MakeShared<FJsonValueString>(Event.Message));
        }
        if (Errors.IsEmpty() && !Data.ErrorMsg.IsEmpty()) Errors.Add(MakeShared<FJsonValueString>(Data.ErrorMsg));
        const bool bScriptOk = Errors.IsEmpty() && Script->GetLastCompileStatus() != ENiagaraScriptCompileStatus::NCS_Error;
        bOk &= bScriptOk;
        if (!Errors.IsEmpty() || !Warnings.IsEmpty() || !bScriptOk)
        {
            TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("script"), Script->GetPathName());
            Report->SetArrayField(TEXT("errors"), Errors);
            Report->SetArrayField(TEXT("warnings"), Warnings);
            Reports.Add(MakeShared<FJsonValueObject>(Report));
        }
    }
    Context.Result->SetBoolField(TEXT("compileOk"), bOk);
    Context.Result->SetArrayField(TEXT("compiledScripts"), Reports);
    return bOk;
}

bool SyncCompiledDI(FActionContext& Context, FDITarget& Target)
{
    UNiagaraDataInterface* Runtime = FNiagaraEditorUtilities::GetResolvedRuntimeInstanceForEditorDataInterfaceInstance(*Target.System, *Target.Instance);
    if (!Runtime)
    {
        Context.Result->SetBoolField(TEXT("runtimeSynced"), false);
        Context.Result->SetStringField(TEXT("runtimeSyncNote"), TEXT("No compiled runtime instance resolved (the input may be unused). Not saved."));
        return false;
    }
    // Resolved Name comes directly from CachedDefaultDataInterfaces.Name in NiagaraResolveDIHelpers.cpp.
    TSet<FName> Names;
    Target.System->ForEachScriptWithRuntimeData([&](UNiagaraScript* Script, const FNiagaraScriptRuntimeData* Data)
    {
        for (const FNiagaraScriptResolvedDataInterfaceInfo& Info : Data->GetResolvedDataInterfaces())
            if (Info.ResolvedDataInterface == Runtime) Names.Add(Info.Name);
    });
    bool bOk = true;
    TSet<UNiagaraDataInterface*> Updated;
    int32 CachedCount = 0;
    Target.System->ForEachScriptWithRuntimeData([&](UNiagaraScript* Script, const FNiagaraScriptRuntimeData* Data)
    {
        for (const FNiagaraScriptResolvedDataInterfaceInfo& Info : Data->GetResolvedDataInterfaces())
        {
            if (!Names.Contains(Info.Name) || !Info.ResolvedDataInterface || Info.ResolvedDataInterface->GetClass() != Target.Instance->GetClass()) continue;
            UNiagaraDataInterface* Instance = Info.ResolvedDataInterface;
            if (!Updated.Contains(Instance))
            {
                Instance->Modify(false);
                bOk &= Target.Instance->CopyTo(Instance);
                Updated.Add(Instance);
            }
            for (FNiagaraScriptDataInterfaceInfo& Cached : Script->GetCachedDefaultDataInterfaces())
            {
                if (Cached.Name != Info.Name || !Cached.DataInterface || Cached.DataInterface->GetClass() != Target.Instance->GetClass()) continue;
                Cached.DataInterface->Modify(false);
                bOk &= Target.Instance->CopyTo(Cached.DataInterface);
                ++CachedCount;
            }
        }
    });
    bOk &= !Updated.IsEmpty() && CachedCount > 0;
    Context.Result->SetBoolField(TEXT("runtimeSynced"), bOk);
    Context.Result->SetNumberField(TEXT("runtimeInstancesUpdated"), Updated.Num());
    Context.Result->SetNumberField(TEXT("cachedDefaultsUpdated"), CachedCount);
    return bOk;
}

// SendAutomationError discards Result. Use its underlying error response so rollback evidence reaches the caller.
void SendDIError(FActionContext& Context, const FString& Message, const FString& Code)
{
    Context.Result->SetBoolField(TEXT("saved"), false);
    Context.Subsystem->SendAutomationResponse(Context.RequestingSocket, Context.RequestId, false, Message, Context.Result, Code);
}

FString DIGraphState(UNiagaraGraph* Graph)
{
    TArray<FString> Entries;
    if (Graph) for (UEdGraphNode* Node : Graph->Nodes)
    {
        Entries.Add(Node->NodeGuid.ToString() + TEXT(":") + Node->GetClass()->GetPathName());
        for (UEdGraphPin* Pin : Node->Pins)
        {
            FString Entry = Node->NodeGuid.ToString() + TEXT(":") + Pin->PinId.ToString() + TEXT(":") + Pin->PinName.ToString()
                + TEXT(":") + Pin->PinType.PinCategory.ToString() + TEXT(":") + Pin->PinType.PinSubCategory.ToString()
                + TEXT(":") + GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get())
                + FString::Printf(TEXT(":%d:%d:"), int32(Pin->Direction), int32(Pin->PinType.ContainerType))
                + Pin->DefaultValue + TEXT(":") + GetPathNameSafe(Pin->DefaultObject) + TEXT(":") + Pin->DefaultTextValue.ToString();
            TArray<FString> Links;
            for (UEdGraphPin* Link : Pin->LinkedTo) Links.Add(Link->GetOwningNode()->NodeGuid.ToString() + TEXT(":") + Link->PinId.ToString());
            Links.Sort();
            Entries.Add(Entry + TEXT(":") + FString::Join(Links, TEXT(",")));
        }
    }
    Entries.Sort();
    return FString::Join(Entries, TEXT("\n"));
}

// A separate, non-flipping transaction is an object snapshot, not an entry in the user's undo history.
// Restore through UTransBuffer::Undo: graph pin serialization requires its CurrentTransaction context.
struct FDIWriteSnapshot
{
    TSharedRef<FTransaction> Objects = MakeShared<FTransaction>();
    TStrongObjectPtr<UTransactor> EditorHistory;
    TArray<FGuid> HistoryIds;
    int32 HistoryUndoCount = 0;
    FString CaptureError;
    TArray<TStrongObjectPtr<UObject>> KeepAlive;
    TMap<UNiagaraDataInterface*, TStrongObjectPtr<UNiagaraDataInterface>> Interfaces;
    TMap<FString, UNiagaraDataInterface*> Compiled;
    TMap<UPackage*, bool> Dirty;
    TArray<FNiagaraVariableBase> UserVariables;
    TArray<uint8> UserBytes;
    TMap<FName, UNiagaraDataInterface*> UserInterfaces;
    TArray<UEdGraphNode*> Nodes;
    FString GraphState;
    FDITarget Original;

    bool SaveInterface(UNiagaraDataInterface* Instance)
    {
        if (!Instance || Interfaces.Contains(Instance)) return true;
        TStrongObjectPtr<UNiagaraDataInterface> Copy(NewObject<UNiagaraDataInterface>(GetTransientPackage(), Instance->GetClass()));
        if (!Instance->CopyTo(Copy.Get()))
        {
            CaptureError = TEXT("CopyTo failed for ") + Instance->GetPathName() + TEXT(" (") + Instance->GetClass()->GetName() + TEXT(")");
            return false;
        }
        if (!Instance->Equals(Copy.Get()))
        {
            CaptureError = TEXT("Equals mismatch for ") + Instance->GetPathName() + TEXT(" (") + Instance->GetClass()->GetName() + TEXT(")");
            return false;
        }
        KeepAlive.Emplace(Instance);
        Dirty.FindOrAdd(Instance->GetOutermost(), Instance->GetOutermost()->IsDirty());
        Interfaces.Add(Instance, MoveTemp(Copy));
        return true;
    }

    static TMap<FString, UNiagaraDataInterface*> CompiledInterfaces(UNiagaraSystem* System)
    {
        TMap<FString, UNiagaraDataInterface*> Result;
        System->ForEachScriptWithRuntimeData([&](UNiagaraScript* Script, const FNiagaraScriptRuntimeData* Data)
        {
            for (const FNiagaraScriptResolvedDataInterfaceInfo& Info : Data->GetResolvedDataInterfaces())
                Result.Add(Script->GetPathName() + TEXT("|runtime|") + Info.Name.ToString(), Info.ResolvedDataInterface);
        });
        for (UNiagaraScript* Script : GatherModuleInputScripts(System)) if (Script)
            for (const FNiagaraScriptDataInterfaceInfo& Info : Script->GetCachedDefaultDataInterfaces())
                Result.Add(Script->GetPathName() + TEXT("|cached|") + Info.Name.ToString(), Info.DataInterface);
        return Result;
    }

    bool Capture(FDITarget& Target)
    {
        EditorHistory.Reset(GEditor->Trans.Get());
        HistoryUndoCount = EditorHistory->GetUndoCount();
        for (int32 Index = 0; Index < EditorHistory->GetQueueLength(); ++Index)
            HistoryIds.Add(EditorHistory->GetTransaction(Index)->GetId());
        Original = Target;
        GraphState = DIGraphState(Target.Graph);
        if (Target.Graph) for (UEdGraphNode* Node : Target.Graph->Nodes) Nodes.Add(Node);
        TArray<UObject*> Children;
        GetObjectsWithOuter(Target.System, Children, EGetObjectsFlags::IncludeNestedObjects);
        Children.Insert(Target.System, 0);
        for (UObject* Object : Children)
        {
            if (!IsValid(Object)) continue;
            KeepAlive.Emplace(Object);
            Dirty.FindOrAdd(Object->GetOutermost(), Object->GetOutermost()->IsDirty());
            Objects->SaveObject(Object);
            if (!SaveInterface(Cast<UNiagaraDataInterface>(Object))) return false;
        }
        for (const FNiagaraVariableWithOffset& Variable : Target.System->GetExposedParameters().ReadParameterVariables())
        {
            UserVariables.Add(Variable);
            if (Variable.GetType().IsDataInterface()) UserInterfaces.Add(Variable.GetName(), Target.System->GetExposedParameters().GetDataInterface(Variable));
        }
        UserBytes = Target.System->GetExposedParameters().GetParameterDataArray();
        Compiled = CompiledInterfaces(Target.System);
        for (const auto& Pair : Compiled) if (!SaveInterface(Pair.Value)) return false;
        return true;
    }

    bool Restore(FActionContext& Context, FDITarget& Target, TArray<TSharedPtr<FJsonValue>>& Residual)
    {
        auto Failed = [&](const FString& What) { Residual.Add(MakeShared<FJsonValueString>(What)); };
        TArray<UEdGraphNode*> AddedNodes;
        if (Target.Graph) for (UEdGraphNode* Node : Target.Graph->Nodes) if (!Nodes.Contains(Node)) AddedNodes.Add(Node);
        UNiagaraDataInterface* AddedInstance = Target.Instance && Target.Instance != Original.Instance && !Interfaces.Contains(Target.Instance) ? Target.Instance : nullptr;
        {
            // Only the temporary buffer owns this rollback entry. The real buffer was canceled, preserving redo too.
            // Undo registers CurrentTransaction and supplies the operation lifecycle required by SerializePin.
            TStrongObjectPtr<UTransBuffer> RollbackBuffer(NewObject<UTransBuffer>(GetTransientPackage()));
            RollbackBuffer->Initialize(64ull * 1024ull * 1024ull);
            RollbackBuffer->UndoBuffer.Add(Objects);
            TGuardValue<TObjectPtr<UTransactor>> UseRollbackBuffer(GEditor->Trans, TObjectPtr<UTransactor>(RollbackBuffer.Get()));
            if (!RollbackBuffer->Undo(false)) Failed(TEXT("Snapshot undo through the temporary transaction buffer failed."));
        }
        Target = Original;
        for (UEdGraphNode* Node : AddedNodes) Node->MarkAsGarbage();
        if (AddedInstance) AddedInstance->MarkAsGarbage();
        for (const auto& Pair : Interfaces)
            if (!IsValid(Pair.Key) || !Pair.Value->CopyTo(Pair.Key) || !Pair.Key->Equals(Pair.Value.Get()))
                Failed(TEXT("Original DI not restored: ") + GetPathNameSafe(Pair.Key));
        Target.System->GetExposedParameters().Rebind();
        Target.System->GetExposedParameters().OnParameterChange();
        Target.System->GetExposedParameters().OnInterfaceChange();
        if (Target.Function)
        {
            // Runtime lookup handles are derived non-UPROPERTY state. Rebuild them from the restored graph.
            FActionContext RollbackContext = Context;
            RollbackContext.Result = MakeShared<FJsonObject>();
            const bool bCompiled = CompileDIChange(RollbackContext, Target.System, true);
            Context.Result->SetBoolField(TEXT("rollbackCompileOk"), bCompiled);
            Context.Result->SetArrayField(TEXT("rollbackCompiledScripts"), RollbackContext.Result->GetArrayField(TEXT("compiledScripts")));
            if (!bCompiled) Failed(TEXT("Restored system could not be recompiled."));
        }
        const int32 ErrorsBeforeCompiled = Residual.Num();
        // A failed sync can have overwritten only part of either set. Restore and compare every recorded entry.
        const TMap<FString, UNiagaraDataInterface*> Current = CompiledInterfaces(Target.System);
        if (Current.Num() != Compiled.Num()) Failed(TEXT("Compiled DI entry count differs from the original."));
        for (const auto& Pair : Compiled)
        {
            UNiagaraDataInterface* const* Destination = Current.Find(Pair.Key);
            if (!Destination || bool(*Destination) != bool(Pair.Value)) { Failed(Pair.Key); continue; }
            if (!Pair.Value) continue;
            const auto* Backup = Interfaces.Find(Pair.Value);
            if (!Backup || !(*Backup)->CopyTo(*Destination) || !(*Destination)->Equals(Backup->Get())) Failed(Pair.Key);
        }
        Context.Result->SetBoolField(TEXT("runtimeAndCacheRestored"), Residual.Num() == ErrorsBeforeCompiled);
        const bool bGraphRestored = DIGraphState(Target.Graph) == GraphState;
        Context.Result->SetBoolField(TEXT("graphRestored"), bGraphRestored);
        if (!bGraphRestored) Failed(TEXT("Emitter graph nodes, pins or connections differ from the original."));
        TArray<FNiagaraVariableBase> Variables;
        for (const FNiagaraVariableWithOffset& Variable : Target.System->GetExposedParameters().ReadParameterVariables()) Variables.Add(Variable);
        if (Variables != UserVariables || Target.System->GetExposedParameters().GetParameterDataArray() != UserBytes)
            Failed(TEXT("Exposed user parameter definitions or value bytes differ from the original."));
        for (const auto& Pair : UserInterfaces)
            if (UserDataInterface(Target.System, Pair.Key) != Pair.Value) Failed(TEXT("User DI binding differs: ") + Pair.Key.ToString());
        for (const auto& Pair : Interfaces)
            if (!IsValid(Pair.Key) || !Pair.Key->Equals(Pair.Value.Get())) Failed(TEXT("Original DI changed during rollback compilation: ") + GetPathNameSafe(Pair.Key));
        TArray<FGuid> CurrentHistoryIds;
        for (int32 Index = 0; Index < EditorHistory->GetQueueLength(); ++Index)
            CurrentHistoryIds.Add(EditorHistory->GetTransaction(Index)->GetId());
        const bool bHistoryRestored = GEditor->Trans == EditorHistory.Get()
            && CurrentHistoryIds == HistoryIds && EditorHistory->GetUndoCount() == HistoryUndoCount;
        Context.Result->SetBoolField(TEXT("undoHistoryRestored"), bHistoryRestored);
        if (!bHistoryRestored) Failed(TEXT("Editor undo/redo history differs from before the failed operation."));
        if (Residual.IsEmpty())
            for (const auto& Pair : Dirty) Pair.Key->SetDirtyFlag(Pair.Value);
        else Failed(TEXT("Rollback is incomplete; package dirty state was not cleared. No save was attempted."));
        Context.Result->SetBoolField(TEXT("packageDirty"), Target.System->GetOutermost()->IsDirty());
        return Residual.IsEmpty();
    }
};

bool SetDI(FActionContext& Context, FDITarget& Target)
{
    Context.Result->SetBoolField(TEXT("saved"), false);
    Context.Result->SetBoolField(TEXT("rolledBack"), true);
    Context.Result->SetBoolField(TEXT("mutationStarted"), false);
    if (Target.Graph) Context.Result->SetNumberField(TEXT("moduleGraphNodeCountBefore"), Target.Graph->Nodes.Num());
    Context.Result->SetBoolField(TEXT("packageDirty"), Target.System->GetOutermost()->IsDirty());
    const TSharedPtr<FJsonObject>* Properties = nullptr;
    if (!Context.Payload->TryGetObjectField(TEXT("properties"), Properties) || !Properties || !Properties->IsValid())
    {
        SendDIError(Context, TEXT("properties must be an object."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    TStrongObjectPtr<UNiagaraDataInterface> Trial(NewObject<UNiagaraDataInterface>(GetTransientPackage(), Target.Class));
    if (Target.Instance && Target.Instance->GetClass() == Target.Class && !Target.Instance->CopyTo(Trial.Get()))
    {
        SendDIError(Context, TEXT("Could not copy the existing data interface for validation. Nothing was changed."), TEXT("DI_COPY_FAILED"));
        return true;
    }
    TArray<FProperty*> Validated;
    for (const auto& Pair : (*Properties)->Values)
    {
        FProperty* Property = FindFProperty<FProperty>(Target.Class, FName(*Pair.Key));
        if (!IsEditableDIProperty(Property) || Validated.Contains(Property))
        {
            SendDIError(Context, FString::Printf(TEXT("Property '%s' is missing, not editable or repeated. Available: %s. Nothing was changed."), *Pair.Key, *EditableDINames(Target.Class)), TEXT("INVALID_PROPERTY"));
            return true;
        }
        FString Error;
        if (!ApplyJsonValueToProperty(Trial.Get(), Property, Pair.Value, Error))
        {
            SendDIError(Context, FString::Printf(TEXT("Property '%s': %s. Nothing was changed."), *Pair.Key, *Error), TEXT("INVALID_PROPERTY_VALUE"));
            return true;
        }
        Validated.Add(Property);
    }
    if (Target.bUserExists && Target.Instance && Target.Instance->GetClass() != Target.Class)
    {
        SendDIError(Context, TEXT("Existing user instance class differs from its declared type; nothing was changed."), TEXT("PARAM_TYPE_MISMATCH"));
        return true;
    }
    if (Target.bUserExists && !Target.Instance)
    {
        SendDIError(Context, TEXT("Existing user parameter has no data-interface instance; nothing was changed."), TEXT("DI_INSTANCE_MISSING"));
        return true;
    }
    if (!GEditor || !GEditor->CanTransact() || GIsTransacting || !GEditor->Trans || GEditor->Trans->IsActive())
    {
        SendDIError(Context, TEXT("A separate editor transaction is required; nothing was changed."), TEXT("TRANSACTION_UNAVAILABLE"));
        return true;
    }
    FDIWriteSnapshot Snapshot;
    if (!Snapshot.Capture(Target))
    {
        SendDIError(Context, TEXT("Could not capture and verify the original DI state: ") + Snapshot.CaptureError + TEXT(". Nothing was changed."), TEXT("DI_SNAPSHOT_FAILED"));
        return true;
    }
    FScopedTransaction Transaction(NSLOCTEXT("EngineWire", "ConfigureNiagaraDI", "Configure Niagara data interface"));
    Context.Result->SetBoolField(TEXT("mutationStarted"), true);
    Context.Result->RemoveField(TEXT("rolledBack"));
    auto RollbackError = [&](const FString& Message, const FString& Code)
    {
        Transaction.Cancel();
        TArray<TSharedPtr<FJsonValue>> Residual;
        const bool bRestored = Snapshot.Restore(Context, Target, Residual);
        Context.Result->SetBoolField(TEXT("rolledBack"), bRestored);
        Context.Result->SetArrayField(TEXT("residualChanges"), Residual);
        AddDIReadback(Context, Target);
        if (Target.Graph) Context.Result->SetNumberField(TEXT("moduleGraphNodeCount"), Target.Graph->Nodes.Num());
        SendDIError(Context, Message + (bRestored ? TEXT(" Original state restored; not saved.") : TEXT(" Rollback incomplete; inspect residualChanges. Not saved.")), Code);
        return true;
    };
    Target.System->Modify();
    if (!Target.UserName.IsNone())
    {
        if (!Target.bUserExists)
        {
            if (!AddDataInterfaceUserParameter(Target.System, Target.UserName.ToString(), Target.Class))
            {
                return RollbackError(TEXT("Failed to create data-interface user parameter."), TEXT("DI_CREATE_FAILED"));
            }
            Target.Instance = UserDataInterface(Target.System, Target.UserName);
        }
        if (!Target.Instance)
        {
            return RollbackError(TEXT("User parameter creation did not yield a data-interface instance."), TEXT("DI_INSTANCE_MISSING"));
        }
    }
    else
    {
        const bool bReuse = Target.OverridePin && Target.OverridePin->LinkedTo.Num() == 1
            && NodeDataInterface(Target.OverridePin->LinkedTo[0]->GetOwningNode()) == Target.Instance
            && Target.Instance && Target.Instance->GetClass() == Target.Class;
        if (!bReuse)
        {
            Target.Graph->Modify();
            Target.Function->Modify();
            UEdGraphPin& Pin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
                *Target.Function, Target.Handle, Target.Input.GetType(), FGuid(), FGuid());
            ClearNiagaraInputOverrideLinks(Pin);
            if (!Pin.LinkedTo.IsEmpty())
                return RollbackError(TEXT("Override pin still has connections after cleanup."), TEXT("DI_OVERRIDE_LINKS_REMAIN"));
            FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(Pin, Target.Class, Target.Handle.GetParameterHandleString().ToString(), Target.Instance);
            Target.Graph->NotifyGraphChanged();
        }
    }
    if (!Target.Instance)
    {
        return RollbackError(TEXT("Engine did not create the data-interface instance."), TEXT("DI_CREATE_FAILED"));
    }
    Target.Instance->Modify();
    for (FProperty* Property : Validated)
    {
        // Conversion already succeeded on the transient instance. Copying the validated property cannot fail halfway.
        Property->CopyCompleteValue_InContainer(Target.Instance, Trial.Get());
        FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
        Target.Instance->PostEditChangeProperty(Event);
    }
    Target.Instance->OnChanged().Broadcast();
    Target.Mode = TEXT("inline");
    Target.LinkedParameter.Reset();
    bool bCanSave = true;
    if (Target.Function)
    {
        bCanSave = CompileDIChange(Context, Target.System);
        if (!bCanSave)
        {
            Context.Result->SetBoolField(TEXT("runtimeSynced"), false);
            return RollbackError(TEXT("Data-interface change failed compilation."), TEXT("DI_COMPILE_FAILED"));
        }
        if (!SyncCompiledDI(Context, Target))
            return RollbackError(TEXT("Data-interface runtime/cache synchronization failed."), TEXT("DI_SYNC_FAILED"));
    }
    else
    {
        Target.System->GetExposedParameters().OnInterfaceChange();
        Context.Result->SetBoolField(TEXT("compileRequired"), false);
        Context.Result->SetBoolField(TEXT("runtimeSynced"), true);
    }
    Target.System->MarkPackageDirty();
    // Do not use MarkDirtyAndVerify: it requests another compile after the explicit runtime/cache synchronization.
    const bool bSaved = bCanSave && Context.bSave && McpSafeOperations::McpSafeAssetSave(Target.System);
    Context.Result->SetBoolField(TEXT("saved"), bSaved);
    AddDIReadback(Context, Target);
    Context.Result->SetBoolField(TEXT("packageDirty"), Target.System->GetOutermost()->IsDirty());
    if (Target.Graph) Context.Result->SetNumberField(TEXT("moduleGraphNodeCount"), Target.Graph->Nodes.Num());
    Context.Result->SetStringField(TEXT("dataInterfacePath"), Target.Instance->GetPathName());
    Context.SendSuccess(true, TEXT("Data interface configured."));
    return true;
}
}

bool HandleDataInterfaceConfigAction(FActionContext& Context, const FString& SubAction)
{
    const bool bWrite = SubAction == TEXT("set_niagara_data_interface");
    if (!bWrite && SubAction != TEXT("get_niagara_data_interface")) return false;
    FDITarget Target;
    if (!ResolveDITarget(Context, bWrite, Target)) return true;
    if (bWrite) return SetDI(Context, Target);
    AddDIReadback(Context, Target);
    Context.SendSuccess(true, TEXT("Data interface read without creating an override or saving."));
    return true;
}
}
#else
namespace McpNiagaraAuthoringHandlers
{
bool HandleDataInterfaceConfigAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction != TEXT("set_niagara_data_interface") && SubAction != TEXT("get_niagara_data_interface")) return false;
    Context.SendError(TEXT("Data-interface configuration is supported only on UE 5.8."), TEXT("NOT_SUPPORTED"));
    return true;
}
}
#endif
