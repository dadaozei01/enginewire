#include "Domains/BlueprintGraph/Code/McpGraphCodeWrite.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/BlueprintGraph/Code/McpGraphCodeEmit.h"
#include "Domains/BlueprintGraph/McpBPGraphBatchSteps.h"
#include "EdGraph/EdGraph.h"
#include "Foundation/GraphLayout/McpGraphNodeExtent.h"
#include "Foundation/GraphLayout/McpRuled.h"
#include "K2Node_FunctionResult.h"

namespace McpGraphCode
{
namespace Write
{
namespace WriteFormParts
{
// Left of and below everything in the graph: where a made entry starts.
FVector2D FreeSpot(const UEdGraph* Graph)
{
    float Left = TNumericLimits<float>::Max();
    float Bottom = -TNumericLimits<float>::Max();
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        float Width = 0.0f;
        float Height = 0.0f;
        if (Node)
        {
            McpGraphLayout::EstimateNodeExtent(*Node, Width, Height);
            Left = FMath::Min(Left, static_cast<float>(Node->NodePosX));
            Bottom = FMath::Max(Bottom, Node->NodePosY + Height);
        }
    }
    return Bottom > -TNumericLimits<float>::Max() ? FVector2D(Left, Bottom + 200.0f) : FVector2D::ZeroVector;
}

// A form's new nodes laid out in the house style (McpRuled) from its entry; an entry the graph had and kept nodes
// stay where they are, a made entry starts its own region clear of the others, and the old body it replaces is not in
// the way. Returns the lowest bottom edge of the form.
float LayoutForm(UEdGraph* Graph, const TArray<UEdGraphNode*>& Nodes, const TSet<UEdGraphNode*>& Kept, const FVector2D& Spot,
                 const TSet<UEdGraphNode*>& Replaced, bool bEntryMade)
{
    TArray<UEdGraphNode*> Free;
    for (int32 At = bEntryMade ? 0 : 1; At < Nodes.Num(); ++At)
    {
        if (!Kept.Contains(Nodes[At]))
        {
            Free.Add(Nodes[At]);
        }
    }
    float Width = 0.0f;
    float Height = 0.0f;
    McpGraphLayout::EstimateNodeExtent(*Nodes[0], Width, Height);
    return FMath::Max(Nodes[0]->NodePosY + Height, McpRuled::LayoutNodes(Graph, Free, &Spot, nullptr, &Replaced));
}

// Epic's _uses_symbol: the symbol anywhere in the forms.
bool UsesSymbol(const TArray<FSExpr>& Forms, const TCHAR* Name)
{
    return Forms.ContainsByPredicate([Name](const FSExpr& Item)
                                     { return (Item.Type == FSExpr::EType::Symbol && Item.Text.Equals(Name, ESearchCase::CaseSensitive)) || UsesSymbol(Item.Items, Name); });
}
} // namespace WriteFormParts

using namespace WriteFormParts;

UEdGraphNode* FTranspiler::Entry(const FSExpr& Form, UEdGraphNode* Existing)
{
    Line = Form.Line;
    UEdGraphNode* Node = Existing ? Existing : Create(TEXT("AddEvent|") + FormName(Form));
    // An event the Blueprint has on another page comes back from its spawner instead of a new one.
    if (Node && Node->GetGraph() != Graph)
    {
        Fail(FString::Printf(TEXT("%s is in graph %s: write it there (graphName %s)."), *FormName(Form), *Node->GetGraph()->GetName(),
                             *Node->GetGraph()->GetName()));
        return nullptr;
    }
    return Node;
}

// Epic's transpile: the entry's outputs bound by their pin names; an (fn) head's names must be among them (an (event)
// head's list is only skipped); one self node per form whose body uses self.
bool FTranspiler::Body(const FSExpr& Form, UEdGraphNode* InEntry)
{
    Line = Form.Line;
    const int32 Start = BodyStart(Form);
    FBindings Bindings;
    for (UEdGraphPin* Out : DataPins(InEntry, false))
    {
        Bindings.Add(Out->PinName.ToString(), Out);
    }
    TArray<FString> Missing;
    const bool bFunction = Form.Items[0].Text == TEXT("fn");
    for (int32 At = 0; bFunction && At < Form.Items[2].Items.Num(); ++At)
    {
        if (!Bindings.Contains(Form.Items[2].Items[At].Text))
        {
            Missing.Add(Form.Items[2].Items[At].Text);
        }
    }
    if (Missing.Num() > 0)
    {
        return Fail(FString::Printf(TEXT("Function parameter(s) not found in graph: %s (code names a function's inputs; add_function makes them)."),
                                    *FString::Join(Missing, TEXT(", "))));
    }
    const TArray<FSExpr> Statements(Form.Items.GetData() + Start, Form.Items.Num() - Start);
    if (!Bindings.Contains(TEXT("self")) && UsesSymbol(Statements, TEXT("self")))
    {
        const TArray<UEdGraphPin*> Self = DataPins(Create(TEXT("Variables|Getareferencetoself")), false);
        if (Self.Num() == 0)
        {
            return false;
        }
        Bindings.Add(TEXT("self"), Self[0]);
    }
    UEdGraphPin* Then = ExecPin(InEntry, false, FString());
    if (!Then)
    {
        return Fail(FString::Printf(TEXT("%s has no exec output to start a body from."), *FormName(Form)));
    }
    Graph->GetSchema()->BreakPinLinks(*Then, true);
    FFlow Flow{{Then}, nullptr};
    TagForExec.Reset();
    return Stmts(Form.Items, Start, Bindings, Flow);
}

// A return node is matched by class: the menu calls it Add Return Node..., the node itself Return Node. An operator by
// its symbol: (- a b) asks for Utilities|Operators|Subtract, while the node it was read from is Math|Float|float-float.
UEdGraphNode* FTranspiler::Reuse(const FString& TypeId)
{
    if (StatementTag.IsEmpty())
    {
        return nullptr;
    }
    const FString Prefix = StatementTag.Mid(1).ToLower();
    const TCHAR* Operator = Emit::OperatorFor(TypeId);
    UEdGraphNode* Found = nullptr;
    for (UEdGraphNode* Node : Reusable)
    {
        const UK2Node* K2 = Cast<UK2Node>(Node);
        const TCHAR* NodeOperator = K2 && Operator ? Emit::OperatorFor(TypeIdOf(K2)) : nullptr;
        const bool bKind = K2 && (K2->IsA<UK2Node_FunctionResult>() ? TypeId == TEXT("|AddReturnNode...")
                                  : NodeOperator                     ? FCString::Strcmp(NodeOperator, Operator) == 0
                                                                     : MenuTitleKey(TypeIdOf(K2)) == MenuTitleKey(TypeId));
        if (bKind && Node->NodeGuid.ToString().ToLower().StartsWith(Prefix))
        {
            Found = Node;
            break;
        }
    }
    if (!Found)
    {
        return nullptr;
    }
    StatementTag.Reset();
    Reusable.Remove(Found);
    Kept.Add(Found);
    Graph->GetSchema()->BreakNodeLinks(*Found);
    for (UEdGraphPin* Pin : DataPins(Found, true))
    {
        Graph->GetSchema()->ResetPinToAutogeneratedDefaultValue(Pin, false);
    }
    Made.Add(Found, Line);
    return Found;
}

// A goto names a statement of this code, else a node the graph keeps (an event the code does not rewrite); an old-body
// node about to go is no target.
bool FTranspiler::ResolveGotos()
{
    for (const FGoto& Goto : Gotos)
    {
        Line = Goto.Line;
        UEdGraphNode* const* Claimed = Tagged.Find(Goto.Tag);
        UEdGraphNode* Target = Claimed ? *Claimed : nullptr;
        for (int32 At = 0; !Target && At < Graph->Nodes.Num(); ++At)
        {
            UEdGraphNode* Node = Graph->Nodes[At];
            const bool bStays = Node && !Reusable.Contains(Node);
            Target = bStays && Node->NodeGuid.ToString().ToLower().StartsWith(Goto.Tag.Mid(1).ToLower()) ? Node : nullptr;
        }
        UEdGraphPin* In = Target ? ExecPin(Target, true, Goto.Pin) : nullptr;
        if (!In)
        {
            return Fail(Target ? FString::Printf(TEXT("(goto %s %s): that node has no exec input %s."), *Goto.Tag, *Goto.Pin, *Goto.Pin)
                               : FString::Printf(TEXT("(goto %s): neither this code nor the graph has a node with that id."), *Goto.Tag));
        }
        for (UEdGraphPin* Out : Goto.From)
        {
            if (!Connect(Out, In))
            {
                return false;
            }
        }
    }
    return true;
}

bool BuildForms(UBlueprint* Blueprint, const TArray<FSExpr>& Forms, const TArray<FMenuEntry>& Candidates, const TSet<FString>& Titles,
                FGraphPlan& Plan, TMap<UEdGraphNode*, int32>& Made, TArray<TSharedPtr<FJsonValue>>& FormRows, FString& OutError)
{
    FTranspiler Transpiler(Blueprint, Plan.Graph, MenuEntries(Blueprint, Plan.Graph, Candidates, Titles));
    Transpiler.Reusable.Append(Plan.Old);
    FVector2D Spot = FreeSpot(Plan.Graph);
    TArray<TArray<UEdGraphNode*>> FormNodes;
    for (int32 At = 0; At < Plan.Forms.Num(); ++At)
    {
        const FSExpr& Form = Forms[Plan.Forms[At]];
        TSet<UEdGraphNode*> Earlier;
        Transpiler.Made.GetKeys(Earlier);
        UEdGraphNode* Entry = Transpiler.Entry(Form, Plan.Entries[At]);
        if (!Entry || (BodyStart(Form) < Form.Items.Num() && !Transpiler.Body(Form, Entry)))
        {
            OutError = Transpiler.Error;
            return false;
        }
        TArray<UEdGraphNode*>& Nodes = FormNodes.AddDefaulted_GetRef();
        Nodes.Add(Entry);
        for (const TPair<UEdGraphNode*, int32>& Each : Transpiler.Made)
        {
            if (Each.Key != Entry && !Earlier.Contains(Each.Key))
            {
                Nodes.Add(Each.Key);
            }
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("form"), Form.Items[0].Text + TEXT(" ") + FormName(Form));
        Row->SetStringField(TEXT("graph"), Plan.Graph->GetName());
        Row->SetNumberField(TEXT("nodes"), Nodes.Num() - 1);
        Row->SetBoolField(TEXT("entryMade"), Plan.Entries[At] == nullptr);
        FormRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    if (!Transpiler.ResolveGotos())
    {
        OutError = Transpiler.Error;
        return false;
    }
    Plan.Old.RemoveAll([&Transpiler](UEdGraphNode* Node) { return Transpiler.Kept.Contains(Node); });
    Plan.Kept = Transpiler.Kept.Num();
    // Each form is laid out among the others by region: McpRuled keeps every event's logic RegionGap clear of the rest.
    const TSet<UEdGraphNode*> Replaced(Plan.Old);
    // Forms on entries the graph has first, their logic growing where it is; then the made events, below all of it,
    // so one event's region never ends up in among another's.
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        if (Pass == 1)
        {
            Spot = FreeSpot(Plan.Graph);
        }
        for (int32 At = 0; At < FormNodes.Num(); ++At)
        {
            const bool bMade = Plan.Entries[At] == nullptr;
            if (bMade != (Pass == 1))
            {
                continue;
            }
            const float Bottom = LayoutForm(Plan.Graph, FormNodes[At], Transpiler.Kept, Spot, Replaced, bMade);
            if (bMade)
            {
                Spot.Y = Bottom + 200.0f;
            }
        }
    }
    Made.Append(Transpiler.Made);
    return true;
}
} // namespace Write
} // namespace McpGraphCode
