#include "Domains/BlueprintGraph/Code/McpGraphCodeWrite.h"

#include "BlueprintActionFilter.h"
#include "BlueprintTypePromotion.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MathExpression.h"
#include "UObject/UObjectIterator.h"

namespace McpGraphCode
{
namespace Write
{
namespace WriteNodeParts
{
// A Math Expression has no menu entry: read_graph_as_code prints it with its expression as the title ('|' and the text,
// spaces removed), and the write of that text refused it as no node on the menu. A '|' id holding an operator or a call
// is made as one, its inputs in the order the expression names them, as the read printed its arguments.
UEdGraphNode* MathExpression(UEdGraph* Graph, const FString& TypeId)
{
    const FString Expression = TypeId.StartsWith(TEXT("|")) ? TypeId.Mid(1) : FString();
    bool bOperator = false;
    for (const TCHAR Char : Expression)
    {
        bOperator = bOperator || FCString::Strchr(TEXT("()+-*/%<>="), Char) != nullptr;
    }
    if (!bOperator)
    {
        return nullptr;
    }
    FGraphNodeCreator<UK2Node_MathExpression> Creator(*Graph);
    UK2Node_MathExpression* Node = Creator.CreateNode(false);
    Node->Expression = Expression;
    Creator.Finalize();
    Node->ReconstructNode();
    if (!Node->Pins.ContainsByPredicate([](const UEdGraphPin* Pin) { return Pin && Pin->Direction == EGPD_Output && !Pin->bOrphanedPin; }))
    {
        Graph->RemoveNode(Node);
        return nullptr;
    }
    return Node;
}

// An operator function the promotable operator covers (vector * float, vector + vector): the action database leaves its
// own spawner out, so no menu offers it, yet a node made by function reads back with it as its id. The function whose
// category and title make TypeId is made directly, as a plain call: the function spawner makes a promotable operator,
// which picked vector * vector once its A was wired, kept that name after B took a float, and broke links on the way.
UEdGraphNode* PromotableFunction(UEdGraph* Graph, const FString& TypeId)
{
    for (TObjectIterator<UFunction> It; It; ++It)
    {
        const UFunction* Function = *It;
        if (!FTypePromotion::IsFunctionPromotionReady(Function))
        {
            continue;
        }
        const FString Id = CategoryText(UK2Node_CallFunction::GetDefaultCategoryForFunction(Function, FText::GetEmpty())).Replace(TEXT(" "), TEXT("")) +
                           TEXT("|") + SourceText(UK2Node_CallFunction::GetUserFacingFunctionName(Function)).Replace(TEXT(" "), TEXT(""));
        if (Id.Equals(TypeId, ESearchCase::IgnoreCase))
        {
            FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
            UK2Node_CallFunction* Node = Creator.CreateNode(false);
            Node->SetFromFunction(Function);
            Creator.Finalize();
            return Node;
        }
    }
    return nullptr;
}

// Epic's _WILDCARD_LITERAL_NODES: the node that makes a literal for a pin with no type yet.
const TCHAR* LiteralNode(FSExpr::EType Type)
{
    switch (Type)
    {
    case FSExpr::EType::Bool: return TEXT("Math|Boolean|MakeLiteralBool");
    case FSExpr::EType::Int: return TEXT("Math|Integer|MakeLiteralInt");
    case FSExpr::EType::Float: return TEXT("Math|Float|MakeLiteralFloat");
    default: return TEXT("Utilities|String|MakeLiteralString");
    }
}

// Epic's _format_literal: a pin default as Blueprint stores it.
FString LiteralText(const FSExpr& Value)
{
    return Value.Type == FSExpr::EType::Bool ? FString(Value.bTrue ? TEXT("true") : TEXT("false")) : Value.Text;
}

// A loop or gate the action menu does not name: the StandardMacros graph of that title.
UEdGraphNode* StandardMacro(UEdGraph* Graph, const FString& TitleKey)
{
    UBlueprint* Macros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
    UEdGraph* Macro = nullptr;
    if (!Macros)
    {
        return nullptr;
    }
    for (UEdGraph* Each : Macros->MacroGraphs)
    {
        if (Each && Each->GetName().ToLower() == TitleKey)
        {
            Macro = Each;
            break;
        }
    }
    if (!Macro)
    {
        return nullptr;
    }
    FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
    UK2Node_MacroInstance* Node = Creator.CreateNode(false);
    Node->SetMacroGraph(Macro);
    Creator.Finalize();
    return Node;
}
} // namespace WriteNodeParts

using namespace WriteNodeParts;

TArray<UEdGraphPin*> DataPins(UEdGraphNode* Node, bool bInputs)
{
    TArray<UEdGraphPin*> Pins;
    if (!Node)
    {
        return Pins;
    }
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && !Pin->bHidden && (Pin->Direction == EGPD_Input) == bInputs && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
            !(Pin->Direction == EGPD_Output && Pin->PinName == TEXT("OutputDelegate")))
        {
            Pins.Add(Pin);
        }
    }
    return Pins;
}

