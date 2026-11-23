#include "Domains/BlueprintGraph/McpBPGraphBatchSteps.h"

#include "Foundation/BridgeHelpers/Responses/McpJsonFields.h"
#include "Foundation/GraphLayout/McpRuled.h"

#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"

namespace McpBlueprintGraphHandlers::GraphBatch
{
namespace
{
constexpr int32 AutoColumns = 5;
// A refused slot moves to the overlap guard's own suggestion (the nearest free slot, else free space right
// of what it hit) for this many hops, then to the auto grid, clear of everything the graph held when the batch began.
constexpr int32 MaxSuggestionHops = 24;
constexpr int32 MaxPlacementTries = 48;

// Auto-placed nodes fill a grid right of whatever the graph already holds.
void PlaceOnGrid(FBatchState& State, const TSharedPtr<FJsonObject>& Payload)
{
    const int32 Slot = State.AutoPlaced++;
    Payload->SetNumberField(TEXT("posX"), State.OriginX + (Slot % AutoColumns) * 360.0f);
    Payload->SetNumberField(TEXT("posY"), (Slot / AutoColumns) * 260.0f);
}

bool IsOverlapRefusal(const FMcpCapturedResponse& Reply, const TSharedPtr<FJsonObject>*& OutSuggested)
{
    return !Reply.bSuccess && Reply.ErrorCode == TEXT("NODE_OVERLAP") && Reply.Result.IsValid() &&
           Reply.Result->TryGetObjectField(TEXT("suggestedPosition"), OutSuggested);
}

// The titles of the nodes a refused slot overlapped, for the step's placementWarning.
FString OverlappedTitles(const FMcpCapturedResponse& Refusal)
{
    TArray<FString> Titles;
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (Refusal.Result->TryGetArrayField(TEXT("overlappingNodes"), Nodes))
    {
        for (const TSharedPtr<FJsonValue>& Value : *Nodes)
        {
            const TSharedPtr<FJsonObject>* Node = nullptr;
            FString Title;
            if (Value.IsValid() && Value->TryGetObject(Node) && (*Node)->TryGetStringField(TEXT("title"), Title))
            {
                Titles.AddUnique(Title);
            }
        }
    }
    return Titles.Num() > 0 ? FString::Join(Titles, TEXT(", ")) : FString(TEXT("an existing node"));
}

// Where a node goes beside one it is wired to; the lowest Rank wins. bBetterPending: a neighbour
// that would rank better has not settled yet.
struct FAnchor
{
    int32 Rank = MAX_int32;
    float X = 0.0f;
    float Y = 0.0f;
    bool bBetterPending = false;
};

// Right of what runs it, below-left of what reads it, left of what it runs, right of what feeds it.
// A node still waiting to be settled is no anchor: it sits on the grid.
FAnchor AnchorBeside(const UEdGraphNode& Node, const TSet<const UEdGraphNode*>& Unsettled)
{
    constexpr float Gap = 80.0f;
    float Width = 0.0f;
    float Height = 0.0f;
    McpGraphLayout::EstimateNodeExtent(Node, Width, Height);
    FAnchor Best;
    int32 PendingRank = MAX_int32;
    for (const UEdGraphPin* Pin : Node.Pins)
    {
        if (!Pin)
        {
            continue;
        }
        const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
        const bool bInput = Pin->Direction == EGPD_Input;
        for (const UEdGraphPin* Linked : Pin->LinkedTo)
        {
            const UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
            const int32 Rank = bExec ? (bInput ? 0 : 2) : (bInput ? 3 : 1);
            if (Other && Unsettled.Contains(Other))
            {
                PendingRank = FMath::Min(PendingRank, Rank);
                continue;
            }
            if (!Other || Rank >= Best.Rank)
            {
                continue;
            }
            float OtherW = 0.0f;
            float OtherH = 0.0f;
            McpGraphLayout::EstimateNodeExtent(*Other, OtherW, OtherH);
            Best.Rank = Rank;
            Best.X = Rank == 0 || Rank == 3 ? Other->NodePosX + OtherW + Gap : Other->NodePosX - Width - Gap;
            Best.Y = Rank == 1 ? Other->NodePosY + OtherH + Gap : Other->NodePosY;
        }
    }
    Best.bBetterPending = PendingRank < Best.Rank;
    return Best;
}
// A step that names no graph runs where the node it names is, else in the batch's graph: the links of a function's
// body were looked for in the batch's EventGraph, and the batch failed although every node was there.
void FollowStepGraph(const FActionContext& Parent, const TSharedPtr<FJsonObject>& Payload)
{
    if (Payload->HasField(TEXT("graphName")))
    {
        return;
    }
    const UEdGraphNode* Node = FindBatchNode(Parent.Blueprint, McpGetFirstStringField(Payload,
        {TEXT("fromNodeId"), TEXT("nodeId"), TEXT("nodeGuid"), TEXT("sourceNode")}));
    const FString Graph = Node && Node->GetGraph() ? Node->GetGraph()->GetName()
                                                   : GetJsonStringField(Parent.Payload, TEXT("graphName"));
    if (!Graph.IsEmpty())
    {
        Payload->SetStringField(TEXT("graphName"), Graph);
    }
}

void RecordConnectLinkChange(UBlueprint* Blueprint, FBatchState& State,
                             const TSharedPtr<FJsonObject>& Payload, const FMcpCapturedResponse& Reply)
{
    if (!Reply.bSuccess || !Reply.Result.IsValid())
    {
        return;
    }
    FString ConvGuid;
    if (Reply.Result->TryGetStringField(TEXT("conversionNodeId"), ConvGuid) && !ConvGuid.IsEmpty() &&
        !State.ExistingGuids.Contains(ConvGuid))
    {
        State.AutoPlacedGuids.AddUnique(ConvGuid);
    }
    const FString RawFromGuid = McpGetFirstStringField(
        Payload, {TEXT("fromNodeId"), TEXT("fromNode"), TEXT("sourceNodeGuid"), TEXT("sourceNodeId"), TEXT("sourceNode"), TEXT("nodeId")});
    const FString RawToGuid = McpGetFirstStringField(
        Payload, {TEXT("toNodeId"), TEXT("toNode"), TEXT("targetNodeGuid"), TEXT("targetNodeId"), TEXT("targetNode")});
    if (RawFromGuid.IsEmpty() || RawToGuid.IsEmpty())
    {
        return;
    }
    FString SourcePinName;
    FString TargetPinName;
    FString SourcePinType;
    FString TargetPinType;
    Reply.Result->TryGetStringField(TEXT("sourcePinName"), SourcePinName);
    Reply.Result->TryGetStringField(TEXT("targetPinName"), TargetPinName);
    Reply.Result->TryGetStringField(TEXT("sourcePinType"), SourcePinType);
    Reply.Result->TryGetStringField(TEXT("targetPinType"), TargetPinType);

    UEdGraphNode* FromNode = FindBatchNode(Blueprint, RawFromGuid);
    UEdGraphNode* ToNode = FindBatchNode(Blueprint, RawToGuid);
    if (!FromNode || !ToNode)
    {
        return;
    }
    UEdGraphPin* FromPin = FromNode->FindPin(FName(*SourcePinName));
    UEdGraphPin* ToPin = ToNode->FindPin(FName(*TargetPinName));
    if (FromPin && ToPin && FromPin->Direction == EGPD_Input && ToPin->Direction == EGPD_Output)
    {
        Swap(FromNode, ToNode);
        Swap(FromPin, ToPin);
    }
    FBatchLinkChange Change;
    Change.FromGuid = FromNode->NodeGuid.ToString();
    Change.ToGuid = ToNode->NodeGuid.ToString();
    Change.bExec = (FromPin && FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) ||
                   (ToPin && ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) ||
                   SourcePinType == UEdGraphSchema_K2::PC_Exec.ToString() ||
                   TargetPinType == UEdGraphSchema_K2::PC_Exec.ToString();
    if (Change.bExec && FromPin)
    {
        int32 ExecOutIndex = 0;
        for (const UEdGraphPin* Pin : FromNode->Pins)
        {
            if (Pin && !Pin->bHidden && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                if (Pin == FromPin)
                {
                    Change.bSecondaryExecOut = ExecOutIndex > 0;
                    break;
                }
                ++ExecOutIndex;
            }
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* Replaced = nullptr;
    if (Reply.Result->TryGetArrayField(TEXT("replacedLinks"), Replaced) && Replaced)
    {
        for (const TSharedPtr<FJsonValue>& Item : *Replaced)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            FString OldGuid;
            if (Item.IsValid() && Item->TryGetObject(Obj) && Obj && (*Obj)->TryGetStringField(TEXT("nodeId"), OldGuid) && !OldGuid.IsEmpty())
            {
                Change.ReplacedGuids.AddUnique(OldGuid);
            }
        }
    }
    State.LinkChanges.Add(MoveTemp(Change));
}

bool HasVisibleExecPin(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return false;
    }
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && !Pin->bHidden && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return true;
        }
    }
    return false;
}

