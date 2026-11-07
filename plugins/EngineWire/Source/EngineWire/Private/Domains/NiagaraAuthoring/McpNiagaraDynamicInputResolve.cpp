#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"
#include "EdGraphSchema_Niagara.h"

#if MCP_HAS_NIAGARA_INPUT_OVERRIDES
namespace McpNiagaraAuthoringHandlers
{
FString BareInputName(const FString& InputName)
{
    int32 DotIndex = INDEX_NONE;
    return InputName.FindLastChar(TEXT('.'), DotIndex) ? InputName.Mid(DotIndex + 1) : InputName;
}

// A function or dynamic-input script declares each input as an input node. A module script reads its inputs from
// the parameter map as "Module.<Name>" pins instead, the only form most modules (InitializeParticle) have.
bool FindModuleInput(UNiagaraNodeFunctionCall* Module, const FString& InputName, FNiagaraVariable& OutInput)
{
    UNiagaraGraph* CalledGraph = Module ? Module->GetCalledGraph() : nullptr;
    if (!CalledGraph)
    {
        return false;
    }
    const FString Bare = BareInputName(InputName);
    // A Set Variables module's input keeps the whole variable name, Module.Particles.Position. A name left with such a
    // namespace after any module prefix means exactly that input: its last part alone would read another module's
    // Module.Position.
    FString Relative = InputName;
    if (!Relative.RemoveFromStart(TEXT("Module."), ESearchCase::IgnoreCase) &&
        !Relative.RemoveFromStart(Module->GetFunctionName() + TEXT("."), ESearchCase::IgnoreCase) &&
        Module->FunctionScript)
    {
        Relative.RemoveFromStart(Module->FunctionScript->GetName() + TEXT("."), ESearchCase::IgnoreCase);
    }
    const FString FirstPart = Relative.Left(Relative.Find(TEXT(".")));
    const bool bVariableName = Relative.Contains(TEXT(".")) && (FirstPart.Equals(TEXT("Particles"), ESearchCase::IgnoreCase) ||
        FirstPart.Equals(TEXT("Emitter"), ESearchCase::IgnoreCase) || FirstPart.Equals(TEXT("System"), ESearchCase::IgnoreCase) ||
        FirstPart.Equals(TEXT("Engine"), ESearchCase::IgnoreCase) || FirstPart.Equals(TEXT("User"), ESearchCase::IgnoreCase) ||
        FirstPart.Equals(TEXT("Local"), ESearchCase::IgnoreCase) || FirstPart.Equals(TEXT("Transient"), ESearchCase::IgnoreCase) ||
        FirstPart.Equals(TEXT("StackContext"), ESearchCase::IgnoreCase) || FirstPart.Equals(TEXT("Output"), ESearchCase::IgnoreCase));
    for (UEdGraphNode* Node : bVariableName ? TArray<UEdGraphNode*>() : TArray<UEdGraphNode*>(CalledGraph->Nodes))
    {
        UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
        const FString Candidate = InputNode ? InputNode->Input.GetName().ToString() : FString();
        if (InputNode && (Candidate.Equals(InputName, ESearchCase::IgnoreCase) ||
            Candidate.Equals(Bare, ESearchCase::IgnoreCase) ||
            Candidate.EndsWith(TEXT(".") + Bare, ESearchCase::IgnoreCase)))
        {
            OutInput = InputNode->Input;
            return true;
        }
    }
    TArray<FString> ModuleForms{TEXT("Module.") + Relative};
    if (!bVariableName)
    {
        ModuleForms.AddUnique(TEXT("Module.") + Bare);
    }
    for (const FString& ModuleForm : ModuleForms)
    {
        for (UEdGraphNode* Node : CalledGraph->Nodes)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin->Direction == EGPD_Output && Pin->PinName.ToString().Equals(ModuleForm, ESearchCase::IgnoreCase))
                {
                    OutInput = FNiagaraVariable(UEdGraphSchema_Niagara::PinToTypeDefinition(Pin), Pin->PinName);
                    return true;
                }
            }
        }
    }
    return false;
}

// "RandomRangeFloat.Minimum": an input of a dynamic input that feeds a module input, named by the dynamic input's
// own function name as get_niagara_info lists it. Only module calls are searched elsewhere.
UNiagaraNodeFunctionCall* FindDynamicInputCallInput(UNiagaraGraph* Graph, const FString& InputName, FNiagaraVariable& OutInput)
{
    int32 DotIndex = INDEX_NONE;
    if (!Graph || !InputName.FindChar(TEXT('.'), DotIndex))
    {
        return nullptr;
    }
    const FString Prefix = InputName.Left(DotIndex);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node);
        if (Call && Call->GetCalledUsage() == ENiagaraScriptUsage::DynamicInput && Call->GetFunctionName().Equals(Prefix, ESearchCase::IgnoreCase) &&
            FindModuleInput(Call, InputName, OutInput))
        {
            return Call;
        }
    }
    return nullptr;
}

FString ListModuleInputs(UNiagaraNodeFunctionCall* Module)
{
    TArray<FString> Names;
    UNiagaraGraph* CalledGraph = Module ? Module->GetCalledGraph() : nullptr;
    if (!CalledGraph)
    {
        return FString();
    }
    for (UEdGraphNode* Node : CalledGraph->Nodes)
    {
        if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
        {
            Names.AddUnique(InputNode->Input.GetName().ToString());
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output && Pin->PinName.ToString().StartsWith(TEXT("Module.")))
            {
                Names.AddUnique(Pin->PinName.ToString());
            }
        }
    }
    return FString::Join(Names, TEXT(", "));
}