UEdGraphPin* DataPin(UEdGraphNode* Node, bool bInput, const TCHAR* Name)
{
    for (UEdGraphPin* Pin : DataPins(Node, bInput))
    {
        if (Pin->PinName == Name)
        {
            return Pin;
        }
    }
    return nullptr;
}

UEdGraphPin* ExecPin(UEdGraphNode* Node, bool bInput, const FString& Name)
{
    if (!Node)
    {
        return nullptr;
    }
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && !Pin->bHidden && (Pin->Direction == EGPD_Input) == bInput && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
            (Name.IsEmpty() || Pin->PinName.ToString().Equals(Name, ESearchCase::CaseSensitive)))
        {
            return Pin;
        }
    }
    return nullptr;
}

bool FTranspiler::Fail(const FString& Message)
{
    Error = Line > 0 ? FString::Printf(TEXT("Line %d: %s"), Line, *Message) : Message;
    return false;
}

// Epic's create_node: a named custom event, else the menu entry with that id (any case when no exact one), else a
// StandardMacros graph of that title.
UEdGraphNode* FTranspiler::Create(const FString& TypeId)
{
    if (UEdGraphNode* Reused = Reuse(TypeId))
    {
        return Reused;
    }
    UEdGraphNode* Node = nullptr;
    const FString CustomEvent = TEXT("AddEvent|Custom|");
    if (TypeId.StartsWith(CustomEvent, ESearchCase::CaseSensitive))
    {
        FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
        UK2Node_CustomEvent* Event = Creator.CreateNode(false);
        Event->CustomFunctionName = FName(*TypeId.Mid(CustomEvent.Len()));
        Creator.Finalize();
        Node = Event;
    }
    else
    {
        const FMenuEntry* Found = Menu.FindByPredicate([&TypeId](const FMenuEntry& Entry) { return Entry.Id.Equals(TypeId, ESearchCase::CaseSensitive); });
        Found = Found ? Found : Menu.FindByPredicate([&TypeId](const FMenuEntry& Entry) { return Entry.Id.Equals(TypeId, ESearchCase::IgnoreCase); });
        if (Found)
        {
            const FBlueprintActionInfo Info(Found->Owner, Found->Spawner);
            Node = Found->Spawner->Invoke(Graph, Info.GetBindings(), FVector2D::ZeroVector);
        }
        Node = Node ? Node : StandardMacro(Graph, MenuTitleKey(TypeId));
        Node = Node ? Node : MathExpression(Graph, TypeId);
        Node = Node ? Node : PromotableFunction(Graph, TypeId);
    }
    if (!Node)
    {
        TArray<FString> Offered;
        for (const FMenuEntry& Entry : Menu)
        {
            if (Offered.Num() < 8 && MenuTitleKey(Entry.Id) == MenuTitleKey(TypeId))
            {
                Offered.AddUnique(Entry.Id);
            }
        }
        Fail(Offered.Num() > 0 ? FString::Printf(TEXT("the menu of graph %s has no %s; it offers that title as %s."), *Graph->GetName(), *TypeId, *FString::Join(Offered, TEXT(", ")))
                               : FString::Printf(TEXT("the menu of graph %s has no node %s (a type id is Category|Title, spaces removed, as read_graph_as_code prints it)."), *Graph->GetName(), *TypeId));
        return nullptr;
    }
    Made.Add(Node, Line);
    return Node;
}