bool IsExistingEntryAnchor(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return false;
    }
    if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_FunctionEntry>())
    {
        return true;
    }
    bool bHasExecIn = false;
    bool bHasExecOut = false;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->bHidden || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            continue;
        }
        if (Pin->Direction == EGPD_Input)
        {
            bHasExecIn = true;
        }
        else
        {
            bHasExecOut = true;
        }
    }
    return !bHasExecIn && bHasExecOut;
}

bool IsExecAligned(const UEdGraphNode* Upstream, const UEdGraphNode* Downstream)
{
    if (!Upstream || !Downstream)
    {
        return false;
    }
    const McpRuled::FShape UpShape = McpRuled::EstimateShape(*Upstream);
    const McpRuled::FShape DownShape = McpRuled::EstimateShape(*Downstream);
    const double UpLine = static_cast<double>(Upstream->NodePosY) +
                          static_cast<double>(UpShape.ExecOut.Num() > 0 ? UpShape.ExecOut[0] : UpShape.Height * 0.5f);
    const double DownLine = static_cast<double>(Downstream->NodePosY) +
                            static_cast<double>(DownShape.ExecIn >= 0.0f ? DownShape.ExecIn : DownShape.Height * 0.5f);
    const double UpRight = static_cast<double>(Upstream->NodePosX) + static_cast<double>(UpShape.Width);
    const double DownLeft = static_cast<double>(Downstream->NodePosX);
    return DownLeft >= UpRight - 1.0 && DownLeft <= UpRight + 640.0 && FMath::Abs(DownLine - UpLine) <= 64.0;
}

