// System-level Niagara Scratch modules: the scripts the Niagara system editor lists under Local Modules.
//
// The system editor lists only System.ScratchPadScripts (GetOuterAndTargetScripts in NiagaraScratchPadViewModel.cpp);
// a script kept on an emitter's ScratchPads runs but never shows there. So a script made here lives on the system, made
// the way UNiagaraScratchPadViewModel::CreateNewScript makes one, without opening the editor.
//
// NiagaraEditor does not export the node classes this needs: Map Get/Set, If and Select have private headers, and
// UNiagaraNodeWithDynamicPins::RequestNewTypedPin and its StaticClass do not link. Everything below therefore goes through
// reflection (classes by name, properties by name) and virtual dispatch, which need no exported symbol.
//
// A new pin takes the Add pin's slot and a fresh Add pin goes after it, as the engine's RequestNewTypedPin does. Custom
// HLSL depends on that: UNiagaraNodeCustomHlsl::BuildParameterMapHistory indexes its signature by pin position, skipping
// only the Add pin, so a pin after the Add pin reads past the end of the signature and crashes the system editor.
// Its OnNewTypedPinAdded rebuilds the signature from the pins, so AddTypedPin keeps the two in step.
#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"

#if MCP_HAS_NIAGARA_INPUT_OVERRIDES && ENGINE_MINOR_VERSION >= 8

#include "EdGraphSchema_Niagara.h"
#include "Editor.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraTypeRegistry.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectIterator.h"

namespace McpNiagaraAuthoringHandlers
{
// Keep class types out of the shared user-parameter resolver. Creating the CDO lets each interface register its
// own selection flags; registering with None only ensures a type handle exists and does not broaden those flags.
FNiagaraTypeDefinition ResolveScratchType(const FString& Name)
{
    const FNiagaraTypeDefinition ValueType = ResolveNiagaraTypeByName(Name);
    if (ValueType.IsValid()) return ValueType;
    const FString ClassName = Name.StartsWith(TEXT("NiagaraDataInterface"), ESearchCase::IgnoreCase) ? Name : TEXT("NiagaraDataInterface") + Name;
    UClass* Found = nullptr;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetName().Equals(ClassName, ESearchCase::IgnoreCase)
            && It->IsChildOf(UNiagaraDataInterface::StaticClass())
            && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            if (Found) return FNiagaraTypeDefinition();
            Found = *It;
        }
    }
    if (!Found) return FNiagaraTypeDefinition();
    Found->GetDefaultObject();
    const FNiagaraTypeDefinition Type(Found);
    FNiagaraTypeRegistry::Register(Type, ENiagaraTypeRegistryFlags::None);
    return Type;
}

// The graphs the view model duplicates when the system editor opens are the ones edited here, so an open editor would
// keep editing its stale copy and overwrite these edits on its next Apply.
bool RefuseWhileEditorOpen(FActionContext& Context, UNiagaraSystem* System)
{
    UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (Editors && Editors->FindEditorForAsset(System, false))
    {
        Context.SendError(FString::Printf(TEXT("%s is open in the Niagara editor, whose copy of the script would overwrite this edit. Close that editor and call again. Nothing was changed."), *System->GetName()), TEXT("SYSTEM_EDITOR_OPEN"));
        return true;
    }
    return false;
}

const TCHAR* NiagaraUsageText(ENiagaraScriptUsage Usage)
{
    switch (Usage)
    {
    case ENiagaraScriptUsage::Function: return TEXT("Function");
    case ENiagaraScriptUsage::Module: return TEXT("Module");
    case ENiagaraScriptUsage::DynamicInput: return TEXT("DynamicInput");
    case ENiagaraScriptUsage::ParticleSpawnScript: return TEXT("ParticleSpawn");
    case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("ParticleSpawnInterpolated");
    case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("ParticleUpdate");
    case ENiagaraScriptUsage::ParticleEventScript: return TEXT("ParticleEvent");
    case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("ParticleSimulationStage");
    case ENiagaraScriptUsage::ParticleGPUComputeScript: return TEXT("ParticleGPUCompute");
    case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("EmitterSpawn");
    case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("EmitterUpdate");
    case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("SystemSpawn");
    case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("SystemUpdate");
    default: return TEXT("Other");
    }
}

const TCHAR* NiagaraCompileStatusText(ENiagaraScriptCompileStatus Status)
{
    switch (Status)
    {
    case ENiagaraScriptCompileStatus::NCS_Dirty: return TEXT("Dirty");
    case ENiagaraScriptCompileStatus::NCS_Error: return TEXT("Error");
    case ENiagaraScriptCompileStatus::NCS_UpToDate: return TEXT("UpToDate");
    case ENiagaraScriptCompileStatus::NCS_BeingCreated: return TEXT("BeingCreated");
    case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
    case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings: return TEXT("ComputeUpToDateWithWarnings");
    default: return TEXT("Unknown");
    }
}


bool CompileSystemAndCollect(UNiagaraSystem* System, TArray<TSharedPtr<FJsonValue>>& OutReports,
    TFunctionRef<void(const FNiagaraCompileEvent&)> OnError)
{
    System->RequestCompile(false);
    System->WaitForCompilationComplete(false, false);
    bool bCompileOk = true;
    for (UNiagaraScript* Script : GatherModuleInputScripts(System))
    {
        if (!Script)
        {
            continue;
        }
        const FNiagaraVMExecutableData& Data = Script->GetVMExecutableData();
        TArray<TSharedPtr<FJsonValue>> Errors;
        TArray<TSharedPtr<FJsonValue>> Warnings;
        for (const FNiagaraCompileEvent& Event : Data.LastCompileEvents)
        {
            if (Event.Severity != FNiagaraCompileEventSeverity::Error && Event.Severity != FNiagaraCompileEventSeverity::Warning)
            {
                continue;
            }
            (Event.Severity == FNiagaraCompileEventSeverity::Error ? Errors : Warnings).Add(MakeShared<FJsonValueString>(Event.Message));
            if (Event.Severity == FNiagaraCompileEventSeverity::Error)
            {
                OnError(Event);
            }
        }
        if (Errors.IsEmpty() && !Data.ErrorMsg.IsEmpty())
        {
            Errors.Add(MakeShared<FJsonValueString>(Data.ErrorMsg));
        }
        const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
        bCompileOk &= Errors.IsEmpty() && Status != ENiagaraScriptCompileStatus::NCS_Error;
        // Only a script something can be said about: an emitter's many stage scripts are mostly clean.
        if (Errors.Num() > 0 || Warnings.Num() > 0 || Status == ENiagaraScriptCompileStatus::NCS_Error)
        {
            TSharedPtr<FJsonObject> Report = McpHandlerUtils::CreateResultObject();
            Report->SetStringField(TEXT("script"), Script->GetPathName(System->GetOutermost()));
            Report->SetStringField(TEXT("usage"), NiagaraUsageText(Script->GetUsage()));
            Report->SetStringField(TEXT("status"), NiagaraCompileStatusText(Status));
            Report->SetArrayField(TEXT("errors"), Errors);
            Report->SetArrayField(TEXT("warnings"), Warnings);
            OutReports.Add(MakeShared<FJsonValueObject>(Report));
        }
    }
    return bCompileOk;
}