bool FTranspiler::Connect(UEdGraphPin* Out, UEdGraphPin* In)
{
    TSet<UEdGraphNode*> Before;
    Before.Reserve(Graph->Nodes.Num());
    for (UEdGraphNode* Existing : Graph->Nodes)
    {
        if (Existing)
        {
            Before.Add(Existing);
        }
    }
    const bool bConnected = Out && In && Graph->GetSchema()->TryCreateConnection(Out, In);
    // Schema auto-conversion (e.g. int -> double) spawns intermediate nodes at (0, 0). Record any newly added
    // nodes on both success and failure so layout, node counts, and compiler-message line attribution include them.
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && !Before.Contains(Node))
        {
            Made.Add(Node, Line);
        }
    }
    if (bConnected)
    {
        return true;
    }
    return Fail(FString::Printf(TEXT("%s.%s cannot be wired to %s.%s."), Out ? *Out->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("?"),
                                Out ? *Out->PinName.ToString() : TEXT("?"), In ? *In->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("?"),
                                In ? *In->PinName.ToString() : TEXT("?")));
}

bool FTranspiler::SetValue(UEdGraphPin* Pin, const FString& Value)
{
    Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
    return true;
}

UEdGraphPin* FTranspiler::MakeLiteral(const FSExpr& Value)
{
    UEdGraphNode* Node = Create(LiteralNode(Value.Type));
    const TArray<UEdGraphPin*> Ins = DataPins(Node, true);
    const TArray<UEdGraphPin*> Outs = DataPins(Node, false);
    if (Ins.Num() > 0)
    {
        SetValue(Ins[0], LiteralText(Value));
    }
    return Outs.Num() > 0 ? Outs[0] : nullptr;
}

// Every exec output the flow waits on into the node's exec input; false when it has none or nothing waits.
bool FTranspiler::WireExec(UEdGraphNode* Node, FFlow& Flow)
{
    UEdGraphPin* In = ExecPin(Node, true, FString());
    if (In && !TagForExec.IsEmpty())
    {
        Tagged.FindOrAdd(TagForExec) = Node;
        TagForExec.Reset();
    }
    if (!In || Flow.Pending.Num() == 0)
    {
        return false;
    }
    for (UEdGraphPin* Out : Flow.Pending)
    {
        if (!Connect(Out, In))
        {
            return false;
        }
    }
    return true;
}

UEdGraphPin* FTranspiler::Bound(const FString& Name, const FBindings& Bindings)
{
    if (UEdGraphPin* const* Pin = Bindings.Find(Name))
    {
        return *Pin;
    }
    TArray<FString> Names;
    Bindings.GetKeys(Names);
    Names.Sort();
    Fail(Name.StartsWith(TEXT("/")) ? FString::Printf(TEXT("\"%s\" looks like a class path or asset reference: quote it, \"%s\"."), *Name, *Name)
                                     : FString::Printf(TEXT("Undefined variable \"%s\". Available: %s."), *Name, *FString::Join(Names, TEXT(", "))));
    return nullptr;
}

bool FTranspiler::WireArg(const FSExpr& Value, UEdGraphPin* Pin, const FBindings& Bindings, FFlow& Flow)
{
    if (Value.Type == FSExpr::EType::Symbol)
    {
        UEdGraphPin* Source = Bound(Value.Text, Bindings);
        return Source && Connect(Source, Pin);
    }
    if (Value.Type == FSExpr::EType::List)
    {
        UEdGraphPin* Out = nullptr;
        return Expr(Value, Bindings, Flow, Out) && (!Out || Connect(Out, Pin));
    }
    // A literal into a pin with no type yet goes through a Make Literal node; others take it as the pin's value.
    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
    {
        UEdGraphPin* Literal = MakeLiteral(Value);
        return Literal && Connect(Literal, Pin);
    }
    return SetValue(Pin, LiteralText(Value));
}

bool FTranspiler::WireWithPre(const FSExpr& Value, UEdGraphPin* Pin, UEdGraphPin* Pre, const FBindings& Bindings, FFlow& Flow)
{
    if (Value.Type == FSExpr::EType::List)
    {
        return !Pre || Connect(Pre, Pin);
    }
    return WireArg(Value, Pin, Bindings, Flow);
}
} // namespace Write
} // namespace McpGraphCode
