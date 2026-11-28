#include "Foundation/GraphLayout/McpGraphLayoutDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Foundation/GraphLayout/McpRuled.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"

namespace McpGraphLayoutDiag
{
namespace
{
bool IsShownPin(const UEdGraphNode& Node, const UEdGraphPin* Pin)
{
    return Pin && !Pin->bHidden &&
           !(Pin->bAdvancedView && Node.AdvancedPinDisplay == ENodeAdvancedPins::Hidden && Pin->LinkedTo.Num() == 0);
}

bool HasVisibleExecPin(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return false;
    }
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (IsShownPin(*Node, Pin) && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return true;
        }
    }
    return false;
}

bool IsActiveRootNode(const UEdGraphNode* Node)
{
    if (!Node || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>())
    {
        return false;
    }
    if (HasVisibleExecPin(Node) || Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_FunctionEntry>() ||
        Node->IsA<UK2Node_FunctionResult>())
    {
        return true;
    }
    return false;
}

bool IsDelegatePin(const UEdGraphPin* Pin)
{
    return Pin && (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate ||
                   Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate);
}

FString CanonicalSourceId(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return FString();
    }
    if (Node->GetClass() == UK2Node_Self::StaticClass())
    {
        return TEXT("self");
    }
    if (const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node))
    {
        if (Getter->VariableReference.IsSelfContext() && !Getter->VariableReference.IsLocalScope())
        {
            FString Ref;
            FMemberReference::StaticStruct()->ExportText(Ref, &Getter->VariableReference, nullptr, nullptr, PPF_None, nullptr);
            return TEXT("self-get:") + Ref;
        }
        if (Getter->VariableReference.IsLocalScope())
        {
            FString Ref;
            FMemberReference::StaticStruct()->ExportText(Ref, &Getter->VariableReference, nullptr, nullptr, PPF_None, nullptr);
            return TEXT("local-get:") + Ref;
        }
    }
    if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        const UFunction* Function = Call->GetClass() == UK2Node_CallFunction::StaticClass() && Call->IsNodePure()
            ? Call->GetTargetFunction()
            : nullptr;
        if (Function && !Call->IsLatentFunction() &&
            Function->HasAllFunctionFlags(FUNC_Native | FUNC_Const) &&
            !Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Event | FUNC_BlueprintEvent | FUNC_Net))
        {
            UEdGraphPin* SelfPin = nullptr;
            bool bExtra = false;
            for (UEdGraphPin* Pin : Call->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                if (Pin->PinName == UEdGraphSchema_K2::PN_Self)
                {
                    SelfPin = Pin;
                    continue;
                }
                if (IsShownPin(*Call, Pin) || Pin->LinkedTo.Num() > 0)
                {
                    bExtra = true;
                    break;
                }
            }
            if (!bExtra)
            {
                FString TargetId = TEXT("self");
                if (SelfPin && SelfPin->LinkedTo.Num() == 1 && SelfPin->LinkedTo[0])
                {
                    TargetId = CanonicalSourceId(SelfPin->LinkedTo[0]->GetOwningNode()) + TEXT(".") +
                               SelfPin->LinkedTo[0]->PinName.ToString();
                }
                return TEXT("const-get:") + Function->GetPathName() + TEXT("(") + TargetId + TEXT(")");
            }
        }
    }
    return Node->NodeGuid.ToString();
}

void CollectFoldedTargets(const UEdGraphPin* Pin, TSet<const UEdGraphPin*>& Seen, TArray<FString>& OutTargets)
{
    if (!Pin || Seen.Contains(Pin))
    {
        return;
    }
    Seen.Add(Pin);
    const UEdGraphNode* Owner = Pin->GetOwningNode();
    if (!Owner)
    {
        return;
    }
    if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Owner))
    {
        const UEdGraphPin* OutPin = Knot->GetOutputPin();
        if (OutPin)
        {
            for (const UEdGraphPin* Next : OutPin->LinkedTo)
            {
                CollectFoldedTargets(Next, Seen, OutTargets);
            }
        }
        return;
    }
    OutTargets.Add(CanonicalSourceId(Owner) + TEXT(".") + Pin->PinName.ToString());
}

bool BoxesOverlap(const FBox2D& A, const FBox2D& B)
{
    return A.Min.X < B.Max.X && B.Min.X < A.Max.X && A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y;
}

bool SegmentIntersectsBox(const FVector2D& P0, const FVector2D& P1, const FBox2D& Box)
{
    const FVector2D D = P1 - P0;
    double TMin = 0.0;
    double TMax = 1.0;
    const auto ClipAxis = [&](double Origin, double Delta, double MinB, double MaxB) -> bool
    {
        if (FMath::Abs(Delta) < 1e-6)
        {
            return Origin >= MinB && Origin <= MaxB;
        }
        double T0 = (MinB - Origin) / Delta;
        double T1 = (MaxB - Origin) / Delta;
        if (T0 > T1)
        {
            Swap(T0, T1);
        }
        TMin = FMath::Max(TMin, T0);
        TMax = FMath::Min(TMax, T1);
        return TMin <= TMax;
    };
    return ClipAxis(P0.X, D.X, Box.Min.X, Box.Max.X) && ClipAxis(P0.Y, D.Y, Box.Min.Y, Box.Max.Y);
}