namespace
{

const TCHAR* ScratchTypeHelp()
{
    return TEXT("Use Float, Int, Bool, Vector, Vector2, Vector4, Color, Position or Quat; Scratch inputs also accept a loaded concrete data-interface class, e.g. NiagaraDataInterfaceArrayFloat or ArrayFloat. Data interfaces cannot be outputs.");
}

// Reaches the protected virtual through a member pointer, which holds a vtable slot and so links without the engine
// exporting anything. Never instantiated.
struct FDynamicPinsAccess : UNiagaraNodeWithDynamicPins
{
    static void NewTypedPinAdded(UNiagaraNodeWithDynamicPins* Node, UEdGraphPin*& Pin)
    {
        void (UNiagaraNodeWithDynamicPins::*Added)(UEdGraphPin*&) = &FDynamicPinsAccess::OnNewTypedPinAdded;
        (Node->*Added)(Pin);
    }
};

UClass* NiagaraEditorClass(const TCHAR* Name)
{
    return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/NiagaraEditor.%s"), Name));
}

UNiagaraNodeWithDynamicPins* AsDynamicPinsNode(UEdGraphNode* Node)
{
    static UClass* DynamicClass = NiagaraEditorClass(TEXT("NiagaraNodeWithDynamicPins"));
    return Node && DynamicClass && Node->IsA(DynamicClass) ? static_cast<UNiagaraNodeWithDynamicPins*>(Node) : nullptr;
}

const UEdGraphSchema_Niagara* NiagaraSchema()
{
    return GetDefault<UEdGraphSchema_Niagara>();
}

UEdGraphPin* FindAddPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc && Pin->PinName == TEXT("Add"))
        {
            return Pin;
        }
    }
    return nullptr;
}

// UNiagaraNodeWithDynamicPins::RequestNewTypedPin from what links.
UEdGraphPin* AddTypedPin(UNiagaraNodeWithDynamicPins* Node, EEdGraphPinDirection Direction, const FNiagaraTypeDefinition& Type, FName Name, FString& OutError)
{
    UEdGraphPin* AddPin = FindAddPin(Node, Direction);
    if (!AddPin)
    {
        OutError = FString::Printf(TEXT("This node takes no new %s pins."), Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        return nullptr;
    }
    if (!Node->AllowNiagaraTypeForAddPin(Type))
    {
        OutError = FString::Printf(TEXT("This node takes no %s pin."), *Type.GetName());
        return nullptr;
    }
    Node->Modify();
    const FEdGraphPinType AddPinType = AddPin->PinType;
    AddPin->Modify();
    AddPin->PinType = NiagaraSchema()->TypeDefinitionToPinType(Type);
    AddPin->PinName = Name;
    Node->CreatePin(Direction, AddPinType, TEXT("Add"));
    FDynamicPinsAccess::NewTypedPinAdded(Node, AddPin);
    Node->MarkNodeRequiresSynchronization(TEXT("McpScratchAddPin"), true);
    return AddPin;
}

bool IsCustomHlslNode(const UEdGraphNode* Node)
{
    static UClass* CustomHlslClass = NiagaraEditorClass(TEXT("NiagaraNodeCustomHlsl"));
    return Node && CustomHlslClass && Node->IsA(CustomHlslClass);
}

// The node's code is private and SetCustomHlsl does not link, so this writes the property and reports the change the way
// the details panel does. That rebuilds the pins from the signature; links and values carry over by pin name, but any
// UEdGraphPin* held across this call is stale.
bool WriteCustomHlsl(UEdGraphNode* Node, const FString& Code, FString& OutError)
{
    FStrProperty* Property = CastField<FStrProperty>(Node->GetClass()->FindPropertyByName(TEXT("CustomHlsl")));
    if (!Property)
    {
        OutError = TEXT("This engine's Custom HLSL node has no CustomHlsl property.");
        return false;
    }
    Node->Modify();
    Property->SetPropertyValue_InContainer(Node, Code);
    FPropertyChangedEvent Changed(Property, EPropertyChangeType::ValueSet);
    Node->PostEditChangeProperty(Changed);
    return true;
}

UEdGraphPin* FindMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->Direction == Direction && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
        {
            return Pin;
        }
    }
    return nullptr;
}

UNiagaraGraph* ScriptGraph(UNiagaraScript* Script)
{
    UNiagaraScriptSource* Source = Script ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
    return Source ? Source->NodeGraph : nullptr;
}

UNiagaraNodeInput* FindMapInputNode(UNiagaraGraph* Graph)
{
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UNiagaraNodeInput* Input = Cast<UNiagaraNodeInput>(Node);
        if (Input && Input->Input.GetType() == FNiagaraTypeDefinition::GetParameterMapDef())
        {
            return Input;
        }
    }
    return nullptr;
}

UNiagaraNodeOutput* FindOutputNode(UNiagaraGraph* Graph)
{
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node))
        {
            return Output;
        }
    }
    return nullptr;
}

bool Connect(UEdGraphPin* From, UEdGraphPin* To, FString& OutError)
{
    const UEdGraphSchema_Niagara* Schema = NiagaraSchema();
    const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
    if (Response.Response == CONNECT_RESPONSE_DISALLOW)
    {
        OutError = FString::Printf(TEXT("%s cannot connect to %s: %s"), *From->PinName.ToString(), *To->PinName.ToString(), *Response.Message.ToString());
        return false;
    }
    if (!Schema->TryCreateConnection(From, To))
    {
        OutError = FString::Printf(TEXT("%s did not connect to %s."), *From->PinName.ToString(), *To->PinName.ToString());
        return false;
    }
    return true;
}

UEdGraphNode* NewGraphNode(UNiagaraGraph* Graph, UClass* Class, TFunctionRef<void(UEdGraphNode*)> Configure)
{
    Graph->Modify();
    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, Class, NAME_None, RF_Transactional);
    Graph->AddNode(Node, false, false);
    Configure(Node);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    if (Node->Pins.Num() == 0)
    {
        Node->AllocateDefaultPins();
    }
    return Node;
}

void RemoveGraphNode(UNiagaraGraph* Graph, UEdGraphNode* Node)
{
    Node->Modify();
    Node->BreakAllNodeLinks();
    Graph->RemoveNode(Node);
}

// "Mul" finds Numeric::Mul. The op table does not link, so its names come from the schema's own node menu.
TArray<FName> ListOpNames(UNiagaraGraph* Graph)
{
    TArray<FName> Names;
    // The menu's node templates are throwaways; they go in a transient graph, not in the system's package.
    UEdGraph* Temporaries = NewObject<UEdGraph>(GetTransientPackage());
    for (const TSharedPtr<FNiagaraAction_NewNode>& Action : NiagaraSchema()->GetGraphActions(Graph, nullptr, Temporaries))
    {
        if (const UNiagaraNodeOp* Op = Action.IsValid() ? Cast<UNiagaraNodeOp>(Action->WeakNodeTemplate.Get()) : nullptr)
        {
            Names.AddUnique(Op->OpName);
        }
    }
    Names.Sort(FNameLexicalLess());
    return Names;
}

UEdGraphNode* NewOpNode(UNiagaraGraph* Graph, const FString& OpName, FString& OutError)
{
    const auto Make = [Graph](FName Name)
    {
        return NewGraphNode(Graph, UNiagaraNodeOp::StaticClass(), [Name](UEdGraphNode* Node) { CastChecked<UNiagaraNodeOp>(Node)->OpName = Name; });
    };
    UEdGraphNode* Node = Make(FName(*OpName));
    if (Node->Pins.Num() > 0)
    {
        return Node;
    }
    RemoveGraphNode(Graph, Node);
    TArray<FString> Matches;
    for (const FName& Name : ListOpNames(Graph))
    {
        const FString Candidate = Name.ToString();
        if (Candidate.Equals(OpName, ESearchCase::IgnoreCase) || Candidate.EndsWith(TEXT("::") + OpName, ESearchCase::IgnoreCase))
        {
            Matches.Add(Candidate);
        }
    }
    if (Matches.Num() == 1)
    {
        return Make(FName(*Matches[0]));
    }
    OutError = Matches.Num() > 1
        ? FString::Printf(TEXT("Op '%s' is ambiguous: %s."), *OpName, *FString::Join(Matches, TEXT(", ")))
        : FString::Printf(TEXT("No op named '%s'. get_niagara_scratch_module with listOps lists them (Numeric::Add, Numeric::Mul, ...)."), *OpName);
    return nullptr;
}

