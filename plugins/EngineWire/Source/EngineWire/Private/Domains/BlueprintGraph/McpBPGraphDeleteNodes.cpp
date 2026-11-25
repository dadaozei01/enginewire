#include "Domains/BlueprintGraph/McpBPGraphPrivate.h"
#include "Domains/BlueprintGraph/McpBPGraphBatchSteps.h"
#include "Foundation/BridgeHelpers/Responses/McpJsonFields.h"

#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Foundation/GraphLayout/McpGraphLayoutDiagnostics.h"
#include "Foundation/GraphLayout/McpLayered.h"
#include "Foundation/GraphLayout/McpRuled.h"
#include "Foundation/McpScopedEditorTransaction.h"
#include "K2Node_Knot.h"
#include "ScopedTransaction.h"

// delete_node: one node (nodeId or nodeGuid) or many (nodeIds). Removing a 71-node dead chain used to take
// 71 consented calls, each recompiling and saving the Blueprint; nodeIds deletes them all under one consent,
// one transaction, one compile and one save. All or nothing: every id must resolve to a user-deletable node
// before anything is removed.
namespace McpBlueprintGraphHandlers
{
bool DeleteNodes(FActionContext& Context)
{
    if (Context.SubAction != TEXT("delete_node"))
    {
        return false;
    }

    TArray<FString> Ids;
    const FString SingleId = McpGetFirstStringField(Context.Payload, {TEXT("nodeId"), TEXT("nodeGuid")});
    if (!SingleId.IsEmpty())
    {
        Ids.Add(SingleId);
    }
    const TArray<TSharedPtr<FJsonValue>>* IdValues = nullptr;
    if (Context.Payload->TryGetArrayField(TEXT("nodeIds"), IdValues) && IdValues)
    {
        for (const TSharedPtr<FJsonValue>& Value : *IdValues)
        {
            FString Id;
            if (Value.IsValid() && Value->TryGetString(Id) && !Id.IsEmpty())
            {
                Ids.AddUnique(Id);
            }
        }
    }
    if (Ids.Num() == 0)
    {
        Context.SendError(TEXT("Give nodeId (or nodeGuid) for one node, or nodeIds for several."), TEXT("INVALID_ARGUMENT"));
        return true;
    }

    // `pinName` only means something to the break_pin_links fold (deleteScope "pin_links"). Sent without
    // it, "operate on this pin" and "delete the whole node" contradict each other, and a destructive
    // default must not resolve that in its own favour.
    FString ScopedPinName;
    if (Context.Payload->TryGetStringField(TEXT("pinName"), ScopedPinName) && !ScopedPinName.IsEmpty())
    {
        Context.SendError(FString::Printf(
            TEXT("'pinName' ('%s') was sent with deleteScope 'node', which deletes the ENTIRE node and ignores "
                 "the pin. Re-send with deleteScope: \"pin_links\" to break that pin's links instead, or drop "
                 "'pinName' to confirm you meant to delete the whole node."), *ScopedPinName),
            TEXT("CONTRADICTORY_SCOPE"));
        return true;
    }

    TArray<UEdGraphNode*> Targets;
    TArray<FString> Missing, Protected;
    for (const FString& Id : Ids)
    {
        UEdGraphNode* Node = Context.FindNode(Id);
        if (!Node)
        {
            Missing.Add(Id);
        }
        // Function entry and result nodes are managed by the editor: removing one orphans the function
        // graph and a later compile asserts (ReplaceFunctionReferences), so the UI's own gate applies.
        else if (!Node->CanUserDeleteNode())
        {
            Protected.Add(FString::Printf(TEXT("%s (%s)"), *Id, *Node->GetClass()->GetName()));
        }
        else
        {
            Targets.AddUnique(Node);
        }
    }
    if (Missing.Num() > 0)
    {
        if (Ids.Num() == 1)
        {
            Context.SendNodeNotFound(Missing[0]);
            return true;
        }
        Context.SendError(FString::Printf(TEXT("Nothing was deleted: no node matched %s. Read the ids with inspect_graph."),
            *FString::Join(Missing, TEXT(", "))), TEXT("NODE_NOT_FOUND"));
        return true;
    }
    if (Protected.Num() > 0)
    {
        Context.SendError(FString::Printf(
            TEXT("Nothing was deleted: %s cannot be deleted (function entry and result nodes are managed by the editor)."),
            *FString::Join(Protected, TEXT(", "))), TEXT("PROTECTED_NODE"));
        return true;
    }

    const FScopedTransaction Transaction(FText::FromString(
        Targets.Num() == 1 ? TEXT("Delete Blueprint Node") : TEXT("Delete Blueprint Nodes")));
    Context.Blueprint->Modify();
    TArray<TSharedPtr<FJsonValue>> Removed;
    for (UEdGraphNode* Node : Targets)
    {
        if (UEdGraph* Graph = Node->GetGraph())
        {
            Graph->Modify();
        }
        Removed.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
        FBlueprintEditorUtils::RemoveNode(Context.Blueprint, Node, true);
    }
    SaveLoadedAssetThrottled(Context.Blueprint);

    TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
    Result->SetArrayField(TEXT("removed"), Removed);
    Result->SetNumberField(TEXT("removedCount"), Removed.Num());
    McpHandlerUtils::AddVerification(Result, Context.Blueprint);
    Context.SendResponse(Removed.Num() == 1 ? TEXT("Node deleted.") : FString::Printf(TEXT("%d nodes deleted."), Removed.Num()), Result);
    return true;
}

// arrange_nodes / format_graph: arranges Blueprint graph nodes with full-graph, event-region, or selection scope,
// preview/dry-run support, comment-box preservation, PC_Exec back-edge outer corridor routing, semantic topology
// verification, and structured before/after quality diagnostics (EW-001..EW-008).
bool ArrangeNodes(FActionContext& Context)
{
    const bool bIsFormatGraph = Context.SubAction == TEXT("format_graph");
    if (Context.SubAction != TEXT("arrange_nodes") && !bIsFormatGraph)
    {
        return false;
    }

    FString Scope = GetJsonStringField(Context.Payload, TEXT("scope"));
    if (Scope.IsEmpty())
    {
        Scope = bIsFormatGraph ? TEXT("graph") : TEXT("selection");
    }
    const bool bFullGraphScope = Scope.Equals(TEXT("graph"), ESearchCase::IgnoreCase);
    const bool bEventRegionScope = Scope.Equals(TEXT("event_regions"), ESearchCase::IgnoreCase);
    const bool bPreview = GetJsonBoolField(Context.Payload, TEXT("preview"), false) ||
                          GetJsonBoolField(Context.Payload, TEXT("dryRun"), false);
    const bool bRouteBackEdges = GetJsonBoolField(Context.Payload, TEXT("routeBackEdges"), true);
    const bool bLocalizeSharedGetters = GetJsonBoolField(Context.Payload, TEXT("localizeSharedGetters"), true);

    TArray<UObject*> Nodes;
    const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
    const bool bHasIds = Context.Payload->TryGetArrayField(TEXT("nodeIds"), Ids) && Ids && Ids->Num() > 0;
    if (bFullGraphScope && !bHasIds)
    {
        for (UEdGraphNode* Node : Context.TargetGraph->Nodes)
        {
            if (Node && !Node->IsA<UEdGraphNode_Comment>())
            {
                Nodes.AddUnique(Node);
            }
        }
        if (Nodes.Num() == 0)
        {
            TSharedPtr<FJsonObject> EmptyResult = McpHandlerUtils::CreateResultObject();
            EmptyResult->SetStringField(TEXT("layoutStatus"), TEXT("no_changes"));
            EmptyResult->SetStringField(TEXT("scope"), Scope);
            EmptyResult->SetArrayField(TEXT("moved"), {});
            EmptyResult->SetArrayField(TEXT("unmoved"), {});
            EmptyResult->SetArrayField(TEXT("alreadyInPlace"), {});
            Context.SendResponse(TEXT("Graph has no nodes to arrange."), EmptyResult);
            return true;
        }
    }
    else
    {
        if (!bHasIds)
        {
            Context.SendError(TEXT("arrange_nodes needs nodeIds (or scope: \"graph\" on format_graph). The nodes left out stay put and anchor them."),
                              TEXT("INVALID_ARGUMENT"));
            return true;
        }
        // Every id resolves before anything moves: an unknown one is refused with nothing changed.
        for (const TSharedPtr<FJsonValue>& Id : *Ids)
        {
            UEdGraphNode* Node = Context.FindNode(Id.IsValid() ? Id->AsString() : FString());
            if (!Node)
            {
                Context.SendNodeNotFound(Id.IsValid() ? Id->AsString() : FString());
                return true;
            }
            if (!Node->IsA<UEdGraphNode_Comment>())
            {
                Nodes.AddUnique(Node);
            }
        }
    }

    // For event_regions scope, expand listed seeds to their owning exec chains and exclusive pure data producers.
    if (bEventRegionScope && Nodes.Num() > 0)
    {
        TSet<UEdGraphNode*> Expanded;
        TArray<UEdGraphNode*> Open;
        for (UObject* Obj : Nodes)
        {
            if (UEdGraphNode* N = Cast<UEdGraphNode>(Obj))
            {
                Expanded.Add(N);
                Open.Add(N);
            }
        }
        while (Open.Num() > 0)
        {
            UEdGraphNode* Cur = Open.Pop(EAllowShrinking::No);
            for (const UEdGraphPin* Pin : Cur->Pins)
            {
                if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
                    if (Other && Other->GetGraph() == Context.TargetGraph && !Other->IsA<UEdGraphNode_Comment>() &&
                        !Expanded.Contains(Other))
                    {
                        Expanded.Add(Other);
                        Open.Add(Other);
                    }
                }
            }
        }
        for (bool bGrew = true; bGrew;)
        {
            bGrew = false;
            TArray<UEdGraphNode*> ToAdd;
            for (UEdGraphNode* Node : Expanded)
            {
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        continue;
                    }
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Prod = Linked ? Linked->GetOwningNode() : nullptr;
                        if (!Prod || Prod->GetGraph() != Context.TargetGraph || Prod->IsA<UEdGraphNode_Comment>() ||
                            Expanded.Contains(Prod))
                        {
                            continue;
                        }
                        bool bProdHasExec = false;
                        for (const UEdGraphPin* PPin : Prod->Pins)
                        {
                            if (PPin && !PPin->bHidden && PPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                            {
                                bProdHasExec = true;
                                break;
                            }
                        }
                        if (!bProdHasExec)
                        {
                            ToAdd.AddUnique(Prod);
                        }
                    }
                }
            }
            for (UEdGraphNode* Prod : ToAdd)
            {
                Expanded.Add(Prod);
                Nodes.AddUnique(Prod);
                bGrew = true;
            }
        }
        for (UEdGraphNode* Node : Expanded)
        {
            Nodes.AddUnique(Node);
        }
    }

    FString Layout = GetJsonStringField(Context.Payload, TEXT("layout"));
    if (Layout.IsEmpty())
    {
        Layout = TEXT("ruled");
    }
    const bool bLayered = Layout == TEXT("layered");
    const bool bRuled = Layout == TEXT("ruled");
    const bool bBeside = Layout == TEXT("beside");
    if (!bLayered && !bRuled && !bBeside)
    {
        Context.SendError(FString::Printf(TEXT("Unknown layout '%s'. Use ruled (default), layered, or beside."), *Layout),
                          TEXT("INVALID_ARGUMENT"));
        return true;
    }

    TArray<UEdGraphNode*> Listed;
    TMap<UEdGraphNode*, FIntPoint> Original;
    TMap<const UEdGraphNode*, FVector2D> OriginalAllPos;
    for (UEdGraphNode* Node : Context.TargetGraph->Nodes)
    {
        if (Node && !Node->IsA<UEdGraphNode_Comment>())
        {
            OriginalAllPos.Add(Node, FVector2D(Node->NodePosX, Node->NodePosY));
        }
    }
    for (UObject* Object : Nodes)
    {
        UEdGraphNode* Node = CastChecked<UEdGraphNode>(Object);
        Original.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
        Listed.Add(Node);
    }

    const TSet<UEdGraphNode*> ScopeSet(Listed);
    const TMap<const UEdGraphNode*, FBox2D> BeforeBoxes = McpGraphLayoutDiag::MeasureNodeBoxes(Context.TargetGraph);
    const TArray<McpGraphLayoutDiag::FCommentGroup> OriginalComments =
        McpGraphLayoutDiag::CollectCommentGroups(Context.TargetGraph, BeforeBoxes);
    const TArray<FString> BeforeFingerprint = McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Context.TargetGraph);
    const McpGraphLayoutDiag::FLayoutQualityReport BeforeQuality =
        McpGraphLayoutDiag::AnalyzeGraphQuality(Context.TargetGraph, bFullGraphScope ? nullptr : &ScopeSet,
                                                &BeforeBoxes, &OriginalComments, &OriginalAllPos);

    TSharedPtr<FJsonObject> Result = McpHandlerUtils::CreateResultObject();
    Result->SetStringField(TEXT("scope"), Scope);
    Result->SetStringField(TEXT("layout"), Layout);

    if (bPreview)
    {
        TMap<UEdGraphNode*, FIntPoint> PreviewPositions;
        FString Failure;
        if (bLayered)
        {
            // McpLayered is pure geometry without a preview hook: solve in-place, record, then restore.
            McpLayered::LayoutNodes(Listed);
            int32 ObstacleHits = 0;
            McpGraphLayoutDiag::ShiftBlockClearOfObstacles(Context.TargetGraph, Listed, BeforeBoxes, OriginalComments, ObstacleHits);
            for (UEdGraphNode* Node : Listed)
            {
                PreviewPositions.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
                const FIntPoint& Orig = Original.FindChecked(Node);
                Node->NodePosX = Orig.X;
                Node->NodePosY = Orig.Y;
            }
        }
        else if (bRuled)
        {
            McpRuled::FParams Params;
            Params.bAllowMovingExisting = bFullGraphScope;
            Params.bLocalizeSharedGetters = bLocalizeSharedGetters;
            Params.bRouteBackwardExecs = bRouteBackEdges;
            Params.bProtectExistingComments = !bFullGraphScope;
            Params.bPreviewOnly = true;
            McpRuled::LayoutNodes(Context.TargetGraph, Listed, nullptr, nullptr, nullptr, Params, &Failure, &PreviewPositions);
        }
        if (!Failure.IsEmpty())
        {
            Result->SetStringField(TEXT("layoutStatus"), TEXT("failed"));
            Context.SendError(Failure, TEXT("LAYOUT_COLLISION"));
            return true;
        }
        TArray<TSharedPtr<FJsonValue>> PlannedMoved;
        double MaxDisp = 0.0;
        for (const TPair<UEdGraphNode*, FIntPoint>& Entry : PreviewPositions)
        {
            const FIntPoint* Orig = Original.Find(Entry.Key);
            if (Orig && *Orig != Entry.Value)
            {
                MaxDisp = FMath::Max(MaxDisp, (FVector2D(Entry.Value.X, Entry.Value.Y) - FVector2D(Orig->X, Orig->Y)).Size());
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("nodeId"), Entry.Key->NodeGuid.ToString());
                Row->SetStringField(TEXT("nodeTitle"), Entry.Key->GetNodeTitle(ENodeTitleType::ListView).ToString());
                Row->SetNumberField(TEXT("x"), Entry.Value.X);
                Row->SetNumberField(TEXT("y"), Entry.Value.Y);
                PlannedMoved.Add(MakeShared<FJsonValueObject>(Row));
            }
        }
        TSharedRef<FJsonObject> Diag = MakeShared<FJsonObject>();
        Diag->SetObjectField(TEXT("before"), BeforeQuality.ToJson());
        Diag->SetNumberField(TEXT("plannedMaxDisplacement"), FMath::RoundToInt(MaxDisp));
        Diag->SetBoolField(TEXT("topologyFingerprintMatched"), true);
        Result->SetStringField(TEXT("layoutStatus"), PlannedMoved.Num() > 0 ? TEXT("planned") : TEXT("no_changes"));
        Result->SetArrayField(TEXT("moved"), PlannedMoved);
        Result->SetArrayField(TEXT("unmoved"), {});
        Result->SetArrayField(TEXT("alreadyInPlace"), {});
        Result->SetObjectField(TEXT("layoutDiagnostics"), Diag);
        Context.SendResponse(FString::Printf(TEXT("Previewed %s layout (%s scope): %d of %d nodes would move (max displacement %d)."),
                                             *Layout, *Scope, PlannedMoved.Num(), Original.Num(), FMath::RoundToInt(MaxDisp)),
                             Result);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Moved;
    TArray<TSharedPtr<FJsonValue>> Stayed;
    TArray<TSharedPtr<FJsonValue>> InPlace;
    TArray<UEdGraphNode*> AddedNodes;
    TArray<UEdGraphNode*> UpdatedComments;
    bool bTopologyMatched = true;
    {
        // Include comments in transaction object set so undo restores comment box refits too.
        TArray<UObject*> TxObjects = Nodes;
        for (const McpGraphLayoutDiag::FCommentGroup& Group : OriginalComments)
        {
            if (Group.Comment)
            {
                TxObjects.AddUnique(Group.Comment);
            }
        }
        const FMcpScopedEditorTransaction Transaction(FText::FromString(TEXT("Arrange Nodes")),
                                                      EMcpMutationDurability::EditorStateOnly, TxObjects);
        GraphBatch::FBatchState State;
        if (bBeside)
        {
            for (UEdGraphNode* Node : Listed)
            {
                Node->NodePosX = -1000000 - Original.Num() * 480;
                Node->NodePosY = -1000000;
                State.AutoPlacedGuids.Add(Node->NodeGuid.ToString());
            }
        }
        if (bLayered)
        {
            McpLayered::LayoutNodes(Listed);
            int32 ObstacleHits = 0;
            if (!McpGraphLayoutDiag::ShiftBlockClearOfObstacles(Context.TargetGraph, Listed, BeforeBoxes, OriginalComments, ObstacleHits))
            {
                for (const TPair<UEdGraphNode*, FIntPoint>& Entry : Original)
                {
                    Entry.Key->NodePosX = Entry.Value.X;
                    Entry.Key->NodePosY = Entry.Value.Y;
                }
                Context.SendError(TEXT("layered: no clear spot for the selection without moving unselected nodes or comments; positions restored. Use ruled or widen the selection."),
                                  TEXT("LAYOUT_COLLISION"));
                return true;
            }
            Result->SetNumberField(TEXT("layeredObstacleHitsBeforeShift"), ObstacleHits);
        }
        else if (bRuled)
        {
            McpRuled::FParams Params;
            Params.bAllowMovingExisting = bFullGraphScope;
            Params.bLocalizeSharedGetters = bLocalizeSharedGetters;
            Params.bRouteBackwardExecs = bRouteBackEdges;
            Params.bProtectExistingComments = !bFullGraphScope;
            Params.bCollapseAutoExecKnots = true;
            FString Failure;
            McpRuled::LayoutNodes(Context.TargetGraph, Listed, nullptr, &AddedNodes, nullptr, Params, &Failure,
                                  nullptr, &UpdatedComments);
            if (!Failure.IsEmpty())
            {
                Context.SendError(Failure, TEXT("LAYOUT_COLLISION"));
                return true;
            }
        }

        const TArray<FString> AfterFingerprint = McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Context.TargetGraph);
        bTopologyMatched = (BeforeFingerprint == AfterFingerprint);
        if (!bTopologyMatched)
        {
            for (const TPair<UEdGraphNode*, FIntPoint>& Entry : Original)
            {
                if (Entry.Key && Context.TargetGraph->Nodes.Contains(Entry.Key))
                {
                    Entry.Key->NodePosX = Entry.Value.X;
                    Entry.Key->NodePosY = Entry.Value.Y;
                }
            }
            Result->SetStringField(TEXT("layoutStatus"), TEXT("rolled_back"));
            Context.SendError(TEXT("Layout aborted because logical pin topology changed unexpectedly; positions restored."),
                              TEXT("TOPOLOGY_MISMATCH"));
            return true;
        }

        const TArray<UEdGraphNode*> Unmoved = bBeside ? GraphBatch::SettleAutoPlacedNodes(Context.Blueprint, State) : TArray<UEdGraphNode*>();
        TSet<UEdGraphNode*> LiveGraphNodes(Context.TargetGraph->Nodes);
        for (const TPair<UEdGraphNode*, FIntPoint>& Entry : Original)
        {
            UEdGraphNode* Node = Entry.Key;
            if (!Node || !LiveGraphNodes.Contains(Node))
            {
                continue;
            }
            if (Unmoved.Contains(Node))
            {
                Node->NodePosX = Entry.Value.X;
                Node->NodePosY = Entry.Value.Y;
                Stayed.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
                continue;
            }
            if (FIntPoint(Node->NodePosX, Node->NodePosY) == Entry.Value)
            {
                InPlace.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
            Row->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Row->SetNumberField(TEXT("x"), Node->NodePosX);
            Row->SetNumberField(TEXT("y"), Node->NodePosY);
            Moved.Add(MakeShared<FJsonValueObject>(Row));
        }
        Context.TargetGraph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Context.Blueprint);
        if (Moved.Num() > 0 || AddedNodes.Num() > 0 || UpdatedComments.Num() > 0)
        {
            Transaction.DescribeInto(Result);
        }
    }
    const bool bSaved = SaveLoadedAssetThrottled(Context.Blueprint);

    TSet<UEdGraphNode*> AfterScopeSet;
    for (UEdGraphNode* N : Listed)
    {
        if (N && Context.TargetGraph->Nodes.Contains(N))
        {
            AfterScopeSet.Add(N);
        }
    }
    AfterScopeSet.Append(AddedNodes);
    const McpGraphLayoutDiag::FLayoutQualityReport AfterQuality =
        McpGraphLayoutDiag::AnalyzeGraphQuality(Context.TargetGraph, bFullGraphScope ? nullptr : &AfterScopeSet,
                                                nullptr, &OriginalComments, &OriginalAllPos);

    const bool bDegraded = AfterQuality.NodeOverlapCount > BeforeQuality.NodeOverlapCount ||
                           AfterQuality.CommentIntrusionCount > 0 ||
                           AfterQuality.DanglingAutoKnotCount > 0;
    const FString Status = (Moved.Num() == 0 && AddedNodes.Num() == 0 && UpdatedComments.Num() == 0)
        ? TEXT("no_changes")
        : (bDegraded ? TEXT("degraded") : TEXT("applied"));

    TSharedRef<FJsonObject> Diag = MakeShared<FJsonObject>();
    Diag->SetObjectField(TEXT("before"), BeforeQuality.ToJson());
    Diag->SetObjectField(TEXT("after"), AfterQuality.ToJson());
    Diag->SetBoolField(TEXT("topologyFingerprintMatched"), bTopologyMatched);
    Diag->SetNumberField(TEXT("addedNodes"), AddedNodes.Num());
    Diag->SetNumberField(TEXT("updatedComments"), UpdatedComments.Num());

    TArray<TSharedPtr<FJsonValue>> UpdatedCommentIds;
    for (UEdGraphNode* Comment : UpdatedComments)
    {
        if (Comment)
        {
            UpdatedCommentIds.Add(MakeShared<FJsonValueString>(Comment->NodeGuid.ToString()));
        }
    }

    Result->SetStringField(TEXT("layoutStatus"), Status);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetArrayField(TEXT("moved"), Moved);
    Result->SetArrayField(TEXT("unmoved"), Stayed);
    Result->SetArrayField(TEXT("alreadyInPlace"), InPlace);
    Result->SetArrayField(TEXT("updatedComments"), UpdatedCommentIds);
    Result->SetObjectField(TEXT("layoutDiagnostics"), Diag);
    Context.SendResponse(FString::Printf(
        TEXT("Moved %d of %d nodes (%s, %s scope; %d already in place, %d comments updated; overlaps %d->%d, backward exec %d->%d, wire crossings %d->%d)."),
        Moved.Num(), Original.Num(), *Layout, *Scope, InPlace.Num(), UpdatedComments.Num(),
        BeforeQuality.NodeOverlapCount, AfterQuality.NodeOverlapCount,
        BeforeQuality.BackwardExecEdgeCount, AfterQuality.BackwardExecEdgeCount,
        BeforeQuality.WireNodeCrossingCount, AfterQuality.WireNodeCrossingCount),
        Result);
    return true;
}
}