FVector2D PinAnchorPoint(const UEdGraphNode& Node, const UEdGraphPin& Pin, const McpRuled::FShape& Shape)
{
    const double X = Pin.Direction == EGPD_Output
        ? static_cast<double>(Node.NodePosX) + static_cast<double>(Shape.Width)
        : static_cast<double>(Node.NodePosX);
    double YOffset = Shape.Height * 0.5;
    if (Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
    {
        if (Pin.Direction == EGPD_Input && Shape.ExecIn >= 0.0f)
        {
            YOffset = Shape.ExecIn;
        }
        else if (Pin.Direction == EGPD_Output)
        {
            int32 ExecIdx = 0;
            for (const UEdGraphPin* Other : Node.Pins)
            {
                if (IsShownPin(Node, Other) && Other->Direction == EGPD_Output &&
                    Other->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    if (Other == &Pin)
                    {
                        if (Shape.ExecOut.IsValidIndex(ExecIdx))
                        {
                            YOffset = Shape.ExecOut[ExecIdx];
                        }
                        break;
                    }
                    ++ExecIdx;
                }
            }
        }
    }
    else if (Pin.Direction == EGPD_Input)
    {
        int32 DataIdx = 0;
        for (const UEdGraphPin* Other : Node.Pins)
        {
            if (IsShownPin(Node, Other) && Other->Direction == EGPD_Input &&
                Other->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                if (Other == &Pin)
                {
                    if (Shape.DataIn.IsValidIndex(DataIdx))
                    {
                        YOffset = Shape.DataIn[DataIdx];
                    }
                    break;
                }
                ++DataIdx;
            }
        }
    }
    else
    {
        int32 DataIdx = 0;
        for (const UEdGraphPin* Other : Node.Pins)
        {
            if (IsShownPin(Node, Other) && Other->Direction == EGPD_Output &&
                Other->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                if (Other == &Pin)
                {
                    if (Shape.DataOut.IsValidIndex(DataIdx))
                    {
                        YOffset = Shape.DataOut[DataIdx];
                    }
                    break;
                }
                ++DataIdx;
            }
        }
    }
    return FVector2D(X, static_cast<double>(Node.NodePosY) + YOffset);
}

TSharedRef<FJsonObject> EdgeIssueToJson(const FEdgeIssue& Issue)
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("kind"), Issue.Kind);
    Obj->SetStringField(TEXT("edgeType"), Issue.EdgeType);
    Obj->SetStringField(TEXT("fromNodeId"), Issue.FromGuid);
    Obj->SetStringField(TEXT("fromTitle"), Issue.FromTitle);
    Obj->SetStringField(TEXT("fromPin"), Issue.FromPin);
    Obj->SetStringField(TEXT("toNodeId"), Issue.ToGuid);
    Obj->SetStringField(TEXT("toTitle"), Issue.ToTitle);
    Obj->SetStringField(TEXT("toPin"), Issue.ToPin);
    Obj->SetNumberField(TEXT("deltaX"), FMath::RoundToInt(Issue.DeltaX));
    Obj->SetNumberField(TEXT("deltaY"), FMath::RoundToInt(Issue.DeltaY));
    Obj->SetNumberField(TEXT("length"), FMath::RoundToInt(Issue.Length));
    if (Issue.CrossedNodeCount > 0)
    {
        Obj->SetNumberField(TEXT("crossedNodes"), Issue.CrossedNodeCount);
    }
    return Obj;
}
} // namespace

TSharedRef<FJsonObject> FLayoutQualityReport::ToJson() const
{
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetNumberField(TEXT("nodeOverlapCount"), NodeOverlapCount);
    Obj->SetNumberField(TEXT("wireNodeCrossingCount"), WireNodeCrossingCount);
    Obj->SetNumberField(TEXT("dataWireNodeCrossingCount"), DataWireNodeCrossingCount);
    Obj->SetNumberField(TEXT("commentIntrusionCount"), CommentIntrusionCount);
    Obj->SetNumberField(TEXT("backwardExecEdgeCount"), BackwardExecEdgeCount);
    Obj->SetNumberField(TEXT("longCrossRegionExecCount"), LongCrossRegionExecCount);
    Obj->SetNumberField(TEXT("longDelegateEdgeCount"), LongDelegateEdgeCount);
    Obj->SetNumberField(TEXT("longDataEdgeCount"), LongDataEdgeCount);
    Obj->SetNumberField(TEXT("danglingAutoKnotCount"), DanglingAutoKnotCount);
    Obj->SetNumberField(TEXT("boundaryEdgeCount"), BoundaryEdgeCount);
    Obj->SetNumberField(TEXT("maxDataEdgeLength"), FMath::RoundToInt(MaxDataEdgeLength));
    Obj->SetNumberField(TEXT("maxDisplacement"), FMath::RoundToInt(MaxDisplacement));

    TArray<TSharedPtr<FJsonValue>> IssueArr;
    const int32 MaxIssues = FMath::Min(Issues.Num(), 40);
    for (int32 Index = 0; Index < MaxIssues; ++Index)
    {
        IssueArr.Add(MakeShared<FJsonValueObject>(EdgeIssueToJson(Issues[Index])));
    }
    Obj->SetArrayField(TEXT("issues"), IssueArr);

    if (BoundaryEdges.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> BoundaryArr;
        const int32 MaxBoundary = FMath::Min(BoundaryEdges.Num(), 40);
        for (int32 Index = 0; Index < MaxBoundary; ++Index)
        {
            BoundaryArr.Add(MakeShared<FJsonValueObject>(EdgeIssueToJson(BoundaryEdges[Index])));
        }
        Obj->SetArrayField(TEXT("boundaryEdges"), BoundaryArr);
    }

    if (UnusedPureGroups.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> GroupsArr;
        for (const TArray<FString>& Group : UnusedPureGroups)
        {
            TArray<TSharedPtr<FJsonValue>> Ids;
            for (const FString& Id : Group)
            {
                Ids.Add(MakeShared<FJsonValueString>(Id));
            }
            GroupsArr.Add(MakeShared<FJsonValueArray>(Ids));
        }
        Obj->SetArrayField(TEXT("unusedPureGroups"), GroupsArr);
    }

    return Obj;
}