UEdGraphNode* NewSelectNode(UNiagaraGraph* Graph, const FNiagaraTypeDefinition& SelectorType, int32 Options, FString& OutError)
{
    UClass* SelectClass = NiagaraEditorClass(TEXT("NiagaraNodeSelect"));
    FStructProperty* SelectorProp = SelectClass ? FindFProperty<FStructProperty>(SelectClass, TEXT("SelectorPinType")) : nullptr;
    FIntProperty* OptionsProp = SelectClass ? FindFProperty<FIntProperty>(SelectClass, TEXT("NumOptionsPerVariable")) : nullptr;
    if (!SelectorProp || !OptionsProp)
    {
        OutError = TEXT("This engine's Select node has no SelectorPinType or NumOptionsPerVariable property.");
        return nullptr;
    }
    return NewGraphNode(Graph, SelectClass, [&](UEdGraphNode* Node)
    {
        *SelectorProp->ContainerPtrToValuePtr<FNiagaraTypeDefinition>(Node) = SelectorType;
        OptionsProp->SetPropertyValue_InContainer(Node, FMath::Max(2, Options));
    });
}

bool ParseStage(const FString& Stage, ENiagaraScriptUsage& OutUsage)
{
    static const TPair<const TCHAR*, ENiagaraScriptUsage> Stages[] = {
        {TEXT("ParticleSpawn"), ENiagaraScriptUsage::ParticleSpawnScript},
        {TEXT("ParticleUpdate"), ENiagaraScriptUsage::ParticleUpdateScript},
        {TEXT("EmitterSpawn"), ENiagaraScriptUsage::EmitterSpawnScript},
        {TEXT("EmitterUpdate"), ENiagaraScriptUsage::EmitterUpdateScript},
        {TEXT("SystemSpawn"), ENiagaraScriptUsage::SystemSpawnScript},
        {TEXT("SystemUpdate"), ENiagaraScriptUsage::SystemUpdateScript},
    };
    for (const auto& Each : Stages)
    {
        if (Stage.Equals(Each.Key, ESearchCase::IgnoreCase))
        {
            OutUsage = Each.Value;
            return true;
        }
    }
    return false;
}


UNiagaraScript* FindScratchScript(UNiagaraSystem* System, const FString& ScriptName)
{
    for (UNiagaraScript* Script : System->ScratchPadScripts)
    {
        if (Script && Script->GetName().Equals(ScriptName, ESearchCase::IgnoreCase))
        {
            return Script;
        }
    }
    return nullptr;
}

FString ListScratchScripts(UNiagaraSystem* System)
{
    TArray<FString> Names;
    for (UNiagaraScript* Script : System->ScratchPadScripts)
    {
        if (Script)
        {
            Names.Add(Script->GetName());
        }
    }
    return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
}

UNiagaraScript* LoadScratchOrError(FActionContext& Context, UNiagaraSystem* System)
{
    const FString ScriptName = GetJsonStringField(Context.Payload, TEXT("scriptName"));
    UNiagaraScript* Script = FindScratchScript(System, ScriptName);
    if (!Script)
    {
        Context.SendError(FString::Printf(TEXT("%s has no system-level scratch script '%s'; it has: %s."), *System->GetName(), *ScriptName, *ListScratchScripts(System)), TEXT("SCRIPT_NOT_FOUND"));
    }
    return Script;
}

FString NodeKindText(const UEdGraphNode* Node)
{
    const FString ClassName = Node->GetClass()->GetName();
    if (ClassName == TEXT("NiagaraNodeParameterMapGet")) return TEXT("map_get");
    if (ClassName == TEXT("NiagaraNodeParameterMapSet")) return TEXT("map_set");
    if (ClassName == TEXT("NiagaraNodeIf")) return TEXT("if");
    if (ClassName == TEXT("NiagaraNodeSelect")) return TEXT("select");
    if (ClassName == TEXT("NiagaraNodeCustomHlsl")) return TEXT("custom_hlsl");
    if (Node->IsA<UNiagaraNodeOp>()) return TEXT("op");
    if (Node->IsA<UNiagaraNodeFunctionCall>()) return TEXT("function");
    if (Node->IsA<UNiagaraNodeInput>()) return TEXT("input");
    if (Node->IsA<UNiagaraNodeOutput>()) return TEXT("output");
    return ClassName;
}

TSharedPtr<FJsonObject> DescribeGraph(UNiagaraGraph* Graph)
{
    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        TSharedPtr<FJsonObject> NodeObj = McpHandlerUtils::CreateResultObject();
        NodeObj->SetStringField(TEXT("node"), Node->NodeGuid.ToString());
        NodeObj->SetStringField(TEXT("kind"), NodeKindText(Node));
        NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        NodeObj->SetNumberField(TEXT("x"), Node->NodePosX);
        NodeObj->SetNumberField(TEXT("y"), Node->NodePosY);
        if (IsCustomHlslNode(Node))
        {
            if (const FStrProperty* Code = CastField<FStrProperty>(Node->GetClass()->FindPropertyByName(TEXT("CustomHlsl"))))
            {
                NodeObj->SetStringField(TEXT("hlsl"), Code->GetPropertyValue_InContainer(Node));
            }
        }
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->bHidden || Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc)
            {
                continue;
            }
            TSharedPtr<FJsonObject> PinObj = McpHandlerUtils::CreateResultObject();
            PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
            PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
            PinObj->SetStringField(TEXT("type"), UEdGraphSchema_Niagara::PinToTypeDefinition(Pin).GetName());
            if (Pin->Direction == EGPD_Input && !Pin->DefaultValue.IsEmpty())
            {
                PinObj->SetStringField(TEXT("default"), Pin->DefaultValue);
            }
            TArray<TSharedPtr<FJsonValue>> Links;
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                Links.Add(MakeShared<FJsonValueString>(Linked->GetOwningNode()->NodeGuid.ToString() + TEXT(".") + Linked->PinName.ToString()));
            }
            if (Links.Num() > 0)
            {
                PinObj->SetArrayField(TEXT("linkedTo"), Links);
            }
            Pins.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        NodeObj->SetArrayField(TEXT("pins"), Pins);
        Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
    }
    TSharedPtr<FJsonObject> Out = McpHandlerUtils::CreateResultObject();
    Out->SetArrayField(TEXT("nodes"), Nodes);
    return Out;
}

// One batch of graph edits; "as" names a node for the batch's later operations.
class FScratchGraphEditor
{
public:
    FScratchGraphEditor(UNiagaraSystem* InSystem, UNiagaraGraph* InGraph) : System(InSystem), Graph(InGraph)
    {
        Aliases.Add(TEXT("input"), FindMapInputNode(Graph));
        Aliases.Add(TEXT("output"), FindOutputNode(Graph));
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            NextX = FMath::Max(NextX, Node->IsA<UNiagaraNodeOutput>() ? 0 : Node->NodePosX + ColumnWidth);
        }
    }

    bool Apply(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        const FString Name = GetJsonStringField(Op, TEXT("op"));
        if (Name == TEXT("declare_input")) return DeclareParameter(Op, true, OutError);
        if (Name == TEXT("declare_output")) return DeclareParameter(Op, false, OutError);
        if (Name == TEXT("add_node")) return AddNode(Op, OutError) != nullptr;
        if (Name == TEXT("add_pin")) return AddPin(Op, OutError);
        if (Name == TEXT("set_hlsl")) return SetHlsl(Op, OutError);
        if (Name == TEXT("connect")) return ConnectPins(Op, OutError);
        if (Name == TEXT("disconnect")) return DisconnectPins(Op, OutError);
        if (Name == TEXT("set_default")) return SetDefault(Op, OutError);
        if (Name == TEXT("set_position")) return SetPosition(Op, OutError);
        if (Name == TEXT("remove_node")) return RemoveNode(Op, OutError);
        OutError = FString::Printf(TEXT("Unknown op '%s'."), *Name);
        return false;
    }

    // The result lists every alias this batch knows, so a later call can name the nodes by GUID.
    TSharedPtr<FJsonObject> AliasMap() const
    {
        TSharedPtr<FJsonObject> Out = McpHandlerUtils::CreateResultObject();
        for (const TPair<FString, UEdGraphNode*>& Pair : Aliases)
        {
            if (Pair.Value)
            {
                Out->SetStringField(Pair.Key, Pair.Value->NodeGuid.ToString());
            }
        }
        return Out;
    }

    // The output node ends the row the auto-placed nodes make, so wires run left to right.
    void Finish()
    {
        if (UNiagaraNodeOutput* Output = FindOutputNode(Graph); Output && bAutoPlaced)
        {
            Output->Modify();
            Output->NodePosX = FMath::Max(Output->NodePosX, NextX);
        }
    }

