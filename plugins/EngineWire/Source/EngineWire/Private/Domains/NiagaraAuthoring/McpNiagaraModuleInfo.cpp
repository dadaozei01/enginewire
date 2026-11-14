#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"
#include "EdGraphSchema_Niagara.h"
#include "Algo/Reverse.h"

namespace McpNiagaraAuthoringHandlers
{
#if MCP_HAS_NIAGARA_STACK_GRAPH_UTILITIES
// The parameter-map get and set node classes live in NiagaraEditor's private headers and are not exported, so they
// are told apart by class name.
static bool IsNodeOfClass(const UEdGraphNode* Node, const TCHAR* ClassName)
{
    return Node && Node->GetClass()->GetName() == ClassName;
}

// What a linked input reads: the parameter a parameter-map get hands it (User.Tint), or the dynamic input script
// that computes it.
static FString DescribeInputSource(const UEdGraphPin& Source)
{
    const UEdGraphNode* Node = Source.GetOwningNode();
    if (IsNodeOfClass(Node, TEXT("NiagaraNodeParameterMapGet")))
    {
        return Source.PinName.ToString();
    }
    if (const UNiagaraNodeFunctionCall* Function = Cast<UNiagaraNodeFunctionCall>(Node))
    {
        return TEXT("dynamic input ") + Function->GetFunctionName();
    }
    return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
}

// Where a stack module runs: its stage (the script usage of the output node it chains into), the simulation stage's
// name for a simulation stage, and its place in that stage's stack (0 first). The node graph keeps every stage's
// modules in one list in creation order, so the order and stage are read by walking each output node's parameter-map
// chain back to its input (module calls and the parameter-map sets that hold their overrides alternate on it).
struct FModulePlace
{
    FString Stage;
    FString SimulationStage;
    int32 Index = 0;
};

static UEdGraphPin* ParameterMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }
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

static const TCHAR* StageLabel(ENiagaraScriptUsage Usage)
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

static TMap<const UEdGraphNode*, FModulePlace> MapModulePlaces(UNiagaraGraph* Graph, const FNiagaraEmitterHandle& Handle,
    TMap<FGuid, int32>& OutStageModuleCounts)
{
    TMap<FGuid, FString> SimStageNames;
    if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
    {
        for (const UNiagaraSimulationStageBase* SimStage : Data->GetSimulationStages())
        {
            if (SimStage && SimStage->Script)
            {
                SimStageNames.Add(SimStage->Script->GetUsageId(), SimStage->SimulationStageName.ToString());
            }
        }
    }
    TMap<const UEdGraphNode*, FModulePlace> Places;
    TArray<UNiagaraNodeOutput*> Outputs;
    Graph->GetNodesOfClass(Outputs);
    for (UNiagaraNodeOutput* Output : Outputs)
    {
        TArray<const UEdGraphNode*> Chain;
        UEdGraphPin* In = ParameterMapPin(Output, EGPD_Input);
        TSet<const UEdGraphNode*> Seen;
        while (In && In->LinkedTo.Num() > 0 && In->LinkedTo[0])
        {
            UEdGraphNode* Previous = In->LinkedTo[0]->GetOwningNode();
            if (!Previous || Seen.Contains(Previous))
            {
                break;
            }
            Seen.Add(Previous);
            const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Previous);
            if (Call && Call->FunctionScript && Call->GetCalledUsage() == ENiagaraScriptUsage::Module)
            {
                Chain.Add(Previous);
            }
            In = ParameterMapPin(Previous, EGPD_Input);
        }
        Algo::Reverse(Chain);
        const ENiagaraScriptUsage Usage = Output->GetUsage();
        const FString* SimStage = Usage == ENiagaraScriptUsage::ParticleSimulationStageScript ? SimStageNames.Find(Output->GetUsageId()) : nullptr;
        if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
        {
            OutStageModuleCounts.Add(Output->GetUsageId(), Chain.Num());
        }
        for (int32 Index = 0; Index < Chain.Num(); ++Index)
        {
            FModulePlace& Place = Places.Add(Chain[Index]);
            Place.Stage = StageLabel(Usage);
            Place.SimulationStage = SimStage ? *SimStage : FString();
            Place.Index = Index;
        }
    }
    return Places;
}