UEdGraphNode* FindUpstreamFixedExecAnchor(UEdGraphNode* StartNode, const TSet<UEdGraphNode*>& FreeNodes)
{
    TSet<UEdGraphNode*> Visited;
    TArray<UEdGraphNode*> Open = {StartNode};
    while (Open.Num() > 0)
    {
        UEdGraphNode* Current = Open.Pop(EAllowShrinking::No);
        if (!Current || Visited.Contains(Current))
        {
            continue;
        }
        Visited.Add(Current);
        for (const UEdGraphPin* Pin : Current->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Pred = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Pred || Pred->GetGraph() != StartNode->GetGraph())
                {
                    continue;
                }
                if (!FreeNodes.Contains(Pred))
                {
                    return Pred;
                }
                Open.Add(Pred);
            }
        }
    }
    return nullptr;
}
} // namespace

FMcpCapturedResponse RunStep(const FActionContext& Parent, const TSharedPtr<FJsonObject>& Payload,
                             const FString& Edit, const FString& StepId, FString& OutPins)
{
    FollowStepGraph(Parent, Payload);
    FMcpResponseCaptureRegistry::Get().Begin(StepId);
    FActionContext Step{Parent.Subsystem, StepId, Payload, Parent.RequestingSocket, Edit};
    Step.bDeferCompile = true;
    if (!RunBlueprintMemberStep(Parent, Edit, StepId, Payload) && PrepareBlueprintAndGraph(Step))
    {
        const bool bHandled = HandleNodeCreationAction(Step) || HandlePinMutationAction(Step) ||
                              HandleNodeMutationAction(Step);
        (void)bHandled;
    }
    FMcpCapturedResponse Reply = FMcpResponseCaptureRegistry::Get().End(StepId);
    FString Guid;
    if (Reply.bSuccess && Reply.Result.IsValid() && Reply.Result->TryGetStringField(TEXT("nodeGuid"), Guid))
    {
        // A member step (add_event: its parameter pins are what later steps wire) prepares no
        // graph, so its node is found across the Blueprint; it used to report no pins at all.
        OutPins = DescribeNodePins(Step.TargetGraph ? Step.FindNode(Guid) : FindBatchNode(Parent.Blueprint, Guid));
    }
    return Reply;
}