private:
    static constexpr int32 ColumnWidth = 360;

    UEdGraphNode* ResolveNode(const FString& Ref, FString& OutError) const
    {
        if (UEdGraphNode* const* Found = Aliases.Find(Ref))
        {
            if (*Found)
            {
                return *Found;
            }
        }
        FGuid Guid;
        if (FGuid::Parse(Ref, Guid))
        {
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node->NodeGuid == Guid)
                {
                    return Node;
                }
            }
        }
        OutError = FString::Printf(TEXT("No node '%s': name one with its 'as' in this batch, its GUID, input or output."), *Ref);
        return nullptr;
    }

    // "alias.Module.SamplePos": the alias or GUID has no dots, the pin name may have.
    UEdGraphPin* ResolvePin(const FString& Ref, FString& OutError) const
    {
        int32 Dot = INDEX_NONE;
        if (!Ref.FindChar(TEXT('.'), Dot))
        {
            OutError = FString::Printf(TEXT("'%s' is not <node>.<pin>."), *Ref);
            return nullptr;
        }
        UEdGraphNode* Node = ResolveNode(Ref.Left(Dot), OutError);
        if (!Node)
        {
            return nullptr;
        }
        const FString PinName = Ref.Mid(Dot + 1);
        TArray<FString> Names;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
            Names.Add(Pin->PinName.ToString());
        }
        OutError = FString::Printf(TEXT("%s has no pin '%s'; it has: %s."), *Ref.Left(Dot), *PinName, *FString::Join(Names, TEXT(", ")));
        return nullptr;
    }

    void Place(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Op)
    {
        double X = 0, Y = 0;
        const bool bX = Op->TryGetNumberField(TEXT("x"), X);
        const bool bY = Op->TryGetNumberField(TEXT("y"), Y);
        bAutoPlaced |= !bX;
        Node->NodePosX = bX ? FMath::RoundToInt(X) : NextX;
        Node->NodePosY = bY ? FMath::RoundToInt(Y) : 0;
        NextX = FMath::Max(NextX, Node->NodePosX + ColumnWidth);
    }

    bool AddPinsFromList(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Op, const TCHAR* Field, EEdGraphPinDirection Direction, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
        if (!Op->TryGetArrayField(Field, List))
        {
            return true;
        }
        for (const TSharedPtr<FJsonValue>& Each : *List)
        {
            const TSharedPtr<FJsonObject>* PinObj = nullptr;
            if (!Each.IsValid() || !Each->TryGetObject(PinObj))
            {
                OutError = FString::Printf(TEXT("'%s' holds {name, type} objects."), Field);
                return false;
            }
            if (!AddPinNamed(Node, GetJsonStringField(*PinObj, TEXT("name")), GetJsonStringField(*PinObj, TEXT("type")), Direction, OutError))
            {
                return false;
            }
        }
        return true;
    }

    UEdGraphPin* AddPinNamed(UEdGraphNode* Node, const FString& Name, const FString& TypeName, EEdGraphPinDirection Direction, FString& OutError)
    {
        const FNiagaraTypeDefinition Type = ResolveScratchType(TypeName);
        if (!Type.IsValid())
        {
            OutError = FString::Printf(TEXT("Unknown type '%s'. %s"), *TypeName, ScratchTypeHelp());
            return nullptr;
        }
        if (Type.IsDataInterface()
            && !(IsCustomHlslNode(Node) && Direction == EGPD_Input)
            && !(Node->GetClass()->GetName() == TEXT("NiagaraNodeParameterMapGet") && Direction == EGPD_Output))
        {
            OutError = TEXT("Data interfaces are supported only as module inputs (Map Get) or Custom HLSL input pins, not as declared outputs, Custom HLSL outputs, or op/if/select pins.");
            return nullptr;
        }
        UNiagaraNodeWithDynamicPins* Dynamic = AsDynamicPinsNode(Node);
        if (!Dynamic)
        {
            OutError = TEXT("This node takes no new pins.");
            return nullptr;
        }
        if (Name.IsEmpty() && FindMapPin(Node, EGPD_Input))
        {
            OutError = TEXT("A map pin needs a 'name' (Module.Lateral, Particles.Position).");
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == Direction && !Name.IsEmpty() && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                OutError = FString::Printf(TEXT("The node already has a pin '%s'."), *Name);
                return nullptr;
            }
        }
        const bool bCustomHlsl = IsCustomHlslNode(Node);
        if (bCustomHlsl && !CheckCustomHlslPinName(Dynamic, Name, Direction, OutError))
        {
            return nullptr;
        }
        UEdGraphPin* Pin = AddTypedPin(Dynamic, Direction, Type, FName(*Name), OutError);
        if (Pin && bCustomHlsl)
        {
            // OnNewTypedPinAdded marks the pin for renaming, which would open its name box when the graph is next shown.
            Dynamic->CancelEditablePinName(FText::GetEmpty(), Pin);
        }
        return Pin;
    }

    // The engine would quietly rename a pin it dislikes, and code that uses the name asked for would then not compile. The
    // Add pin stands in for the new pin: the check needs a pin to leave out, and the Add pin's name is not free anyway.
    bool CheckCustomHlslPinName(UNiagaraNodeWithDynamicPins* Node, const FString& Name, EEdGraphPinDirection Direction, FString& OutError)
    {
        if (Name.IsEmpty())
        {
            OutError = TEXT("A Custom HLSL pin needs a 'name'; the code uses it as a variable.");
            return false;
        }
        const UEdGraphPin* AddPin = FindAddPin(Node, Direction);
        FText Refusal;
        // Through the base class: the check is public on UNiagaraNode and protected on UNiagaraNodeWithDynamicPins.
        if (AddPin && !static_cast<UNiagaraNode*>(Node)->VerifyEditablePinName(FText::FromString(Name), Refusal, AddPin))
        {
            OutError = FString::Printf(TEXT("Pin name '%s' refused: %s"), *Name, *Refusal.ToString());
            return false;
        }
        return true;
    }

    bool ValidatePinTypes(const TSharedPtr<FJsonObject>& Op, const TCHAR* Field, bool bAllowDataInterface, FString& OutError)
    {
        const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
        if (!Op->TryGetArrayField(Field, List))
        {
            return true;
        }
        for (const TSharedPtr<FJsonValue>& Each : *List)
        {
            const TSharedPtr<FJsonObject>* PinObj = nullptr;
            if (!Each.IsValid() || !Each->TryGetObject(PinObj))
            {
                OutError = FString::Printf(TEXT("'%s' holds {name, type} objects."), Field);
                return false;
            }
            const FString TypeName = GetJsonStringField(*PinObj, TEXT("type"));
            const FNiagaraTypeDefinition Type = ResolveScratchType(TypeName);
            if (!Type.IsValid())
            {
                OutError = FString::Printf(TEXT("Unknown type '%s'. %s"), *TypeName, ScratchTypeHelp());
                return false;
            }
            if (Type.IsDataInterface() && !bAllowDataInterface)
            {
                OutError = TEXT("Data interfaces are supported only as module inputs (Map Get) or Custom HLSL input pins, not as declared outputs, Custom HLSL outputs, or op/if/select pins.");
                return false;
            }
        }
        return true;
    }

    UEdGraphNode* AddNode(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        const FString Kind = GetJsonStringField(Op, TEXT("kind"));
        const bool bCustomHlsl = Kind == TEXT("custom_hlsl");
        // Map Set joins the map chain as it is created, so reject invalid pin types before creating any node.
        if (!ValidatePinTypes(Op, TEXT("pins"), Kind == TEXT("map_get"), OutError)
            || !ValidatePinTypes(Op, TEXT("inputs"), bCustomHlsl, OutError)
            || !ValidatePinTypes(Op, TEXT("outputs"), false, OutError))
        {
            return nullptr;
        }
        UEdGraphNode* Node = nullptr;
        EEdGraphPinDirection PinDirection = EGPD_Output;
        if (Kind == TEXT("map_get") || Kind == TEXT("map_set"))
        {
            const bool bGet = Kind == TEXT("map_get");
            PinDirection = bGet ? EGPD_Output : EGPD_Input;
            Node = NewMapNode(bGet, OutError);
        }
        else if (Kind == TEXT("op"))
        {
            Node = NewOpNode(Graph, GetJsonStringField(Op, TEXT("opName")), OutError);
        }
        else if (Kind == TEXT("if"))
        {
            UClass* IfClass = NiagaraEditorClass(TEXT("NiagaraNodeIf"));
            Node = IfClass ? NewGraphNode(Graph, IfClass, [](UEdGraphNode*) {}) : nullptr;
            OutError = IfClass ? OutError : FString(TEXT("This engine has no If node class."));
        }
        else if (Kind == TEXT("select"))
        {
            const FNiagaraTypeDefinition SelectorType = ResolveNiagaraTypeByName(GetJsonStringField(Op, TEXT("selectorType"), TEXT("Bool")));
            if (SelectorType != FNiagaraTypeDefinition::GetBoolDef() && SelectorType != FNiagaraTypeDefinition::GetIntDef())
            {
                OutError = TEXT("selectorType must be Bool or Int.");
                return nullptr;
            }
            Node = NewSelectNode(Graph, SelectorType, static_cast<int32>(GetJsonNumberField(Op, TEXT("options"), 2)), OutError);
        }
        else if (Kind == TEXT("function"))
        {
            Node = NewFunctionNode(GetJsonStringField(Op, TEXT("functionScriptPath")), OutError);
        }
        else if (bCustomHlsl)
        {
            Node = NewCustomHlslNode(Op, OutError);
        }
        else
        {
            OutError = FString::Printf(TEXT("Unknown node kind '%s': map_get, map_set, op, if, select, function or custom_hlsl."), *Kind);
        }
        if (!Node)
        {
            return nullptr;
        }
        Place(Node, Op);
        const FString Alias = GetJsonStringField(Op, TEXT("as"));
        if (!Alias.IsEmpty())
        {
            Aliases.Add(Alias, Node);
        }
        if (bCustomHlsl)
        {
            // Pins first: writing the code rebuilds the pins from the signature, which then holds them all.
            if (AddPinsFromList(Node, Op, TEXT("inputs"), EGPD_Input, OutError)
                && AddPinsFromList(Node, Op, TEXT("outputs"), EGPD_Output, OutError)
                && WriteCustomHlsl(Node, GetJsonStringField(Op, TEXT("hlsl")), OutError))
            {
                return Node;
            }
            Aliases.Remove(Alias);
            RemoveGraphNode(Graph, Node);
            return nullptr;
        }
        return AddPinsFromList(Node, Op, TEXT("pins"), PinDirection, OutError) ? Node : nullptr;
    }

    UEdGraphNode* NewCustomHlslNode(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        if (GetJsonStringField(Op, TEXT("hlsl")).IsEmpty())
        {
            OutError = TEXT("kind custom_hlsl needs 'hlsl', the function's body.");
            return nullptr;
        }
        if (Op->HasField(TEXT("pins")))
        {
            OutError = TEXT("kind custom_hlsl takes 'inputs' and 'outputs', not 'pins'.");
            return nullptr;
        }
        UClass* Class = NiagaraEditorClass(TEXT("NiagaraNodeCustomHlsl"));
        if (!Class)
        {
            OutError = TEXT("This engine has no Custom HLSL node class.");
            return nullptr;
        }
        return NewGraphNode(Graph, Class, [](UEdGraphNode*) {});
    }

    UEdGraphNode* NewMapNode(bool bGet, FString& OutError)
    {
        UClass* Class = NiagaraEditorClass(bGet ? TEXT("NiagaraNodeParameterMapGet") : TEXT("NiagaraNodeParameterMapSet"));
        UNiagaraNodeInput* MapInput = FindMapInputNode(Graph);
        UNiagaraNodeOutput* Output = FindOutputNode(Graph);
        UEdGraphPin* MapOut = MapInput ? FindMapPin(MapInput, EGPD_Output) : nullptr;
        UEdGraphPin* OutputIn = Output ? FindMapPin(Output, EGPD_Input) : nullptr;
        if (!Class || !MapOut || !OutputIn)
        {
            OutError = TEXT("The script's graph has no parameter map input or output.");
            return nullptr;
        }
        UEdGraphNode* Node = NewGraphNode(Graph, Class, [](UEdGraphNode*) {});
        UEdGraphPin* NodeIn = FindMapPin(Node, EGPD_Input);
        UEdGraphPin* NodeOut = FindMapPin(Node, EGPD_Output);
        if (bGet)
        {
            // A Get reads the map the script receives; connect its map pin elsewhere to read what a Set wrote.
            return Connect(MapOut, NodeIn, OutError) ? Node : nullptr;
        }
        // A Set joins the chain just before the output, so the writes run in the order they were added.
        UEdGraphPin* Upstream = OutputIn->LinkedTo.Num() > 0 ? OutputIn->LinkedTo[0] : MapOut;
        return Connect(Upstream, NodeIn, OutError) && Connect(NodeOut, OutputIn, OutError) ? Node : nullptr;
    }

    UEdGraphNode* NewFunctionNode(const FString& Path, FString& OutError)
    {
        UNiagaraScript* Script = Path.Contains(TEXT("/")) ? LoadObject<UNiagaraScript>(nullptr, *Path, nullptr, LOAD_NoWarn) : FindScratchScript(System, Path);
        if (!Script)
        {
            OutError = FString::Printf(TEXT("functionScriptPath '%s' is neither a function script's path nor a scratch script of this system."), *Path);
            return nullptr;
        }
        if (Script->GetUsage() != ENiagaraScriptUsage::Function)
        {
            OutError = FString::Printf(TEXT("%s is a %s script, not a function."), *Script->GetName(), NiagaraUsageText(Script->GetUsage()));
            return nullptr;
        }
        return NewGraphNode(Graph, UNiagaraNodeFunctionCall::StaticClass(), [Script](UEdGraphNode* Node)
        {
            UNiagaraNodeFunctionCall* Call = CastChecked<UNiagaraNodeFunctionCall>(Node);
            Call->FunctionScript = Script;
            Call->SelectedScriptVersion = Script->GetExposedVersion().VersionGuid;
        });
    }

    UEdGraphNode* FindOrAddMapNode(bool bGet, FString& OutError)
    {
        const FString ClassName = bGet ? TEXT("NiagaraNodeParameterMapGet") : TEXT("NiagaraNodeParameterMapSet");
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node->GetClass()->GetName() == ClassName)
            {
                return Node;
            }
        }
        UEdGraphNode* Node = NewMapNode(bGet, OutError);
        if (Node)
        {
            Place(Node, McpHandlerUtils::CreateResultObject());
            Aliases.Add(bGet ? TEXT("inputs") : TEXT("outputs"), Node);
        }
        return Node;
    }

    // A module input is a Module.<Name> read from the map, an output a write to Output.Module.<Name>: both appear on
    // the module's call once it compiles. A name with its own namespace (Particles.Position) is kept as given.
    bool DeclareParameter(const TSharedPtr<FJsonObject>& Op, bool bInput, FString& OutError)
    {
        FString Name = GetJsonStringField(Op, TEXT("name"));
        if (Name.IsEmpty())
        {
            OutError = TEXT("Missing 'name'.");
            return false;
        }
        if (!Name.Contains(TEXT(".")))
        {
            Name = (bInput ? TEXT("Module.") : TEXT("Output.Module.")) + Name;
        }
        const FNiagaraTypeDefinition Type = ResolveScratchType(GetJsonStringField(Op, TEXT("type")));
        if (Type.IsDataInterface() && !bInput)
        {
            OutError = TEXT("declare_output does not support data interfaces; use declare_input and connect it to a Custom HLSL input.");
            return false;
        }
        const FString NodeRef = GetJsonStringField(Op, TEXT("node"));
        UEdGraphNode* Node = NodeRef.IsEmpty() ? FindOrAddMapNode(bInput, OutError) : ResolveNode(NodeRef, OutError);
        return Node && AddPinNamed(Node, Name, GetJsonStringField(Op, TEXT("type")), bInput ? EGPD_Output : EGPD_Input, OutError);
    }

    bool AddPin(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        UEdGraphNode* Node = ResolveNode(GetJsonStringField(Op, TEXT("node")), OutError);
        if (!Node)
        {
            return false;
        }
        const FString Direction = GetJsonStringField(Op, TEXT("direction"));
        const bool bInput = Direction.IsEmpty() ? FindAddPin(Node, EGPD_Input) != nullptr : Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase);
        return AddPinNamed(Node, GetJsonStringField(Op, TEXT("name")), GetJsonStringField(Op, TEXT("type")), bInput ? EGPD_Input : EGPD_Output, OutError) != nullptr;
    }

    bool SetHlsl(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        UEdGraphNode* Node = ResolveNode(GetJsonStringField(Op, TEXT("node")), OutError);
        if (!Node)
        {
            return false;
        }
        if (!IsCustomHlslNode(Node))
        {
            OutError = FString::Printf(TEXT("'%s' is a %s node, not custom_hlsl."), *GetJsonStringField(Op, TEXT("node")), *NodeKindText(Node));
            return false;
        }
        const FString Code = GetJsonStringField(Op, TEXT("hlsl"));
        if (Code.IsEmpty())
        {
            OutError = TEXT("set_hlsl needs 'hlsl', the function's body.");
            return false;
        }
        return WriteCustomHlsl(Node, Code, OutError);
    }

    bool ResolveWire(const TSharedPtr<FJsonObject>& Op, UEdGraphPin*& OutFrom, UEdGraphPin*& OutTo, FString& OutError)
    {
        OutFrom = ResolvePin(GetJsonStringField(Op, TEXT("from")), OutError);
        OutTo = OutFrom ? ResolvePin(GetJsonStringField(Op, TEXT("to")), OutError) : nullptr;
        return OutTo != nullptr;
    }

    bool ConnectPins(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        UEdGraphPin* From = nullptr;
        UEdGraphPin* To = nullptr;
        return ResolveWire(Op, From, To, OutError) && Connect(From, To, OutError);
    }

    bool DisconnectPins(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        UEdGraphPin* From = nullptr;
        UEdGraphPin* To = nullptr;
        if (!ResolveWire(Op, From, To, OutError))
        {
            return false;
        }
        if (!From->LinkedTo.Contains(To))
        {
            OutError = TEXT("Those pins are not connected.");
            return false;
        }
        NiagaraSchema()->BreakSinglePinLink(From, To);
        return true;
    }

    bool SetDefault(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        UEdGraphPin* Pin = ResolvePin(GetJsonStringField(Op, TEXT("pin")), OutError);
        if (!Pin)
        {
            return false;
        }
        if (Pin->Direction != EGPD_Input || Pin->bNotConnectable)
        {
            OutError = FString::Printf(TEXT("%s is no input with a value of its own."), *Pin->PinName.ToString());
            return false;
        }
        NiagaraSchema()->TrySetDefaultValue(*Pin, GetJsonStringField(Op, TEXT("value")));
        return true;
    }

    bool SetPosition(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        UEdGraphNode* Node = ResolveNode(GetJsonStringField(Op, TEXT("node")), OutError);
        if (Node)
        {
            Node->Modify();
            Node->NodePosX = FMath::RoundToInt(GetJsonNumberField(Op, TEXT("x"), Node->NodePosX));
            Node->NodePosY = FMath::RoundToInt(GetJsonNumberField(Op, TEXT("y"), Node->NodePosY));
        }
        return Node != nullptr;
    }

    bool RemoveNode(const TSharedPtr<FJsonObject>& Op, FString& OutError)
    {
        UEdGraphNode* Node = ResolveNode(GetJsonStringField(Op, TEXT("node")), OutError);
        if (!Node)
        {
            return false;
        }
        if (Node->IsA<UNiagaraNodeInput>() || Node->IsA<UNiagaraNodeOutput>())
        {
            OutError = TEXT("The script's map input and output stay.");
            return false;
        }
        // Keep the map chain whole around a removed Set, as the graph's own node deletion leaves it for the user to fix.
        UEdGraphPin* In = FindMapPin(Node, EGPD_Input);
        UEdGraphPin* Out = FindMapPin(Node, EGPD_Output);
        UEdGraphPin* Upstream = In && In->LinkedTo.Num() > 0 ? In->LinkedTo[0] : nullptr;
        TArray<UEdGraphPin*> Downstream = Out ? Out->LinkedTo : TArray<UEdGraphPin*>();
        RemoveGraphNode(Graph, Node);
        for (UEdGraphPin* Pin : Downstream)
        {
            if (Upstream && !Connect(Upstream, Pin, OutError))
            {
                return false;
            }
        }
        for (auto It = Aliases.CreateIterator(); It; ++It)
        {
            if (It.Value() == Node)
            {
                It.RemoveCurrent();
            }
        }
        return true;
    }

    UNiagaraSystem* System = nullptr;
    UNiagaraGraph* Graph = nullptr;
    TMap<FString, UEdGraphNode*> Aliases;
    int32 NextX = 0;
    bool bAutoPlaced = false;
};

