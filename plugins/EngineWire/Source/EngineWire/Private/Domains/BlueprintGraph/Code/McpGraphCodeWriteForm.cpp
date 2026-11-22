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
// Left of and below everything settled in the graph: where a made entry starts.
FVector2D FreeSpot(const UEdGraph* Graph, const TSet<UEdGraphNode*>& Ignore)
{
    float Left = TNumericLimits<float>::Max();
    float Bottom = -TNumericLimits<float>::Max();
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Ignore.Contains(Node))
        {
            continue;
        }
        float Width = 0.0f;
        float Height = 0.0f;
        McpGraphLayout::EstimateNodeExtent(*Node, Width, Height);
        Left = FMath::Min(Left, static_cast<float>(Node->NodePosX));
        Bottom = FMath::Max(Bottom, Node->NodePosY + Height);
    }
    return Bottom > -TNumericLimits<float>::Max() ? FVector2D(Left, Bottom + 200.0f) : FVector2D::ZeroVector;
}

// A form's new nodes laid out in the house style (McpRuled) from its entry; an entry the graph had and kept nodes
// stay where they are, a made entry starts its own region clear of the others, and ignored nodes (old bodies about to
// go, or other forms not yet laid out) are not in the way. Returns the lowest bottom edge of the form.
float LayoutForm(UEdGraph* Graph, const TArray<UEdGraphNode*>& Nodes, const TSet<UEdGraphNode*>& Kept, const FVector2D& Spot,
                 const TSet<UEdGraphNode*>& Ignore, bool bEntryMade, const McpRuled::FParams& Params = McpRuled::FParams())
{
    TArray<UEdGraphNode*> Free;
    for (int32 At = bEntryMade ? 0 : 1; At < Nodes.Num(); ++At)
    {
        if (!Kept.Contains(Nodes[At]))
        {
            Free.Add(Nodes[At]);
        }
    }
    const float LaidBottom = McpRuled::LayoutNodes(Graph, Free, &Spot, nullptr, &Ignore, Params);
    float Width = 0.0f;
    float Height = 0.0f;
    McpGraphLayout::EstimateNodeExtent(*Nodes[0], Width, Height);
    return FMath::Max(Nodes[0]->NodePosY + Height, LaidBottom);
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
    // Every new node not yet laid out stays in Unplaced (ignored by earlier passes and FreeSpot) so unpositioned nodes
    // sitting at (0, 0) never act as fixed obstacles for another form's layout.
    const TSet<UEdGraphNode*> Replaced(Plan.Old);
    TSet<UEdGraphNode*> Unplaced;
    for (int32 At = 0; At < FormNodes.Num(); ++At)
    {
        const bool bMade = Plan.Entries[At] == nullptr;
        for (int32 Index = bMade ? 0 : 1; Index < FormNodes[At].Num(); ++Index)
        {
            if (!Transpiler.Kept.Contains(FormNodes[At][Index]))
            {
                Unplaced.Add(FormNodes[At][Index]);
            }
        }
    }
    // Pass 0: lay out all forms reusing existing entries together so each sees its own entry anchor cleanly without
    // sibling forms' unplaced nodes blocking it. When writing fresh bodies onto existing entries (no kept chain nodes),
    // keep untouched events and other regions fixed and slide new calls right; when editing an existing kept chain,
    // keep bAllowMovingExisting enabled so insertions can shift downstream nodes or push room down.
    TArray<UEdGraphNode*> ExistingFree;
    bool bPass0HasKept = false;
    for (int32 At = 0; At < FormNodes.Num(); ++At)
    {
        if (Plan.Entries[At] != nullptr)
        {
            for (int32 Index = 1; Index < FormNodes[At].Num(); ++Index)
            {
                UEdGraphNode* Node = FormNodes[At][Index];
                if (Transpiler.Kept.Contains(Node))
                {
                    bPass0HasKept = true;
                }
                else
                {
                    ExistingFree.Add(Node);
                    Unplaced.Remove(Node);
                }
            }
        }
    }
    if (ExistingFree.Num() > 0)
    {
        TSet<UEdGraphNode*> Ignore = Replaced;
        Ignore.Append(Unplaced);
        const FVector2D Spot = FreeSpot(Plan.Graph, Ignore);
        McpRuled::FParams ExistingParams;
        ExistingParams.bAllowMovingExisting = bPass0HasKept;
        McpRuled::LayoutNodes(Plan.Graph, ExistingFree, &Spot, nullptr, &Ignore, ExistingParams);
    }
    // Pass 1: lay out each newly created event below the settled graph, ignoring later unplaced forms.
    TSet<UEdGraphNode*> Pass1SpotIgnore = Replaced;
    Pass1SpotIgnore.Append(Unplaced);
    FVector2D Spot = FreeSpot(Plan.Graph, Pass1SpotIgnore);
    for (int32 At = 0; At < FormNodes.Num(); ++At)
    {
        if (Plan.Entries[At] != nullptr)
        {
            continue;
        }
        for (UEdGraphNode* Node : FormNodes[At])
        {
            Unplaced.Remove(Node);
        }
        TSet<UEdGraphNode*> Ignore = Replaced;
        Ignore.Append(Unplaced);
        const float Bottom = LayoutForm(Plan.Graph, FormNodes[At], Transpiler.Kept, Spot, Ignore, true);
        Spot.Y = Bottom + 200.0f;
    }
    Made.Append(Transpiler.Made);
    return true;
}
} // namespace Write
} // namespace McpGraphCode