bool IsAutoLayoutKnot(const UEdGraphNode* Node)
{
    return Node && Node->IsA<UK2Node_Knot>() && Node->NodeComment.Contains(AutoKnotMarker);
}

void MarkAutoLayoutKnot(UK2Node_Knot* Knot)
{
    if (!Knot)
    {
        return;
    }
    Knot->NodeComment = AutoKnotMarker;
    Knot->bCommentBubbleVisible = false;
    Knot->bCommentBubblePinned = false;
}

namespace
{
int32 CollapseAutoKnotsInternal(UEdGraph* Graph, bool bExecOnly, const TSet<UEdGraphNode*>* ScopeNodes,
                                TArray<FGuid>* OutRemovedGuids)
{
    if (!Graph)
    {
        return 0;
    }
    TArray<UK2Node_Knot*> Candidates;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
        if (!Knot || !IsAutoLayoutKnot(Knot))
        {
            continue;
        }
        UEdGraphPin* InPin = Knot->GetInputPin();
        UEdGraphPin* OutPin = Knot->GetOutputPin();
        if (!InPin || !OutPin)
        {
            continue;
        }
        const bool bIsExec = InPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                             OutPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
        if (bExecOnly != bIsExec)
        {
            continue;
        }
        if (ScopeNodes)
        {
            bool bTouchesScope = ScopeNodes->Contains(Knot);
            for (const UEdGraphPin* Linked : InPin->LinkedTo)
            {
                if (Linked && ScopeNodes->Contains(Linked->GetOwningNode()))
                {
                    bTouchesScope = true;
                    break;
                }
            }
            for (const UEdGraphPin* Linked : OutPin->LinkedTo)
            {
                if (Linked && ScopeNodes->Contains(Linked->GetOwningNode()))
                {
                    bTouchesScope = true;
                    break;
                }
            }
            if (!bTouchesScope)
            {
                continue;
            }
        }
        Candidates.Add(Knot);
    }
    if (Candidates.Num() == 0)
    {
        return 0;
    }

    Graph->Modify();
    const UEdGraphSchema* Schema = Graph->GetSchema();
    int32 Removed = 0;
    for (UK2Node_Knot* Knot : Candidates)
    {
        UEdGraphPin* InPin = Knot->GetInputPin();
        UEdGraphPin* OutPin = Knot->GetOutputPin();
        const TArray<UEdGraphPin*> Sources = InPin->LinkedTo;
        const TArray<UEdGraphPin*> Targets = OutPin->LinkedTo;
        Knot->Modify();
        InPin->BreakAllPinLinks(true);
        OutPin->BreakAllPinLinks(true);
        for (UEdGraphPin* Source : Sources)
        {
            if (!Source || !Source->GetOwningNode())
            {
                continue;
            }
            for (UEdGraphPin* Target : Targets)
            {
                if (!Target || !Target->GetOwningNode() || Source->GetOwningNode() == Target->GetOwningNode())
                {
                    continue;
                }
                Source->GetOwningNode()->Modify();
                Target->GetOwningNode()->Modify();
                if (Schema)
                {
                    Schema->TryCreateConnection(Source, Target);
                }
                else
                {
                    Source->MakeLinkTo(Target);
                }
            }
        }
        if (OutRemovedGuids)
        {
            OutRemovedGuids->Add(Knot->NodeGuid);
        }
        Graph->RemoveNode(Knot);
        ++Removed;
    }
    return Removed;
}
} // namespace

int32 CollapseAutoExecKnots(UEdGraph* Graph, const TSet<UEdGraphNode*>* ScopeNodes, TArray<FGuid>* OutRemovedGuids)
{
    return CollapseAutoKnotsInternal(Graph, /*bExecOnly=*/true, ScopeNodes, OutRemovedGuids);
}

int32 CollapseAutoDataKnots(UEdGraph* Graph, const TSet<UEdGraphNode*>* ScopeNodes, TArray<FGuid>* OutRemovedGuids)
{
    return CollapseAutoKnotsInternal(Graph, /*bExecOnly=*/false, ScopeNodes, OutRemovedGuids);
}