// Replies itself when an op fails: the ops before it stay applied and the reply says which ran.
bool RunOperations(FActionContext& Context, UNiagaraSystem* System, UNiagaraGraph* Graph, bool bRequired)
{
    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Context.Payload->TryGetArrayField(TEXT("operations"), Ops))
    {
        if (bRequired)
        {
            Context.SendError(TEXT("Missing 'operations'."), TEXT("INVALID_ARGUMENT"));
        }
        return !bRequired;
    }
    FScratchGraphEditor Editor(System, Graph);
    int32 Index = 0;
    for (const TSharedPtr<FJsonValue>& Each : *Ops)
    {
        const TSharedPtr<FJsonObject>* Op = nullptr;
        FString Error = TEXT("Each operation is an object.");
        if (!Each.IsValid() || !Each->TryGetObject(Op) || !Editor.Apply(*Op, Error))
        {
            Editor.Finish();
            Graph->NotifyGraphChanged();
            Context.SendError(FString::Printf(TEXT("operations[%d] (%s) failed: %s The %d before it are applied but not saved; get_niagara_scratch_module shows the graph."),
                Index, Op ? *GetJsonStringField(*Op, TEXT("op")) : TEXT("?"), *Error, Index), TEXT("OPERATION_FAILED"));
            return false;
        }
        ++Index;
    }
    Editor.Finish();
    Graph->NotifyGraphChanged();
    Context.Result->SetObjectField(TEXT("aliases"), Editor.AliasMap());
    Context.Result->SetNumberField(TEXT("operationsApplied"), Index);
    return true;
}