// targetNodeId is optional (the contract only requires systemPath + inputName): without it
// the module is resolved by moduleName / targetModuleName, the "Module.Input" prefix of
// inputName, or - when exactly one module exposes the input - by the input alone (dogfood #105).
UNiagaraNodeFunctionCall* ResolveDynamicInputTargetNode(
    FActionContext& Context,
    UNiagaraGraph* Graph,
    const FString& TargetNodeId,
    const FString& InputName,
    FString& OutError,
    FString& OutErrorCode)
{
    TArray<UNiagaraNodeFunctionCall*> Modules;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UNiagaraNodeFunctionCall* FuncCall = Cast<UNiagaraNodeFunctionCall>(Node);
        if (FuncCall && FuncCall->GetCalledUsage() == ENiagaraScriptUsage::Module)
        {
            Modules.Add(FuncCall);
        }
    }

    if (!TargetNodeId.IsEmpty())
    {
        FGuid TargetGuid;
        if (!FGuid::Parse(TargetNodeId, TargetGuid))
        {
            OutError = TEXT("Invalid 'targetNodeId' GUID format.");
            OutErrorCode = TEXT("INVALID_ARGUMENT");
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UNiagaraNodeFunctionCall* FuncCall = Cast<UNiagaraNodeFunctionCall>(Node);
            if (FuncCall && FuncCall->NodeGuid == TargetGuid)
            {
                return FuncCall;
            }
        }
        OutError = FString::Printf(TEXT("Target node '%s' not found."), *TargetNodeId);
        OutErrorCode = TEXT("NODE_NOT_FOUND");
        return nullptr;
    }

    FString ModuleName = GetJsonStringField(Context.Payload, TEXT("moduleName"));
    if (ModuleName.IsEmpty())
    {
        ModuleName = GetJsonStringField(Context.Payload, TEXT("targetModuleName"));
    }
    int32 DotIndex = INDEX_NONE;
    if (ModuleName.IsEmpty() && InputName.FindChar(TEXT('.'), DotIndex))
    {
        // Only a module's name is a module prefix: Particles.Position is itself a Set Variables input's name.
        const FString Prefix = InputName.Left(DotIndex);
        for (const UNiagaraNodeFunctionCall* Module : Modules)
        {
            if (Module->GetFunctionName().Equals(Prefix, ESearchCase::IgnoreCase) ||
                (Module->FunctionScript && Module->FunctionScript->GetName().Equals(Prefix, ESearchCase::IgnoreCase)))
            {
                ModuleName = Prefix;
                break;
            }
        }
    }

    TArray<UNiagaraNodeFunctionCall*> Candidates;
    FString Available;
    FString NamedModuleInputs; // inputs exposed by modules whose name matched (diagnostic for a wrong inputName)
    for (UNiagaraNodeFunctionCall* Module : Modules)
    {
        const FString FunctionName = Module->GetFunctionName();
        const FString ScriptName = Module->FunctionScript ? Module->FunctionScript->GetName() : FString();
        if (!Available.IsEmpty())
        {
            Available += TEXT(", ");
        }
        Available += FString::Printf(TEXT("%s (%s)"), *FunctionName, *Module->NodeGuid.ToString());
        const bool bNameMatches = ModuleName.IsEmpty() ||
            FunctionName.Equals(ModuleName, ESearchCase::IgnoreCase) ||
            ScriptName.Equals(ModuleName, ESearchCase::IgnoreCase);
        // A module whose function graph is not loaded cannot list its inputs; trust an explicit module name.
        const bool bGraphUnavailable = !ModuleName.IsEmpty() && Module->GetCalledGraph() == nullptr;
        if (bNameMatches && !ModuleName.IsEmpty())
        {
            const FString Inputs = ListModuleInputs(Module);
            NamedModuleInputs += (NamedModuleInputs.IsEmpty() || Inputs.IsEmpty() ? TEXT("") : TEXT(", ")) + Inputs;
        }
        FNiagaraVariable Matched;
        if (bNameMatches && (FindModuleInput(Module, InputName, Matched) || bGraphUnavailable))
        {
            Candidates.Add(Module);
        }
    }
    if (Candidates.Num() == 1)
    {
        Context.Result->SetStringField(
            TEXT("targetNodeResolvedBy"), ModuleName.IsEmpty() ? TEXT("inputName") : TEXT("moduleName"));
        return Candidates[0];
    }

    const FString Problem = Candidates.Num() == 0
        ? FString::Printf(TEXT("No module%s exposes input '%s'"),
            ModuleName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" named '%s'"), *ModuleName), *InputName)
        : FString::Printf(TEXT("%d modules expose input '%s'"), Candidates.Num(), *InputName);
    OutError = FString::Printf(
        TEXT("%s. Accepted: targetNodeId (module node GUID), or moduleName plus inputName ('ModuleName.Input' in inputName also works). Modules on this emitter: %s"),
        *Problem, Available.IsEmpty() ? TEXT("<none>") : *Available);
    if (!NamedModuleInputs.IsEmpty())
    {
        OutError += FString::Printf(TEXT(" Inputs exposed by '%s': %s"), *ModuleName, *NamedModuleInputs);
    }
    OutErrorCode = TEXT("INVALID_ARGUMENT");
    return nullptr;
}
}
#endif