TArray<FString> BuildSemanticTopologyFingerprint(const UEdGraph* Graph)
{
    TArray<FString> Rows;
    if (!Graph)
    {
        return Rows;
    }
    const UK2Node_FunctionEntry* Entry = nullptr;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (const UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
        {
            Entry = Candidate;
            break;
        }
    }
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>())
        {
            continue;
        }
        if (Entry)
        {
            if (const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node))
            {
                if (Getter->VariableReference.IsLocalScope() && Entry->FindPin(Getter->GetVarName(), EGPD_Output))
                {
                    if (const UEdGraphPin* OutPin = Getter->FindPin(Getter->GetVarName(), EGPD_Output))
                    {
                        TSet<const UEdGraphPin*> Seen;
                        TArray<FString> Targets;
                        for (const UEdGraphPin* Linked : OutPin->LinkedTo)
                        {
                            CollectFoldedTargets(Linked, Seen, Targets);
                        }
                        const FString EntryId = CanonicalSourceId(Entry);
                        for (const FString& Target : Targets)
                        {
                            Rows.Add(EntryId + TEXT(".") + OutPin->PinName.ToString() + TEXT("->") + Target);
                        }
                    }
                    continue;
                }
            }
        }
        const FString FromId = CanonicalSourceId(Node);
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }
            TSet<const UEdGraphPin*> Seen;
            TArray<FString> Targets;
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                CollectFoldedTargets(Linked, Seen, Targets);
            }
            for (const FString& Target : Targets)
            {
                Rows.Add(FromId + TEXT(".") + Pin->PinName.ToString() + TEXT("->") + Target);
            }
        }
    }
    Rows.Sort();
    return Rows;
}

TArray<TArray<UEdGraphNode*>> FindUnusedPureGroups(const UEdGraph* Graph, const TSet<UEdGraphNode*>* Ignore)
{
    TArray<TArray<UEdGraphNode*>> Groups;
    if (!Graph)
    {
        return Groups;
    }

    TSet<UEdGraphNode*> Active;
    TArray<UEdGraphNode*> Open;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || (Ignore && Ignore->Contains(Node)))
        {
            continue;
        }
        if (IsActiveRootNode(Node))
        {
            Active.Add(Node);
            Open.Add(Node);
        }
    }

    // Reverse BFS along data/delegate inputs (passing through knots) to find all consumed pure nodes.
    while (Open.Num() > 0)
    {
        UEdGraphNode* Current = Open.Pop(EAllowShrinking::No);
        for (const UEdGraphPin* Pin : Current->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Producer = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Producer || Producer->GetGraph() != Graph || (Ignore && Ignore->Contains(Producer)) ||
                    Active.Contains(Producer))
                {
                    continue;
                }
                Active.Add(Producer);
                Open.Add(Producer);
            }
        }
    }

    TSet<UEdGraphNode*> UnusedPure;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>() ||
            (Ignore && Ignore->Contains(Node)) || Active.Contains(Node))
        {
            continue;
        }
        UnusedPure.Add(Node);
    }

    TSet<UEdGraphNode*> Visited;
    for (UEdGraphNode* Seed : UnusedPure)
    {
        if (Visited.Contains(Seed))
        {
            continue;
        }
        TArray<UEdGraphNode*> Component;
        TArray<UEdGraphNode*> Queue = {Seed};
        Visited.Add(Seed);
        while (Queue.Num() > 0)
        {
            UEdGraphNode* Cur = Queue.Pop(EAllowShrinking::No);
            if (UnusedPure.Contains(Cur))
            {
                Component.Add(Cur);
            }
            for (const UEdGraphPin* Pin : Cur->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Neigh = Linked ? Linked->GetOwningNode() : nullptr;
                    if (!Neigh || Neigh->GetGraph() != Graph || Visited.Contains(Neigh))
                    {
                        continue;
                    }
                    if (UnusedPure.Contains(Neigh) || (Neigh->IsA<UK2Node_Knot>() && !Active.Contains(Neigh)))
                    {
                        Visited.Add(Neigh);
                        Queue.Add(Neigh);
                    }
                }
            }
        }
        Component.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
        {
            return A.NodeGuid.ToString() < B.NodeGuid.ToString();
        });
        if (Component.Num() > 0)
        {
            Groups.Add(MoveTemp(Component));
        }
    }
    Groups.Sort([](const TArray<UEdGraphNode*>& A, const TArray<UEdGraphNode*>& B)
    {
        return A[0]->NodeGuid.ToString() < B[0]->NodeGuid.ToString();
    });
    return Groups;
}

TMap<const UEdGraphNode*, FBox2D> MeasureNodeBoxes(const UEdGraph* Graph, const TSet<UEdGraphNode*>* Ignore)
{
    TMap<const UEdGraphNode*, FBox2D> Boxes;
    if (!Graph)
    {
        return Boxes;
    }
    TMap<const UEdGraphNode*, McpRuled::FShape> Shapes;
    McpRuled::MeasureGraphShapes(Graph, Shapes);
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>() || (Ignore && Ignore->Contains(const_cast<UEdGraphNode*>(Node))))
        {
            continue;
        }
        const McpRuled::FShape* ShapePtr = Shapes.Find(Node);
        const McpRuled::FShape Shape = ShapePtr ? *ShapePtr : McpRuled::EstimateShape(*Node);
        const FVector2D TopLeft(Node->NodePosX, Node->NodePosY);
        Boxes.Add(Node, FBox2D(TopLeft, TopLeft + FVector2D(Shape.Width, Shape.Height)));
    }
    return Boxes;
}