void ReportScratchGraph(FActionContext& Context, UNiagaraScript* Script, UNiagaraGraph* Graph)
{
    Context.Result->SetStringField(TEXT("scriptName"), Script->GetName());
    Context.Result->SetStringField(TEXT("scriptPath"), Script->GetPathName());
    Context.Result->SetArrayField(TEXT("nodes"), DescribeGraph(Graph)->GetArrayField(TEXT("nodes")));
}

// The stack's module calls cache the script's inputs; refreshing them is what the editor's Apply does.
void RefreshModuleCalls(UNiagaraScript* Script)
{
    for (TObjectIterator<UNiagaraNodeFunctionCall> It; It; ++It)
    {
        if (It->FunctionScript == Script && !It->HasAnyFlags(RF_ClassDefaultObject))
        {
            It->RefreshFromExternalChanges();
            It->MarkNodeRequiresSynchronization(TEXT("McpScratchApply"), true);
        }
    }
}

static bool CreateScratchModule(FActionContext& Context)
{
    const FString Name = GetJsonStringField(Context.Payload, TEXT("name"));
    const FString StageName = GetJsonStringField(Context.Payload, TEXT("stage"), TEXT("ParticleUpdate"));
    ENiagaraScriptUsage Stage = ENiagaraScriptUsage::Module;
    if (Name.IsEmpty())
    {
        Context.SendError(TEXT("Missing 'name'. Nothing was changed."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    // Any other stage is an emitter's simulation stage, by the name get_niagara_info lists under simulationStages
    // ("SimulationStage:<Name>" also works). Its stack is the output node with the stage script's usage id.
    FString SimStageName;
    if (!ParseStage(StageName, Stage))
    {
        SimStageName = StageName;
        SimStageName.RemoveFromStart(TEXT("SimulationStage:"), ESearchCase::IgnoreCase);
        SimStageName.TrimStartAndEndInline();
        if (SimStageName.IsEmpty())
        {
            Context.SendError(TEXT("stage must be ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate or the name of one of the emitter's simulation stages. Nothing was changed."), TEXT("INVALID_ARGUMENT"));
            return true;
        }
        Stage = ENiagaraScriptUsage::ParticleSimulationStageScript;
    }
    UNiagaraSystem* System = LoadSystemOrError(Context);
    if (!System || RefuseWhileEditorOpen(Context, System))
    {
        return true;
    }
    if (FindScratchScript(System, Name) || StaticFindObject(UObject::StaticClass(), System, *Name))
    {
        Context.SendError(FString::Printf(TEXT("'%s' is taken in %s. Nothing was changed."), *Name, *System->GetName()), TEXT("NAME_TAKEN"));
        return true;
    }

    // The stack to add it to is found first, so a wrong emitter leaves no orphan script behind.
    bool bAddToStack = true;
    Context.Payload->TryGetBoolField(TEXT("addToStack"), bAddToStack);
    UNiagaraGraph* StackGraph = nullptr;
    FGuid StageUsageId;
    if (!SimStageName.IsEmpty())
    {
        // A simulation stage belongs to one emitter, so it is always looked up, even with addToStack false.
        FNiagaraEmitterHandle* Handle = ResolveEmitterHandle(Context, System);
        if (!Handle)
        {
            return true;
        }
        const FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
        TArray<FString> StageNames;
        static const TArray<UNiagaraSimulationStageBase*> NoStages;
        for (UNiagaraSimulationStageBase* SimStage : Data ? Data->GetSimulationStages() : NoStages)
        {
            if (!SimStage || !SimStage->Script)
            {
                continue;
            }
            StageNames.Add(SimStage->SimulationStageName.ToString());
            if (SimStage->SimulationStageName.ToString().Equals(SimStageName, ESearchCase::IgnoreCase))
            {
                StageUsageId = SimStage->Script->GetUsageId();
                SimStageName = SimStage->SimulationStageName.ToString();
            }
        }
        if (!StageUsageId.IsValid())
        {
            Context.SendError(FString::Printf(TEXT("'%s' is neither a regular stage (ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate) nor a simulation stage of emitter %s; its simulation stages: [%s]. Add one with add_niagara_module moduleKind=simulation_stage. Nothing was changed."),
                *StageName, *Context.EmitterName, *FString::Join(StageNames, TEXT(", "))), TEXT("STAGE_NOT_FOUND"));
            return true;
        }
        UNiagaraScriptSource* Source = GetEmitterScriptSource(Handle);
        StackGraph = bAddToStack && Source ? Source->NodeGraph : nullptr;
    }
    else if (bAddToStack)
    {
        const bool bSystemStage = Stage == ENiagaraScriptUsage::SystemSpawnScript || Stage == ENiagaraScriptUsage::SystemUpdateScript;
        if (bSystemStage)
        {
            StackGraph = ScriptGraph(Stage == ENiagaraScriptUsage::SystemSpawnScript ? System->GetSystemSpawnScript() : System->GetSystemUpdateScript());
        }
        else
        {
            FNiagaraEmitterHandle* Handle = ResolveEmitterHandle(Context, System);
            if (!Handle)
            {
                return true;
            }
            UNiagaraScriptSource* Source = GetEmitterScriptSource(Handle);
            StackGraph = Source ? Source->NodeGraph : nullptr;
        }
    }
    UNiagaraNodeOutput* StackOutput = StackGraph ? StackGraph->FindEquivalentOutputNode(Stage, StageUsageId) : nullptr;
    if (bAddToStack && !StackOutput)
    {
        Context.SendError(FString::Printf(TEXT("No %s stack to add the module to. Nothing was changed."), *StageName), TEXT("STAGE_NOT_FOUND"));
        return true;
    }

    UNiagaraScript* Template = Cast<UNiagaraScript>(GetDefault<UNiagaraEditorSettings>()->DefaultModuleScript.TryLoad());
    if (!ScriptGraph(Template))
    {
        Context.SendError(TEXT("The project's default module script (Niagara settings) did not load. Nothing was changed."), TEXT("TEMPLATE_NOT_FOUND"));
        return true;
    }

    FScopedTransaction Transaction(NSLOCTEXT("EngineWire", "CreateScratchModule", "Create Niagara scratch module"));
    System->Modify();
    UNiagaraScript* Script = CastChecked<UNiagaraScript>(StaticDuplicateObject(Template, System, FName(*Name)));
    Script->ClearFlags(RF_Public | RF_Standalone);
    Script->SetFlags(RF_Transactional);
    Script->GetLatestScriptData()->ModuleUsageBitmask |= (1 << static_cast<int32>(Stage));
    System->ScratchPadScripts.Add(Script);

    // Nothing of the template's example stays: only the map input wired to the output.
    UNiagaraGraph* Graph = ScriptGraph(Script);
    UNiagaraNodeInput* MapInput = FindMapInputNode(Graph);
    UNiagaraNodeOutput* Output = FindOutputNode(Graph);
    UEdGraphPin* MapOut = MapInput ? FindMapPin(MapInput, EGPD_Output) : nullptr;
    UEdGraphPin* OutputIn = Output ? FindMapPin(Output, EGPD_Input) : nullptr;
    if (!MapOut || !OutputIn)
    {
        System->ScratchPadScripts.Remove(Script);
        Context.SendError(TEXT("The default module script has no parameter map input and output. Nothing was changed."), TEXT("TEMPLATE_INVALID"));
        return true;
    }
    TArray<UEdGraphNode*> Others;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node != MapInput && Node != Output)
        {
            Others.Add(Node);
        }
    }
    for (UEdGraphNode* Node : Others)
    {
        RemoveGraphNode(Graph, Node);
    }
    FString Error;
    MapOut->BreakAllPinLinks();
    OutputIn->BreakAllPinLinks();
    MapInput->NodePosX = -360;
    MapInput->NodePosY = 0;
    Output->NodePosX = 0;
    Output->NodePosY = 0;
    if (!Connect(MapOut, OutputIn, Error))
    {
        System->ScratchPadScripts.Remove(Script);
        Context.SendError(Error + TEXT(" Nothing was changed."), TEXT("TEMPLATE_INVALID"));
        return true;
    }

    if (!RunOperations(Context, System, Graph, false))
    {
        return true;
    }
    if (StackOutput)
    {
        UNiagaraNodeFunctionCall* Call = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *StackOutput, INDEX_NONE, Name);
        if (!Call)
        {
            Context.SendError(TEXT("The script was made but the stack refused the module; it is listed under Local Modules. Not saved."), TEXT("CREATE_FAILED"));
            return true;
        }
        Context.Result->SetStringField(TEXT("moduleName"), Call->GetFunctionName());
        Context.Result->SetStringField(TEXT("moduleNodeId"), Call->NodeGuid.ToString());
    }
    System->MarkPackageDirty();
    ReportScratchGraph(Context, Script, Graph);
    Context.Result->SetStringField(TEXT("stage"), SimStageName.IsEmpty() ? StageName : TEXT("ParticleSimulationStage"));
    if (!SimStageName.IsEmpty())
    {
        Context.Result->SetStringField(TEXT("simulationStage"), SimStageName);
    }
    Context.SendSuccess(true, FString::Printf(TEXT("Scratch module %s made on the system. Compile with apply_niagara_scratch_module; nothing is saved until then."), *Script->GetName()));
    return true;
}