// A create step that names no position is auto-placed. A slot that turns out to be taken, auto or
// caller-chosen, is never a reason to fail the batch: the node moves to the overlap guard's own
// suggestion, hop by hop (a row of nodes can take a dozen), and to the auto grid if those run out.
// A caller who named the position is told where the node went, in the step's placementWarning.
FMcpCapturedResponse RunPlacedStep(const FActionContext& Parent, FBatchState& State,
                                   const TSharedPtr<FJsonObject>& Payload, const FString& Edit,
                                   const FString& StepId, FString& OutPins)
{
    const bool bCreates = Edit == TEXT("create_node") || Edit == TEXT("create_reroute_node");
    // add_event reads posX/posY too: without them its node landed on the graph origin, over whatever sat there.
    const bool bAuto = (bCreates || Edit == TEXT("add_event")) && !Payload->HasField(TEXT("posX")) &&
                       !Payload->HasField(TEXT("x"));
    if (bAuto)
    {
        PlaceOnGrid(State, Payload);
    }
    // The handlers read `x`/`y` before `posX`/`posY`; move them over so a move below
    // (which writes posX/posY) takes effect.
    double Coord = 0.0;
    if (Payload->TryGetNumberField(TEXT("x"), Coord))
    {
        Payload->SetNumberField(TEXT("posX"), Coord);
        Payload->RemoveField(TEXT("x"));
    }
    if (Payload->TryGetNumberField(TEXT("y"), Coord))
    {
        Payload->SetNumberField(TEXT("posY"), Coord);
        Payload->RemoveField(TEXT("y"));
    }
    double RequestedX = 0.0;
    double RequestedY = 0.0;
    Payload->TryGetNumberField(TEXT("posX"), RequestedX);
    Payload->TryGetNumberField(TEXT("posY"), RequestedY);
    FMcpCapturedResponse Reply = RunStep(Parent, Payload, Edit, StepId, OutPins);
    const TSharedPtr<FJsonObject>* Suggested = nullptr;
    FString Overlapped;
    int32 Moves = 0;
    for (; bCreates && Moves < MaxPlacementTries && IsOverlapRefusal(Reply, Suggested); ++Moves)
    {
        if (Moves == 0)
        {
            Overlapped = OverlappedTitles(Reply);
        }
        if (Moves < MaxSuggestionHops)
        {
            Payload->SetNumberField(TEXT("posX"), (*Suggested)->GetNumberField(TEXT("x")));
            Payload->SetNumberField(TEXT("posY"), (*Suggested)->GetNumberField(TEXT("y")));
        }
        else
        {
            PlaceOnGrid(State, Payload);
        }
        Reply = RunStep(Parent, Payload, Edit, StepId, OutPins);
    }
    FString CreatedGuid;
    if ((bCreates || Edit == TEXT("add_event")) && Reply.bSuccess && Reply.Result.IsValid() &&
        Reply.Result->TryGetStringField(TEXT("nodeGuid"), CreatedGuid) && !State.ExistingGuids.Contains(CreatedGuid))
    {
        if (bAuto)
        {
            State.AutoPlacedGuids.AddUnique(CreatedGuid);
        }
        else
        {
            State.ExplicitlyPlacedGuids.Add(CreatedGuid);
        }
    }
    if (Edit == TEXT("connect_pins"))
    {
        RecordConnectLinkChange(Parent.Blueprint, State, Payload, Reply);
    }
    if (Moves > 0 && !bAuto && Reply.bSuccess && Reply.Result.IsValid())
    {
        double PlacedX = RequestedX;
        double PlacedY = RequestedY;
        Payload->TryGetNumberField(TEXT("posX"), PlacedX);
        Payload->TryGetNumberField(TEXT("posY"), PlacedY);
        FString Warning = FString::Printf(
            TEXT("Requested position (%d, %d) overlaps %s; the node was placed at (%d, %d) instead."),
            FMath::RoundToInt(RequestedX), FMath::RoundToInt(RequestedY), *Overlapped,
            FMath::RoundToInt(PlacedX), FMath::RoundToInt(PlacedY));
        FString Existing;
        if (Reply.Result->TryGetStringField(TEXT("placementWarning"), Existing) && !Existing.IsEmpty())
        {
            Warning += TEXT(" ") + Existing;
        }
        Reply.Result->SetStringField(TEXT("placementWarning"), Warning);
    }
    return Reply;
}