TArray<FCommentGroup> CollectCommentGroups(const UEdGraph* Graph, const TMap<const UEdGraphNode*, FBox2D>& NodeBoxes)
{
    TArray<FCommentGroup> Groups;
    if (!Graph)
    {
        return Groups;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
        if (!Comment)
        {
            continue;
        }
        FCommentGroup Group;
        Group.Comment = Comment;
        Group.Depth = Comment->CommentDepth;
        const FVector2D TopLeft(Comment->NodePosX, Comment->NodePosY);
        const FVector2D Size(FMath::Max(64, Comment->NodeWidth), FMath::Max(48, Comment->NodeHeight));
        Group.OriginalBounds = FBox2D(TopLeft, TopLeft + Size);
        for (const TPair<const UEdGraphNode*, FBox2D>& Entry : NodeBoxes)
        {
            const FBox2D& Box = Entry.Value;
            const FVector2D Center = Box.GetCenter();
            bool bInside = Group.OriginalBounds.IsInsideOrOn(Center);
            if (!bInside && BoxesOverlap(Group.OriginalBounds, Box))
            {
                const double InterW = FMath::Max(0.0, FMath::Min(Group.OriginalBounds.Max.X, Box.Max.X) -
                                                      FMath::Max(Group.OriginalBounds.Min.X, Box.Min.X));
                const double InterH = FMath::Max(0.0, FMath::Min(Group.OriginalBounds.Max.Y, Box.Max.Y) -
                                                      FMath::Max(Group.OriginalBounds.Min.Y, Box.Min.Y));
                const double BoxArea = FMath::Max(1.0, Box.GetArea());
                bInside = (InterW * InterH) / BoxArea >= 0.5;
            }
            if (bInside)
            {
                Group.Members.Add(const_cast<UEdGraphNode*>(Entry.Key));
            }
        }
        Group.bWasEmpty = Group.Members.Num() == 0;
        Groups.Add(MoveTemp(Group));
    }

    // Innermost (highest CommentDepth) first, then smallest area first so nested/overlapping comments resolve consistently.
    Groups.Sort([](const FCommentGroup& A, const FCommentGroup& B)
    {
        if (A.Depth != B.Depth)
        {
            return A.Depth > B.Depth;
        }
        const double AreaA = A.OriginalBounds.GetArea();
        const double AreaB = B.OriginalBounds.GetArea();
        if (!FMath::IsNearlyEqual(AreaA, AreaB, 1.0))
        {
            return AreaA < AreaB;
        }
        return A.Comment->NodeGuid.ToString() < B.Comment->NodeGuid.ToString();
    });
    return Groups;
}

UEdGraphNode_Comment* FindPrimaryOwnerComment(const UEdGraphNode* Node, const TArray<FCommentGroup>& Groups)
{
    if (!Node)
    {
        return nullptr;
    }
    for (const FCommentGroup& Group : Groups)
    {
        if (Group.Members.Contains(const_cast<UEdGraphNode*>(Node)))
        {
            return Group.Comment;
        }
    }
    return nullptr;
}