static bool EditScratchModule(FActionContext& Context)
{
    UNiagaraSystem* System = LoadSystemOrError(Context);
    if (!System || RefuseWhileEditorOpen(Context, System))
    {
        return true;
    }
    UNiagaraScript* Script = LoadScratchOrError(Context, System);
    UNiagaraGraph* Graph = ScriptGraph(Script);
    if (!Script)
    {
        return true;
    }
    if (!Graph)
    {
        Context.SendError(TEXT("The script has no graph."), TEXT("NIAGARA_GRAPH_MISSING"));
        return true;
    }
    FScopedTransaction Transaction(NSLOCTEXT("EngineWire", "EditScratchModule", "Edit Niagara scratch module"));
    Script->Modify();
    if (!RunOperations(Context, System, Graph, true))
    {
        return true;
    }
    System->MarkPackageDirty();
    ReportScratchGraph(Context, Script, Graph);
    Context.SendSuccess(true, TEXT("Scratch module edited. Compile with apply_niagara_scratch_module; nothing is saved until then."));
    return true;
}

// Which scratch script an event in a compiled script came from: its node, or a module call on its stack trail.
UNiagaraScript* AttributeEvent(const FNiagaraCompileEvent& Event, const TMap<FGuid, UNiagaraScript*>& NodeOwners)
{
    if (UNiagaraScript* const* Owner = NodeOwners.Find(Event.NodeGuid))
    {
        return *Owner;
    }
    for (const FGuid& StackGuid : Event.StackGuids)
    {
        if (UNiagaraScript* const* Owner = NodeOwners.Find(StackGuid))
        {
            return *Owner;
        }
    }
    return nullptr;
}