// The grid sits right of everything the graph held, so a chain added to a big graph landed thousands of
// units from the event it hangs off (a Bounce event at x 7646 wired to a new node at x 46464). Each
// auto-placed node now moves beside a node it is wired to, the nearest free slot when that spot is taken;
// one wired only to other new nodes follows them once they have moved, and one with no free slot stays.
TArray<UEdGraphNode*> SettleAutoPlacedNodes(UBlueprint* Blueprint, const FBatchState& State)
{
    TArray<UEdGraphNode*> Pending;
    TSet<const UEdGraphNode*> Unsettled;
    for (const FString& Guid : State.AutoPlacedGuids)
    {
        if (UEdGraphNode* Node = FindBatchNode(Blueprint, Guid))
        {
            Pending.Add(Node);
            Unsettled.Add(Node);
        }
    }
    // A node waits while the neighbour it belongs beside has not settled: a pure node went beside the
    // far-left variable it reads (x 8494) when the Branch reading it settled later (x 10134). A pass that
    // settles nothing lets one node take its best settled neighbour, then the waiting resumes.
    for (bool bRelax = false;;)
    {
        bool bMoved = false;
        for (int32 Index = 0; Index < Pending.Num() && !(bRelax && bMoved); ++Index)
        {
            UEdGraphNode* Node = Pending[Index];
            const FAnchor Anchor = AnchorBeside(*Node, Unsettled);
            if (Anchor.bBetterPending && !bRelax)
            {
                continue;
            }
            float Width = 0.0f;
            float Height = 0.0f;
            McpGraphLayout::EstimateNodeExtent(*Node, Width, Height);
            float X = Anchor.X;
            float Y = Anchor.Y;
            // A taken spot stays on the anchor's side: right of what runs or feeds the node, left of what
            // reads it or what it runs.
            const int32 PreferDX = Anchor.Rank == 0 || Anchor.Rank == 3 ? 1 : -1;
            TArray<McpGraphLayout::FGraphNodeOccupant> Occupants;
            if (Anchor.Rank == MAX_int32 ||
                (McpGraphLayout::CheckGraphNodeOverlap(Node->GetGraph(), X, Y, Width, Height, Occupants,
                                                       McpGraphLayout::NodeOverlapPadding, Node) &&
                 !McpGraphLayout::FindNearestFreeSlot(Node->GetGraph(), Anchor.X, Anchor.Y, Width, Height, Node, X, Y, PreferDX)))
            {
                continue;
            }
            Node->NodePosX = FMath::RoundToInt(X);
            Node->NodePosY = FMath::RoundToInt(Y);
            Unsettled.Remove(Node);
            Pending.RemoveAt(Index--);
            bMoved = true;
        }
        if (!bMoved && bRelax)
        {
            break;
        }
        bRelax = !bMoved;
    }
    return Pending;
}