int32 SyncAndRefitComments(const TArray<FCommentGroup>& Groups,
                           const TMap<const UEdGraphNode*, FVector2D>& OldPositions,
                           const TMap<const UEdGraphNode*, FBox2D>& NewBoxes,
                           bool bAllowShrinkOnFullReflow,
                           TArray<UEdGraphNode_Comment*>* OutUpdated)
{
    constexpr double PadX = 32.0;
    constexpr double PadTop = 56.0;
    constexpr double PadBottom = 32.0;
    int32 UpdatedCount = 0;

    // Groups are ordered innermost first so outer comments can encompass updated nested comments.
    TMap<const UEdGraphNode_Comment*, FBox2D> FinalCommentBoxes;
    for (const FCommentGroup& Group : Groups)
    {
        if (!Group.Comment)
        {
            continue;
        }
        FinalCommentBoxes.Add(Group.Comment, Group.OriginalBounds);
    }

    for (const FCommentGroup& Group : Groups)
    {
        if (!Group.Comment || Group.Members.Num() == 0)
        {
            continue;
        }
        int32 MovedMembers = 0;
        bool bUniformDelta = true;
        FVector2D SharedDelta = FVector2D::ZeroVector;
        FBox2D MemberBounds(ForceInit);
        for (UEdGraphNode* Member : Group.Members)
        {
            if (!Member)
            {
                continue;
            }
            const FBox2D* NewBox = NewBoxes.Find(Member);
            if (!NewBox)
            {
                continue;
            }
            MemberBounds += *NewBox;
            if (const FVector2D* OldPos = OldPositions.Find(Member))
            {
                const FVector2D Delta = FVector2D(Member->NodePosX, Member->NodePosY) - *OldPos;
                if (FMath::Abs(Delta.X) > 0.5 || FMath::Abs(Delta.Y) > 0.5)
                {
                    if (MovedMembers == 0)
                    {
                        SharedDelta = Delta;
                    }
                    else if (!Delta.Equals(SharedDelta, 1.5))
                    {
                        bUniformDelta = false;
                    }
                    ++MovedMembers;
                }
                else if (MovedMembers > 0)
                {
                    bUniformDelta = false;
                }
            }
        }
        if (MovedMembers == 0 || !MemberBounds.bIsValid)
        {
            continue;
        }

        // Also wrap nested child comments that originally sat inside this comment.
        for (const FCommentGroup& Other : Groups)
        {
            if (Other.Comment == Group.Comment || !Other.Comment)
            {
                continue;
            }
            if (Group.OriginalBounds.IsInsideOrOn(Other.OriginalBounds.Min) &&
                Group.OriginalBounds.IsInsideOrOn(Other.OriginalBounds.Max))
            {
                if (const FBox2D* ChildFinal = FinalCommentBoxes.Find(Other.Comment))
                {
                    MemberBounds += *ChildFinal;
                }
            }
        }

        FBox2D TargetBounds(ForceInit);
        if (bUniformDelta && MovedMembers == Group.Members.Num())
        {
            TargetBounds = Group.OriginalBounds.ShiftBy(SharedDelta);
        }
        else
        {
            const FBox2D Tight(MemberBounds.Min - FVector2D(PadX, PadTop),
                               MemberBounds.Max + FVector2D(PadX, PadBottom));
            if (bAllowShrinkOnFullReflow && MovedMembers == Group.Members.Num())
            {
                TargetBounds = Tight;
            }
            else
            {
                TargetBounds = Group.OriginalBounds;
                TargetBounds += Tight;
            }
        }

        const int32 NewX = FMath::RoundToInt(TargetBounds.Min.X);
        const int32 NewY = FMath::RoundToInt(TargetBounds.Min.Y);
        const int32 NewW = FMath::Max(64, FMath::RoundToInt(TargetBounds.Max.X - TargetBounds.Min.X));
        const int32 NewH = FMath::Max(48, FMath::RoundToInt(TargetBounds.Max.Y - TargetBounds.Min.Y));
        if (Group.Comment->NodePosX != NewX || Group.Comment->NodePosY != NewY ||
            Group.Comment->NodeWidth != NewW || Group.Comment->NodeHeight != NewH)
        {
            Group.Comment->Modify();
            Group.Comment->NodePosX = NewX;
            Group.Comment->NodePosY = NewY;
            Group.Comment->NodeWidth = NewW;
            Group.Comment->NodeHeight = NewH;
            ++UpdatedCount;
            if (OutUpdated)
            {
                OutUpdated->Add(Group.Comment);
            }
        }
        FinalCommentBoxes.Add(Group.Comment,
                              FBox2D(FVector2D(NewX, NewY), FVector2D(NewX + NewW, NewY + NewH)));
    }
    return UpdatedCount;
}