static bool ApplyScratchModule(FActionContext& Context)
{
    UNiagaraSystem* System = LoadSystemOrError(Context);
    if (!System || RefuseWhileEditorOpen(Context, System))
    {
        return true;
    }
    TArray<UNiagaraScript*> Scratch;
    if (GetJsonStringField(Context.Payload, TEXT("scriptName")).IsEmpty())
    {
        for (UNiagaraScript* Script : System->ScratchPadScripts)
        {
            Scratch.Add(Script);
        }
    }
    else if (UNiagaraScript* Script = LoadScratchOrError(Context, System))
    {
        Scratch.Add(Script);
    }
    else
    {
        return true;
    }

    TMap<FGuid, UNiagaraScript*> NodeOwners;
    for (UNiagaraScript* Script : Scratch)
    {
        RefreshModuleCalls(Script);
        if (UNiagaraGraph* Graph = ScriptGraph(Script))
        {
            Graph->NotifyGraphChanged();
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                NodeOwners.Add(Node->NodeGuid, Script);
            }
        }
    }
    TMap<UNiagaraScript*, TArray<TSharedPtr<FJsonValue>>> ScratchErrors;
    TArray<TSharedPtr<FJsonValue>> ScriptReports;
    const bool bCompileOk = CompileSystemAndCollect(System, ScriptReports, [&](const FNiagaraCompileEvent& Event)
    {
        if (UNiagaraScript* Owner = AttributeEvent(Event, NodeOwners))
        {
            ScratchErrors.FindOrAdd(Owner).Add(MakeShared<FJsonValueString>(Event.Message));
        }
    });

    TArray<TSharedPtr<FJsonValue>> ScratchReports;
    for (UNiagaraScript* Script : Scratch)
    {
        TSharedPtr<FJsonObject> Report = McpHandlerUtils::CreateResultObject();
        Report->SetStringField(TEXT("scriptName"), Script->GetName());
        const TArray<TSharedPtr<FJsonValue>>* Errors = ScratchErrors.Find(Script);
        Report->SetBoolField(TEXT("ok"), Errors == nullptr);
        Report->SetArrayField(TEXT("errors"), Errors ? *Errors : TArray<TSharedPtr<FJsonValue>>());
        ScratchReports.Add(MakeShared<FJsonValueObject>(Report));
    }
    Context.Result->SetBoolField(TEXT("compileOk"), bCompileOk);
    Context.Result->SetArrayField(TEXT("scratchScripts"), ScratchReports);
    Context.Result->SetArrayField(TEXT("compiledScripts"), ScriptReports);

    // A failed compile keeps the disk's last good copy.
    if (bCompileOk)
    {
        MarkDirtyAndVerify(Context, System);
    }
    else
    {
        System->MarkPackageDirty();
        Context.Result->SetBoolField(TEXT("saved"), false);
    }
    Context.SendSuccess(true, bCompileOk ? TEXT("Compiled without errors.") : TEXT("The system compiled with errors; see compiledScripts. Not saved."));
    return true;
}

static bool GetScratchModule(FActionContext& Context)
{
    UNiagaraSystem* System = LoadSystemOrError(Context);
    if (!System)
    {
        return true;
    }
    const FString ScriptName = GetJsonStringField(Context.Payload, TEXT("scriptName"));
    TArray<TSharedPtr<FJsonValue>> Scripts;
    const auto AddScript = [&](UNiagaraScript* Script, const FString& Owner)
    {
        if (!Script)
        {
            return;
        }
        TSharedPtr<FJsonObject> Entry = McpHandlerUtils::CreateResultObject();
        Entry->SetStringField(TEXT("scriptName"), Script->GetName());
        Entry->SetStringField(TEXT("owner"), Owner);
        Entry->SetStringField(TEXT("usage"), NiagaraUsageText(Script->GetUsage()));
        const bool bWanted = Script->GetName().Equals(ScriptName, ESearchCase::IgnoreCase);
        if (UNiagaraGraph* Graph = bWanted ? ScriptGraph(Script) : nullptr)
        {
            Entry->SetArrayField(TEXT("nodes"), DescribeGraph(Graph)->GetArrayField(TEXT("nodes")));
        }
        Scripts.Add(MakeShared<FJsonValueObject>(Entry));
    };
    for (UNiagaraScript* Script : System->ScratchPadScripts)
    {
        AddScript(Script, TEXT("system"));
    }
    // A script on an emitter runs but never shows under Local Modules; it is listed so it can be told apart.
    for (FNiagaraEmitterHandle& Handle : MCP_MUTABLE_EMITTER_HANDLES(System))
    {
        const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
        if (Data && Data->ScratchPads)
        {
            for (UNiagaraScript* Script : Data->ScratchPads->Scripts)
            {
                AddScript(Script, TEXT("emitter:") + Handle.GetName().ToString());
            }
        }
    }
    Context.Result->SetStringField(TEXT("systemPath"), System->GetPathName());
    Context.Result->SetArrayField(TEXT("scripts"), Scripts);
    bool bListOps = false;
    Context.Payload->TryGetBoolField(TEXT("listOps"), bListOps);
    UNiagaraGraph* AnyGraph = System->ScratchPadScripts.Num() > 0 ? ScriptGraph(System->ScratchPadScripts[0]) : ScriptGraph(System->GetSystemUpdateScript());
    if (bListOps && AnyGraph)
    {
        TArray<TSharedPtr<FJsonValue>> Ops;
        for (const FName& Name : ListOpNames(AnyGraph))
        {
            Ops.Add(MakeShared<FJsonValueString>(Name.ToString()));
        }
        Context.Result->SetArrayField(TEXT("ops"), Ops);
    }
    Context.SendSuccess(true, FString::Printf(TEXT("%d scratch script(s)."), Scripts.Num()));
    return true;
}
} // namespace

bool HandleScratchAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("create_niagara_scratch_module")) return CreateScratchModule(Context);
    if (SubAction == TEXT("edit_niagara_scratch_module")) return EditScratchModule(Context);
    if (SubAction == TEXT("apply_niagara_scratch_module")) return ApplyScratchModule(Context);
    if (SubAction == TEXT("get_niagara_scratch_module")) return GetScratchModule(Context);
    return false;
}
}

#else

namespace McpNiagaraAuthoringHandlers
{
bool HandleScratchAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction.EndsWith(TEXT("_niagara_scratch_module")))
    {
        Context.SendError(TEXT("Niagara scratch modules are built for UE 5.8 only."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    return false;
}
}

#endif