void LayoutAutoPlacedNodes(UBlueprint* Blueprint, const FBatchState& State)
{
    if (!Blueprint)
    {
        return;
    }
    TSet<UEdGraphNode*> FreeNodes;
    TArray<UEdGraphNode*> InitialOrder;
    for (const FString& Guid : State.AutoPlacedGuids)
    {
        UEdGraphNode* Node = FindBatchNode(Blueprint, Guid);
        if (Node && Node->GetGraph() && !FreeNodes.Contains(Node))
        {
            FreeNodes.Add(Node);
            InitialOrder.Add(Node);
        }
    }
    const auto IsFixedAnchor = [&State](const UEdGraphNode* Node) -> bool
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>())
        {
            return true;
        }
        const FString GuidStr = Node->NodeGuid.ToString();
        if (State.ExplicitlyPlacedGuids.Contains(GuidStr))
        {
            return true;
        }
        return State.ExistingGuids.Contains(GuidStr) && IsExistingEntryAnchor(Node);
    };

    // Seed existing exec nodes that were rewired onto a new/misaligned predecessor or moved to a secondary exec pin.
    TSet<UEdGraphNode*> SeededExistingExec;
    for (const FBatchLinkChange& Change : State.LinkChanges)
    {
        if (!Change.bExec)
        {
            continue;
        }
        UEdGraphNode* FromNode = FindBatchNode(Blueprint, Change.FromGuid);
        UEdGraphNode* ToNode = FindBatchNode(Blueprint, Change.ToGuid);
        if (!FromNode || !ToNode || FromNode->GetGraph() != ToNode->GetGraph() || IsFixedAnchor(ToNode) ||
            FreeNodes.Contains(ToNode))
        {
            continue;
        }
        bool bShouldReflowTarget = Change.bSecondaryExecOut;
        if (!bShouldReflowTarget)
        {
            if (FreeNodes.Contains(FromNode))
            {
                UEdGraphNode* Anchor = FindUpstreamFixedExecAnchor(FromNode, FreeNodes);
                bShouldReflowTarget = !Anchor || !IsExecAligned(Anchor, ToNode);
            }
            else
            {
                bShouldReflowTarget = !IsExecAligned(FromNode, ToNode);
            }
        }
        if (bShouldReflowTarget)
        {
            SeededExistingExec.Add(ToNode);
        }
    }

    // Expand forward along affected exec chains up to untouched merge boundaries (preserving internal diamonds).
    if (SeededExistingExec.Num() > 0)
    {
        constexpr int32 MaxExpandedExec = 96;
        TSet<UEdGraphNode*> ExpandedExec = SeededExistingExec;
        TSet<UEdGraphNode*> VisitedExec = SeededExistingExec;
        TArray<UEdGraphNode*> Open = SeededExistingExec.Array();
        while (Open.Num() > 0 && ExpandedExec.Num() < MaxExpandedExec)
        {
            UEdGraphNode* Current = Open.Pop(EAllowShrinking::No);
            for (const UEdGraphPin* Pin : Current->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Next = Linked ? Linked->GetOwningNode() : nullptr;
                    if (!Next || Next->GetGraph() != Current->GetGraph() || IsFixedAnchor(Next) ||
                        VisitedExec.Contains(Next))
                    {
                        continue;
                    }
                    VisitedExec.Add(Next);
                    if (!FreeNodes.Contains(Next))
                    {
                        ExpandedExec.Add(Next);
                    }
                    Open.Add(Next);
                    if (ExpandedExec.Num() >= MaxExpandedExec)
                    {
                        break;
                    }
                }
            }
        }
        // Prune any non-seeded node that merges an untouched external exec path so untouched branches stay anchored.
        for (bool bPruned = true; bPruned;)
        {
            bPruned = false;
            TArray<UEdGraphNode*> ToRemove;
            for (UEdGraphNode* Node : ExpandedExec)
            {
                if (SeededExistingExec.Contains(Node))
                {
                    continue;
                }
                bool bHasExternalExecIn = false;
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                    {
                        continue;
                    }
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                    {
                        UEdGraphNode* Pred = Linked ? Linked->GetOwningNode() : nullptr;
                        if (Pred && Pred->GetGraph() == Node->GetGraph() && !FreeNodes.Contains(Pred) &&
                            !ExpandedExec.Contains(Pred))
                        {
                            bHasExternalExecIn = true;
                            break;
                        }
                    }
                    if (bHasExternalExecIn)
                    {
                        break;
                    }
                }
                if (bHasExternalExecIn)
                {
                    ToRemove.Add(Node);
                }
            }
            for (UEdGraphNode* Node : ToRemove)
            {
                ExpandedExec.Remove(Node);
                bPruned = true;
            }
        }
        FreeNodes.Append(ExpandedExec);
    }

    // Seed misplaced exclusive pure data producers directly wired into an existing fixed node.
    for (const FBatchLinkChange& Change : State.LinkChanges)
    {
        if (Change.bExec)
        {
            continue;
        }
        UEdGraphNode* FromNode = FindBatchNode(Blueprint, Change.FromGuid);
        UEdGraphNode* ToNode = FindBatchNode(Blueprint, Change.ToGuid);
        if (!FromNode || !ToNode || FromNode->GetGraph() != ToNode->GetGraph() || HasVisibleExecPin(FromNode) ||
            IsFixedAnchor(FromNode) || FreeNodes.Contains(FromNode))
        {
            continue;
        }
        bool bExclusiveToTargetOrFree = false;
        bool bSharedOutside = false;
        for (const UEdGraphPin* Pin : FromNode->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                UEdGraphNode* Consumer = Linked ? Linked->GetOwningNode() : nullptr;
                if (!Consumer)
                {
                    continue;
                }
                if (Consumer == ToNode || FreeNodes.Contains(Consumer))
                {
                    bExclusiveToTargetOrFree = true;
                }
                else
                {
                    bSharedOutside = true;
                    break;
                }
            }
            if (bSharedOutside)
            {
                break;
            }
        }
        const bool bMisplaced = FromNode->NodePosX >= ToNode->NodePosX ||
                                (ToNode->NodePosX - FromNode->NodePosX) > 800 ||
                                FMath::Abs(FromNode->NodePosY - ToNode->NodePosY) > 320;
        if (bExclusiveToTargetOrFree && !bSharedOutside && bMisplaced)
        {
            FreeNodes.Add(FromNode);
        }
    }

    // Pull in exclusive pure data upstream feeding any node in FreeNodes (never moving nodes shared with untouched consumers).
    for (bool bGrew = true; bGrew;)
    {
        bGrew = false;
        TArray<UEdGraphNode*> Candidates;
        for (UEdGraphNode* Node : FreeNodes)
        {
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Producer = Linked ? Linked->GetOwningNode() : nullptr;
                    if (!Producer || Producer->GetGraph() != Node->GetGraph() || HasVisibleExecPin(Producer) ||
                        IsFixedAnchor(Producer) || FreeNodes.Contains(Producer))
                    {
                        continue;
                    }
                    bool bHasOutput = false;
                    bool bSharedOutside = false;
                    for (const UEdGraphPin* OutPin : Producer->Pins)
                    {
                        if (!OutPin || OutPin->Direction != EGPD_Output || OutPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                        {
                            continue;
                        }
                        for (const UEdGraphPin* OutLink : OutPin->LinkedTo)
                        {
                            UEdGraphNode* Reader = OutLink ? OutLink->GetOwningNode() : nullptr;
                            if (!Reader)
                            {
                                continue;
                            }
                            bHasOutput = true;
                            if (!FreeNodes.Contains(Reader))
                            {
                                bSharedOutside = true;
                                break;
                            }
                        }
                        if (bSharedOutside)
                        {
                            break;
                        }
                    }
                    if (bHasOutput && !bSharedOutside)
                    {
                        Candidates.AddUnique(Producer);
                    }
                }
            }
        }
        for (UEdGraphNode* Producer : Candidates)
        {
            FreeNodes.Add(Producer);
            bGrew = true;
        }
    }

    if (FreeNodes.Num() == 0)
    {
        return;
    }

    TMap<UEdGraph*, TArray<UEdGraphNode*>> ByGraph;
    TSet<UEdGraphNode*> Added;
    for (UEdGraphNode* Node : InitialOrder)
    {
        if (Node && Node->GetGraph() && FreeNodes.Contains(Node) && !Added.Contains(Node))
        {
            ByGraph.FindOrAdd(Node->GetGraph()).Add(Node);
            Added.Add(Node);
        }
    }
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && FreeNodes.Contains(Node) && !Added.Contains(Node))
            {
                ByGraph.FindOrAdd(Graph).Add(Node);
                Added.Add(Node);
            }
        }
    }
    for (const TPair<UEdGraph*, TArray<UEdGraphNode*>>& Each : ByGraph)
    {
        McpRuled::LayoutNodes(Each.Key, Each.Value);
    }
}
}