FLayoutQualityReport AnalyzeGraphQuality(const UEdGraph* Graph,
                                         const TSet<UEdGraphNode*>* ScopeNodes,
                                         const TMap<const UEdGraphNode*, FBox2D>* CachedBoxes,
                                         const TArray<FCommentGroup>* OriginalComments,
                                         const TMap<const UEdGraphNode*, FVector2D>* OldPositions)
{
    FLayoutQualityReport Report;
    if (!Graph)
    {
        return Report;
    }

    TMap<const UEdGraphNode*, McpRuled::FShape> Shapes;
    McpRuled::MeasureGraphShapes(Graph, Shapes);
    TMap<const UEdGraphNode*, FBox2D> LocalBoxes;
    TArray<const UEdGraphNode*> AllNodes;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>())
        {
            continue;
        }
        AllNodes.Add(Node);
        const McpRuled::FShape* ShapePtr = Shapes.Find(Node);
        const McpRuled::FShape Shape = ShapePtr ? *ShapePtr : McpRuled::EstimateShape(*Node);
        if (!ShapePtr)
        {
            Shapes.Add(Node, Shape);
        }
        if (CachedBoxes && CachedBoxes->Contains(Node))
        {
            LocalBoxes.Add(Node, (*CachedBoxes)[Node]);
        }
        else
        {
            const FVector2D At(Node->NodePosX, Node->NodePosY);
            LocalBoxes.Add(Node, FBox2D(At, At + FVector2D(Shape.Width, Shape.Height)));
        }
    }

    const auto InScope = [ScopeNodes](const UEdGraphNode* Node) -> bool
    {
        return !ScopeNodes || ScopeNodes->Contains(const_cast<UEdGraphNode*>(Node));
    };

    // 1. Node overlaps & max displacement & dangling auto-knots.
    for (int32 I = 0; I < AllNodes.Num(); ++I)
    {
        const UEdGraphNode* A = AllNodes[I];
        const FBox2D& BoxA = LocalBoxes[A];
        if (OldPositions)
        {
            if (const FVector2D* OldPos = OldPositions->Find(A))
            {
                const double Dist = (FVector2D(A->NodePosX, A->NodePosY) - *OldPos).Size();
                Report.MaxDisplacement = FMath::Max(Report.MaxDisplacement, Dist);
            }
        }
        if (IsAutoLayoutKnot(A))
        {
            const UK2Node_Knot* Knot = CastChecked<UK2Node_Knot>(A);
            const UEdGraphPin* InPin = Knot->GetInputPin();
            const UEdGraphPin* OutPin = Knot->GetOutputPin();
            if (!InPin || !OutPin || InPin->LinkedTo.Num() == 0 || OutPin->LinkedTo.Num() == 0)
            {
                ++Report.DanglingAutoKnotCount;
            }
        }
        for (int32 J = 0; J < I; ++J)
        {
            const UEdGraphNode* B = AllNodes[J];
            if ((InScope(A) || InScope(B)) && BoxesOverlap(BoxA, LocalBoxes[B]))
            {
                ++Report.NodeOverlapCount;
            }
        }
    }

    // 2. Comment intrusions (moved nodes landing inside an untouched comment they did not originally belong to).
    if (OriginalComments && OldPositions)
    {
        for (const UEdGraphNode* Node : AllNodes)
        {
            const FVector2D* OldPos = OldPositions->Find(Node);
            if (!OldPos)
            {
                continue;
            }
            if (FVector2D(Node->NodePosX, Node->NodePosY).Equals(*OldPos, 0.5))
            {
                continue;
            }
            const FVector2D Center = LocalBoxes[Node].GetCenter();
            for (const FCommentGroup& Group : *OriginalComments)
            {
                if (!Group.Comment || Group.Members.Contains(const_cast<UEdGraphNode*>(Node)))
                {
                    continue;
                }
                const FBox2D CurrentCommentBox(
                    FVector2D(Group.Comment->NodePosX, Group.Comment->NodePosY),
                    FVector2D(Group.Comment->NodePosX + Group.Comment->NodeWidth,
                              Group.Comment->NodePosY + Group.Comment->NodeHeight));
                if (CurrentCommentBox.IsInsideOrOn(Center))
                {
                    ++Report.CommentIntrusionCount;
                    break;
                }
            }
        }
    }

    // 3. Edge quality: backward execs, long cross-region execs, long delegates, wire-node crossings, boundary edges.
    for (const UEdGraphNode* From : AllNodes)
    {
        const McpRuled::FShape& FromShape = Shapes[From];
        for (const UEdGraphPin* OutPin : From->Pins)
        {
            if (!IsShownPin(*From, OutPin) || OutPin->Direction != EGPD_Output)
            {
                continue;
            }
            const bool bExec = OutPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
            const bool bDelegate = IsDelegatePin(OutPin);
            const FVector2D FromPt = PinAnchorPoint(*From, *OutPin, FromShape);
            for (const UEdGraphPin* InPin : OutPin->LinkedTo)
            {
                const UEdGraphNode* To = InPin ? InPin->GetOwningNode() : nullptr;
                if (!To || To == From || !LocalBoxes.Contains(To))
                {
                    continue;
                }
                const bool bFromIn = InScope(From);
                const bool bToIn = InScope(To);
                if (!bFromIn && !bToIn)
                {
                    continue;
                }

                const FVector2D ToPt = PinAnchorPoint(*To, *InPin, Shapes[To]);
                const FVector2D Delta = ToPt - FromPt;
                const double Length = Delta.Size();
                const FString EdgeType = bExec ? TEXT("exec") : (bDelegate ? TEXT("delegate") : TEXT("data"));

                auto MakeIssue = [&](const TCHAR* Kind, int32 Crossed) -> FEdgeIssue
                {
                    FEdgeIssue Issue;
                    Issue.Kind = Kind;
                    Issue.EdgeType = EdgeType;
                    Issue.FromGuid = From->NodeGuid.ToString();
                    Issue.FromTitle = From->GetNodeTitle(ENodeTitleType::ListView).ToString();
                    Issue.FromPin = OutPin->PinName.ToString();
                    Issue.ToGuid = To->NodeGuid.ToString();
                    Issue.ToTitle = To->GetNodeTitle(ENodeTitleType::ListView).ToString();
                    Issue.ToPin = InPin->PinName.ToString();
                    Issue.DeltaX = Delta.X;
                    Issue.DeltaY = Delta.Y;
                    Issue.Length = Length;
                    Issue.CrossedNodeCount = Crossed;
                    return Issue;
                };

                if (ScopeNodes && (bFromIn != bToIn))
                {
                    ++Report.BoundaryEdgeCount;
                    Report.BoundaryEdges.Add(MakeIssue(TEXT("boundary_edge"), 0));
                }

                int32 CrossedNodes = 0;
                if (Length > 96.0)
                {
                    for (const UEdGraphNode* Obst : AllNodes)
                    {
                        if (Obst == From || Obst == To || Obst->IsA<UK2Node_Knot>())
                        {
                            continue;
                        }
                        const FBox2D& Raw = LocalBoxes[Obst];
                        const FVector2D Margin(6.0, 6.0);
                        if (Raw.GetSize().X <= 16.0 || Raw.GetSize().Y <= 16.0)
                        {
                            continue;
                        }
                        const FBox2D Shrunk(Raw.Min + Margin, Raw.Max - Margin);
                        if (SegmentIntersectsBox(FromPt, ToPt, Shrunk))
                        {
                            ++CrossedNodes;
                        }
                    }
                }

                if (bExec || bDelegate)
                {
                    if (CrossedNodes > 0)
                    {
                        ++Report.WireNodeCrossingCount;
                        Report.Issues.Add(MakeIssue(TEXT("wire_crosses_node"), CrossedNodes));
                    }
                    if (bExec && Delta.X < -64.0)
                    {
                        ++Report.BackwardExecEdgeCount;
                        Report.Issues.Add(MakeIssue(TEXT("backward_exec"), CrossedNodes));
                    }
                    if (bExec && (FMath::Abs(Delta.X) > 960.0 || FMath::Abs(Delta.Y) > 640.0))
                    {
                        ++Report.LongCrossRegionExecCount;
                        Report.Issues.Add(MakeIssue(TEXT("long_exec_span"), CrossedNodes));
                    }
                    if (bDelegate && Length > 1200.0)
                    {
                        ++Report.LongDelegateEdgeCount;
                        Report.Issues.Add(MakeIssue(TEXT("long_delegate_span"), CrossedNodes));
                    }
                }
                else
                {
                    Report.MaxDataEdgeLength = FMath::Max(Report.MaxDataEdgeLength, Length);
                    if (CrossedNodes > 0)
                    {
                        ++Report.DataWireNodeCrossingCount;
                        Report.Issues.Add(MakeIssue(TEXT("data_wire_crosses_node"), CrossedNodes));
                    }
                    if (Length > 600.0)
                    {
                        ++Report.LongDataEdgeCount;
                        Report.Issues.Add(MakeIssue(TEXT("long_data_span"), CrossedNodes));
                    }
                }
            }
        }
    }

    Report.Issues.StableSort([](const FEdgeIssue& A, const FEdgeIssue& B)
    {
        if (A.CrossedNodeCount != B.CrossedNodeCount)
        {
            return A.CrossedNodeCount > B.CrossedNodeCount;
        }
        return A.Length > B.Length;
    });

    // 4. Unused pure node groups.
    const TArray<TArray<UEdGraphNode*>> Unused = FindUnusedPureGroups(Graph);
    for (const TArray<UEdGraphNode*>& Group : Unused)
    {
        TArray<FString> Ids;
        Ids.Reserve(Group.Num());
        for (const UEdGraphNode* Node : Group)
        {
            Ids.Add(Node->NodeGuid.ToString());
        }
        Report.UnusedPureGroups.Add(MoveTemp(Ids));
    }

    return Report;
}