// The emitter's simulation stages in run order: name, enabled, what they iterate over, and their iteration count
// (a fixed number, or the parameter it is bound to).
static TArray<TSharedPtr<FJsonValue>> DescribeSimulationStages(const FNiagaraEmitterHandle& Handle, const TMap<FGuid, int32>& ModuleCounts)
{
    TArray<TSharedPtr<FJsonValue>> Stages;
    const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
    if (!Data)
    {
        return Stages;
    }
    for (UNiagaraSimulationStageBase* SimStage : Data->GetSimulationStages())
    {
        if (!SimStage)
        {
            continue;
        }
        TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
        StageObj->SetStringField(TEXT("name"), SimStage->SimulationStageName.ToString());
        StageObj->SetBoolField(TEXT("enabled"), SimStage->bEnabled);
        if (SimStage->Script)
        {
            StageObj->SetStringField(TEXT("usageId"), SimStage->Script->GetUsageId().ToString());
            if (const int32* Count = ModuleCounts.Find(SimStage->Script->GetUsageId()))
            {
                StageObj->SetNumberField(TEXT("moduleCount"), *Count);
            }
        }
        if (UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(SimStage))
        {
            StageObj->SetStringField(TEXT("iterationSource"), StaticEnum<ENiagaraIterationSource>()->GetNameStringByValue(int64(Generic->IterationSource)));
            const FName Bound = Generic->NumIterations.ResolvedParameter.GetName();
            if (!Bound.IsNone())
            {
                StageObj->SetStringField(TEXT("numIterationsBinding"), Bound.ToString());
            }
            if (Generic->NumIterations.HasDefaultValueEditorOnly() && Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
            {
                StageObj->SetNumberField(TEXT("numIterations"), Generic->NumIterations.GetDefaultValue<int32>());
            }
        }
        Stages.Add(MakeShared<FJsonValueObject>(StageObj));
    }
    return Stages;
}

// The stack modules of an emitter by name. A module's override node (the parameter-map set feeding its map input)
// holds one pin per input set in the stack; an input wired there reads a parameter or a dynamic input, while
// moduleInputs only had the literal stored for it, so a linked input showed a stale value. A dynamic input has an
// override node of its own (RandomRangeFloat.Minimum bound to User.SpeedMin is wired there).
void AddEmitterModuleInfo(TSharedPtr<FJsonObject>& EmitterObj, const FNiagaraEmitterHandle& Handle)
{
    UNiagaraScriptSource* ScriptSource = GetEmitterScriptSource(const_cast<FNiagaraEmitterHandle*>(&Handle));
    if (!ScriptSource || !ScriptSource->NodeGraph)
    {
        return;
    }
    const TSharedPtr<FJsonObject>* InputsField = nullptr;
    const TSharedPtr<FJsonObject> Inputs =
        EmitterObj->TryGetObjectField(TEXT("moduleInputs"), InputsField) && InputsField ? *InputsField : MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Modules;
    TMap<FGuid, int32> StageModuleCounts;
    const TMap<const UEdGraphNode*, FModulePlace> Places = MapModulePlaces(ScriptSource->NodeGraph, Handle, StageModuleCounts);
    for (UEdGraphNode* GraphNode : ScriptSource->NodeGraph->Nodes)
    {
        UNiagaraNodeFunctionCall* Module = Cast<UNiagaraNodeFunctionCall>(GraphNode);
        const ENiagaraScriptUsage Usage = Module && Module->FunctionScript ? Module->GetCalledUsage() : ENiagaraScriptUsage::Function;
        if (Usage != ENiagaraScriptUsage::Module && Usage != ENiagaraScriptUsage::DynamicInput)
        {
            continue;
        }
        if (Usage == ENiagaraScriptUsage::Module)
        {
            TSharedPtr<FJsonObject> ModuleObj = McpHandlerUtils::CreateResultObject();
            ModuleObj->SetStringField(TEXT("name"), Module->GetFunctionName());
            ModuleObj->SetBoolField(TEXT("enabled"), Module->IsNodeEnabled());
            if (const FModulePlace* Place = Places.Find(Module))
            {
                ModuleObj->SetStringField(TEXT("stage"), Place->Stage);
                if (!Place->SimulationStage.IsEmpty())
                {
                    ModuleObj->SetStringField(TEXT("simulationStage"), Place->SimulationStage);
                }
                ModuleObj->SetNumberField(TEXT("stageIndex"), Place->Index);
            }
            else
            {
                // On no stage's chain: the stack does not run it.
                ModuleObj->SetStringField(TEXT("stage"), TEXT("Unattached"));
            }
            Modules.Add(MakeShared<FJsonValueObject>(ModuleObj));
        }
        const FString Prefix = Module->GetFunctionName() + TEXT(".");
        for (const UEdGraphPin* MapIn : Module->Pins)
        {
            const UEdGraphNode* Overrides = MapIn->Direction == EGPD_Input && MapIn->LinkedTo.Num() > 0
                ? MapIn->LinkedTo[0]->GetOwningNode() : nullptr;
            if (!IsNodeOfClass(Overrides, TEXT("NiagaraNodeParameterMapSet")))
            {
                continue;
            }
            for (const UEdGraphPin* Pin : Overrides->Pins)
            {
                const FString Key = Pin->PinName.ToString();
                if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0 && Key.StartsWith(Prefix))
                {
                    TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
                    Link->SetStringField(TEXT("linkedTo"), DescribeInputSource(*Pin->LinkedTo[0]));
                    Inputs->SetObjectField(Key, Link);
                }
            }
        }
    }
    // Stack order: stage by stage as the editor shows them, then each stage's own order.
    static const TArray<FString> StageOrder = {TEXT("EmitterSpawn"), TEXT("EmitterUpdate"), TEXT("ParticleSpawn"), TEXT("ParticleUpdate"),
        TEXT("ParticleEvent"), TEXT("ParticleSimulationStage"), TEXT("Other"), TEXT("Unattached")};
    TArray<FString> SimStageOrder;
    for (const TSharedPtr<FJsonValue>& Stage : DescribeSimulationStages(Handle, StageModuleCounts))
    {
        SimStageOrder.Add(Stage->AsObject()->GetStringField(TEXT("name")));
    }
    Modules.StableSort([&](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
    {
        const TSharedPtr<FJsonObject> L = A->AsObject(), R = B->AsObject();
        const int32 LS = StageOrder.IndexOfByKey(L->GetStringField(TEXT("stage"))), RS = StageOrder.IndexOfByKey(R->GetStringField(TEXT("stage")));
        if (LS != RS) return LS < RS;
        FString LSim, RSim;
        L->TryGetStringField(TEXT("simulationStage"), LSim);
        R->TryGetStringField(TEXT("simulationStage"), RSim);
        const int32 LI = SimStageOrder.IndexOfByKey(LSim), RI = SimStageOrder.IndexOfByKey(RSim);
        if (LI != RI) return LI < RI;
        double LIdx = 0, RIdx = 0;
        L->TryGetNumberField(TEXT("stageIndex"), LIdx);
        R->TryGetNumberField(TEXT("stageIndex"), RIdx);
        return LIdx < RIdx;
    });
    EmitterObj->SetNumberField(TEXT("moduleCount"), Modules.Num());
    EmitterObj->SetArrayField(TEXT("modules"), Modules);
    EmitterObj->SetArrayField(TEXT("simulationStages"), DescribeSimulationStages(Handle, StageModuleCounts));
}
#endif

// moduleNames keeps only those modules' inputs and switches ("Collision"): a whole system's dump ran to 15 KB.
void KeepModuleEntries(const TSharedPtr<FJsonObject>& EmitterObj, const TArray<FString>& ModuleNames)
{
    for (const TCHAR* FieldName : {TEXT("moduleInputs"), TEXT("staticSwitches")})
    {
        const TSharedPtr<FJsonObject>* Field = nullptr;
        if (ModuleNames.Num() == 0 || !EmitterObj->TryGetObjectField(FieldName, Field) || !Field)
        {
            continue;
        }
        TArray<FString> Drop;
        for (const auto& Pair : (*Field)->Values)
        {
            const FString Key(Pair.Key.Len(), *Pair.Key);
            const FString Module = Key.Left(Key.Find(TEXT(".")));
            if (!ModuleNames.ContainsByPredicate([&Module](const FString& Name) { return Name.Equals(Module, ESearchCase::IgnoreCase); }))
            {
                Drop.Add(Key);
            }
        }
        for (const FString& Key : Drop)
        {
            (*Field)->RemoveField(Key);
        }
    }
}
}