bool ShiftBlockClearOfObstacles(UEdGraph* Graph, const TArray<UEdGraphNode*>& Listed,
                                const TMap<const UEdGraphNode*, FBox2D>& BeforeBoxes,
                                const TArray<FCommentGroup>& Comments, int32& OutObstacleHits)
{
    OutObstacleHits = 0;
    const TSet<UEdGraphNode*> ListedSet(Listed);
    constexpr double Pad = 24.0;
    TArray<FBox2D> Obstacles;
    for (const TPair<const UEdGraphNode*, FBox2D>& Entry : BeforeBoxes)
    {
        if (Entry.Key && !ListedSet.Contains(const_cast<UEdGraphNode*>(Entry.Key)) && Entry.Value.bIsValid)
        {
            Obstacles.Add(Entry.Value.ExpandBy(Pad));
        }
    }
    for (const FCommentGroup& Group : Comments)
    {
        bool bOwnsListed = false;
        for (UEdGraphNode* Member : Group.Members)
        {
            if (ListedSet.Contains(Member))
            {
                bOwnsListed = true;
                break;
            }
        }
        if (!bOwnsListed && Group.OriginalBounds.bIsValid)
        {
            Obstacles.Add(Group.OriginalBounds.ExpandBy(Pad));
        }
    }
    if (Obstacles.Num() == 0)
    {
        return true;
    }

    const TMap<const UEdGraphNode*, FBox2D> Solved = MeasureNodeBoxes(Graph);
    TArray<FBox2D> Block;
    for (UEdGraphNode* Node : Listed)
    {
        if (const FBox2D* Box = Solved.Find(Node))
        {
            Block.Add(*Box);
        }
    }
    auto Hits = [&](const FVector2D& Offset) -> int32
    {
        int32 Count = 0;
        for (const FBox2D& Box : Block)
        {
            const FBox2D Moved(Box.Min + Offset, Box.Max + Offset);
            for (const FBox2D& Obstacle : Obstacles)
            {
                if (Moved.Intersect(Obstacle))
                {
                    ++Count;
                }
            }
        }
        return Count;
    };
    OutObstacleHits = Hits(FVector2D::ZeroVector);
    if (OutObstacleHits == 0)
    {
        return true;
    }
    constexpr double Step = 32.0;
    for (int32 K = 1; K <= 256; ++K)
    {
        for (const FVector2D& Dir : {FVector2D(0.0, 1.0), FVector2D(1.0, 0.0), FVector2D(0.0, -1.0)})
        {
            const FVector2D Offset = Dir * (Step * K);
            if (Hits(Offset) == 0)
            {
                for (UEdGraphNode* Node : Listed)
                {
                    Node->NodePosX += FMath::RoundToInt(Offset.X);
                    Node->NodePosY += FMath::RoundToInt(Offset.Y);
                }
                return true;
            }
        }
    }
    return false;
}
} // namespace McpGraphLayoutDiag
