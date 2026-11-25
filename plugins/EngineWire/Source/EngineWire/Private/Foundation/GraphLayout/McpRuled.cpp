#include "Foundation/GraphLayout/McpRuled.h"

namespace McpRuled
{
namespace RuledSteps
{
bool IsExecShape(const FShape& Shape)
{
    return Shape.ExecIn >= 0.0f || Shape.ExecOut.Num() > 0;
}

// Where a node's white line runs, from its top: its exec input, else its first exec output (an event).
float ExecLine(const FShape& Shape)
{
    return Shape.ExecIn >= 0.0f ? Shape.ExecIn : (Shape.ExecOut.Num() > 0 ? Shape.ExecOut[0] : Shape.Height * 0.5f);
}

// Nodes at positions relative to a local origin, moved as one when placed.
struct FPart
{
    TArray<int32> Nodes;
    TArray<FVector2D> At;

    void Add(int32 Node, const FVector2D& Where)
    {
        Nodes.Add(Node);
        At.Add(Where);
    }
    void Append(const FPart& Other, const FVector2D& Offset)
    {
        for (int32 Index = 0; Index < Other.Nodes.Num(); ++Index)
        {
            Add(Other.Nodes[Index], Other.At[Index] + Offset);
        }
    }
};

class FSolver
{
public:
    FSolver(const FProblem& InProblem, const FParams& InParams) : P(InProblem), K(InParams), Count(InProblem.Shapes.Num())
    {
        In.SetNum(Count);
        Out.SetNum(Count);
        for (int32 Link = 0; Link < P.Links.Num(); ++Link)
        {
            const FLink& Each = P.Links[Link];
            if (P.Shapes.IsValidIndex(Each.From) && P.Shapes.IsValidIndex(Each.To) && Each.From != Each.To)
            {
                Out[Each.From].Add(Link);
                In[Each.To].Add(Link);
            }
        }
        for (TArray<int32>& List : Out)
        {
            List.StableSort([this](int32 A, int32 B) { return P.Links[A].FromPin < P.Links[B].FromPin; });
        }
        for (TArray<int32>& List : In)
        {
            List.StableSort([this](int32 A, int32 B) { return P.Links[A].ToPin < P.Links[B].ToPin; });
        }
        Pos = P.Pos;
        Pos.SetNum(Count);
        Done.Init(false, Count);
        for (int32 Node = 0; Node < Count; ++Node)
        {
            Done[Node] = !IsFree(Node);
        }
        // Regions: partition strictly by exec ownership first so shared pure variables or delegate
        // bindings never merge unrelated events into one giant region (EW-002 / EW-005).
        Region.SetNum(Count);
        for (int32 Node = 0; Node < Count; ++Node)
        {
            Region[Node] = Node;
        }
        const auto Root = [this](int32 Node)
        {
            while (Region[Node] != Node)
            {
                Node = Region[Node] = Region[Region[Node]];
            }
            return Node;
        };
        bool bAnyExecNode = false;
        for (int32 Node = 0; Node < Count; ++Node)
        {
            bAnyExecNode |= IsExecShape(P.Shapes[Node]);
        }
        for (const FLink& Each : P.Links)
        {
            if (Each.bExec && Region.IsValidIndex(Each.From) && Region.IsValidIndex(Each.To))
            {
                Region[Root(Each.From)] = Root(Each.To);
            }
        }
        for (int32 Node = 0; Node < Count; ++Node)
        {
            Region[Node] = Root(Node);
        }
        if (bAnyExecNode)
        {
            // Assign pure data nodes to the first exec consumer region that reaches them via data links
            // without merging distinct exec regions that happen to share a pure producer.
            TArray<bool> ClaimedByExec;
            ClaimedByExec.Init(false, Count);
            TArray<int32> PureQueue;
            for (int32 Node = 0; Node < Count; ++Node)
            {
                if (IsExecShape(P.Shapes[Node]))
                {
                    ClaimedByExec[Node] = true;
                    PureQueue.Add(Node);
                }
            }
            for (int32 Head = 0; Head < PureQueue.Num(); ++Head)
            {
                const int32 Consumer = PureQueue[Head];
                for (const int32 Link : In[Consumer])
                {
                    const FLink& Each = P.Links[Link];
                    if (Each.bExec || Each.bDelegate || ClaimedByExec[Each.From] || IsExecShape(P.Shapes[Each.From]))
                    {
                        continue;
                    }
                    ClaimedByExec[Each.From] = true;
                    Region[Each.From] = Region[Consumer];
                    PureQueue.Add(Each.From);
                }
            }
            // Group remaining unconsumed (dead) pure nodes among themselves.
            for (const FLink& Each : P.Links)
            {
                if (!Each.bExec && !Each.bDelegate && Region.IsValidIndex(Each.From) && Region.IsValidIndex(Each.To) &&
                    !ClaimedByExec[Each.From] && !ClaimedByExec[Each.To])
                {
                    Region[Root(Each.From)] = Root(Each.To);
                }
            }
            for (int32 Node = 0; Node < Count; ++Node)
            {
                if (!ClaimedByExec[Node])
                {
                    Region[Node] = Root(Node);
                }
            }
        }
        else
        {
            // Pure-only graph: union non-delegate data links as before.
            for (const FLink& Each : P.Links)
            {
                if (!Each.bDelegate && Region.IsValidIndex(Each.From) && Region.IsValidIndex(Each.To))
                {
                    Region[Root(Each.From)] = Root(Each.To);
                }
            }
            for (int32 Node = 0; Node < Count; ++Node)
            {
                Region[Node] = Root(Node);
            }
        }
        ObstacleAt = P.Obstacles;
        RebuildPlaced();
    }

    FSolution Run()
    {
        // Rows that go on from a node that stays, in link order; then rows from nodes nothing runs (events).
        for (int32 Link = 0; Link < P.Links.Num(); ++Link)
        {
            const FLink& Each = P.Links[Link];
            if (Each.bExec && In[Each.To].Contains(Link) && !IsFree(Each.From) && IsFree(Each.To) && IsExecShape(P.Shapes[Each.To]))
            {
                Queue.Add({Each.To, Link, false});
            }
        }
        TArray<FStart> DelegateRoots;
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (IsFree(Node) && IsExecShape(P.Shapes[Node]) &&
                !In[Node].ContainsByPredicate([this](int32 Link) { return P.Links[Link].bExec; }))
            {
                const bool bIsDelegateCallback =
                    Out[Node].ContainsByPredicate([this](int32 Link) { return P.Links[Link].bDelegate; }) ||
                    In[Node].ContainsByPredicate([this](int32 Link) { return P.Links[Link].bDelegate; });
                if (bIsDelegateCallback)
                {
                    DelegateRoots.Add({Node, INDEX_NONE, false});
                }
                else
                {
                    Queue.Add({Node, INDEX_NONE, false});
                }
            }
        }
        // Schedule delegate callback events after regular roots/continuations so their BindEvent partner
        // is already placed and can provide a local spatial anchor (EW-005).
        Queue.Append(DelegateRoots);
        NextRootY = P.Origin.Y;
        Drain();
        // An exec node reached only round a loop of free nodes: the loop starts at it.
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (!Done[Node] && IsExecShape(P.Shapes[Node]))
            {
                Queue.Add({Node, INDEX_NONE, false});
                Drain();
            }
        }
        // Data read by a node that stays (or is placed): laid out as that node's input group, beside it.
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (!Done[Node])
            {
                continue;
            }
            FPart Inputs;
            Feeds(Node, FVector2D::ZeroVector, Inputs);
            if (Inputs.Nodes.Num() > 0)
            {
                PlaceBlock(Inputs, Pos[Node], false, static_cast<float>(Pos[Node].Y));
            }
        }
        // Pure nodes no free node reads: beside the placed node that reads them, else at the origin.
        for (int32 Node = 0; Node < Count; ++Node)
        {
            const bool bTop = !Out[Node].ContainsByPredicate([this](int32 Link) { return IsFree(P.Links[Link].To) && !Done[P.Links[Link].To]; });
            if (!Done[Node] && bTop)
            {
                PlaceValue(Node);
            }
        }
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (!Done[Node])
            {
                PlaceValue(Node);
            }
        }
        // Route terminal completion down a short corridor beside its source, then along the bottom lane.
        // Do this after floating regions settle so every point uses the final graph positions.
        TArray<int32> Terminals = TerminalRows.Array();
        Terminals.Sort();
        for (const int32 Terminal : Terminals)
        {
            // An early-exit result may have been encountered before a loop's lower body row. Revisit it
            // now, after every branch and value is placed, rather than routing across later rows.
            FPart Result;
            Result.Add(Terminal, FVector2D::ZeroVector);
            // Carry private data inputs (not a shared source or a fixed node) with their return.
            for (int32 Reader = 0; Reader < Result.Nodes.Num(); ++Reader)
            {
                for (const int32 Link : In[Result.Nodes[Reader]])
                {
                    const FLink& Wire = P.Links[Link];
                    if (Wire.bExec || !IsFree(Wire.From) || IsExecShape(P.Shapes[Wire.From]) || Result.Nodes.Contains(Wire.From)) { continue; }
                    const bool bPrivate = !Out[Wire.From].ContainsByPredicate([&](int32 Output) { return !Result.Nodes.Contains(P.Links[Output].To); });
                    if (bPrivate) { Result.Add(Wire.From, Pos[Wire.From] - Pos[Terminal]); }
                }
            }
            FVector2D Want = Pos[Terminal];
            for (int32 Other = 0; Other < Count; ++Other)
            {
                if (!Result.Nodes.Contains(Other) && Region[Other] == Region[Terminal] && !TerminalRows.Contains(Other))
                {
                    Want.X = FMath::Max(Want.X, Pos[Other].X + P.Shapes[Other].Width + K.ExecGap);
                    Want.Y = FMath::Max(Want.Y, Pos[Other].Y + P.Shapes[Other].Height + K.RowGap);
                }
            }
            if (!Want.Equals(Pos[Terminal], 0.5))
            {
                float TerminalPushLine = NoLine;
                for (const int32 Link : In[Terminal])
                {
                    const FLink& Wire = P.Links[Link];
                    if (Wire.bExec)
                    {
                        const float SourceLine = static_cast<float>(Pos[Wire.From].Y + ExecLine(P.Shapes[Wire.From]));
                        TerminalPushLine = TerminalPushLine <= NoLine ? SourceLine : FMath::Min(TerminalPushLine, SourceLine);
                    }
                }
                for (const int32 ResultNode : Result.Nodes)
                {
                    Done[ResultNode] = false;
                    Settled.Remove(ResultNode);
                }
                RebuildPlaced();
                PlaceBlock(Result, Want, false, TerminalPushLine, TerminalPushLine > NoLine);
            }
            TMap<int32, FVector2D> SourceExits;
            for (const int32 Link : In[Terminal])
            {
                const FLink& Wire = P.Links[Link];
                if (!Wire.bExec) { continue; }
                const FShape& Source = P.Shapes[Wire.From];
                const float Output = Source.ExecOut.IsValidIndex(Wire.FromPin) ? Source.ExecOut[Wire.FromPin] : ExecLine(Source);
                const float X = Pos[Wire.From].X + Source.Width + K.DataGap * 0.5f;
                FRowBreak Route;
                Route.Link = Link;
                const FVector2D* ExistingExit = SourceExits.Find(Wire.From);
                Route.A = ExistingExit ? *ExistingExit : FVector2D(X, Pos[Wire.From].Y + Output);
                SourceExits.Add(Wire.From, Route.A);
                Route.B = FVector2D(X, Pos[Terminal].Y + P.Shapes[Terminal].ExecIn);
                Breaks.Add(Route);
            }
        }
        if (K.bRouteBackwardExecs)
        {
            RouteBackwardExecEdges();
        }
        return FSolution{Pos, Breaks};
    }

private:
    struct FStart
    {
        int32 Node = INDEX_NONE;
        // The exec wire into Node from the node it goes on from; none for a root.
        int32 Link = INDEX_NONE;
        // A row break: Node starts the next row of the row Link leaves.
        bool bBreak = false;
        // A second exec output's row: it belongs below the row it leaves, so it never slides along it.
        bool bSide = false;
    };

    const FProblem& P;
    const FParams& K;
    const int32 Count;
    TArray<TArray<int32>> In;
    TArray<TArray<int32>> Out;
    TArray<FVector2D> Pos;
    TArray<bool> Done;
    TArray<int32> Region;
    TArray<FBox2D> PlacedBox;
    TArray<int32> PlacedRegion;
    TMap<int32, FBox2D> RegionBox;
    TArray<FBox2D> ObstacleAt;
    TArray<FStart> Queue;
    // Free nodes placed so far.
    TSet<int32> Settled;
    // A new event's region while its rows are laid out among themselves; then it moves as one clear of the others.
    int32 Floating = INDEX_NONE;
    TArray<int32> FloatingNodes;
    int32 FloatingBreaks = 0;
    TArray<FRowBreak> Breaks;
    TSet<int32> TerminalRows;
    // For a break: the row it leaves, so the reroutes go between that row and the next.
    TMap<int32, TArray<int32>> RowOfBreak;
    TMap<int32, float> RowLeftOfBreak;
    float NextRootY = 0.0f;
    // No line to keep: PlaceBlock only moves the block itself.
    static constexpr float NoLine = -1.0e30f;

    bool IsFree(int32 Node) const
    {
        return P.Free.IsValidIndex(Node) && P.Free[Node];
    }
    float Width(int32 Node) const
    {
        return P.Shapes[Node].Width;
    }
    FBox2D BoxAt(int32 Node, const FVector2D& TopLeft) const
    {
        return FBox2D(TopLeft, TopLeft + FVector2D(P.Shapes[Node].Width, P.Shapes[Node].Height));
    }
    static FBox2D Bounds(const FProblem& Problem, const FPart& Part)
    {
        FBox2D Box(ForceInit);
        for (int32 Index = 0; Index < Part.Nodes.Num(); ++Index)
        {
            const FShape& Shape = Problem.Shapes[Part.Nodes[Index]];
            Box += Part.At[Index];
            Box += Part.At[Index] + FVector2D(Shape.Width, Shape.Height);
        }
        return Box;
    }

    // Computes left-to-right DAG column X coordinates for every unclaimed free pure node feeding Root (at RootX)
    // so a shared pure producer read at multiple depths always sits to the left of its deepest reader.
    void ComputeDataX(int32 Root, float RootX, TMap<int32, float>& OutX) const
    {
        TArray<int32> Reachable;
        TSet<int32> Seen;
        TArray<int32> Open = {Root};
        while (Open.Num() > 0)
        {
            const int32 Consumer = Open.Pop(EAllowShrinking::No);
            const FShape& Reader = P.Shapes[Consumer];
            for (const int32 Link : In[Consumer])
            {
                const FLink& Each = P.Links[Link];
                if (Each.bExec || Done[Each.From] || IsExecShape(P.Shapes[Each.From]) ||
                    !P.Shapes[Each.From].DataOut.IsValidIndex(Each.FromPin) || !Reader.DataIn.IsValidIndex(Each.ToPin))
                {
                    continue;
                }
                if (!Seen.Contains(Each.From))
                {
                    Seen.Add(Each.From);
                    Reachable.Add(Each.From);
                    Open.Add(Each.From);
                }
            }
        }
        if (Reachable.Num() == 0)
        {
            return;
        }
        for (const int32 Node : Reachable)
        {
            OutX.Add(Node, TNumericLimits<float>::Max());
        }
        const FShape& RootShape = P.Shapes[Root];
        for (const int32 Link : In[Root])
        {
            const FLink& Each = P.Links[Link];
            if (!Each.bExec && Seen.Contains(Each.From) && P.Shapes[Each.From].DataOut.IsValidIndex(Each.FromPin) &&
                RootShape.DataIn.IsValidIndex(Each.ToPin))
            {
                float& X = OutX[Each.From];
                X = FMath::Min(X, RootX - K.DataGap - Width(Each.From));
            }
        }
        for (int32 Pass = 0; Pass < Reachable.Num(); ++Pass)
        {
            bool bChanged = false;
            for (const int32 Consumer : Reachable)
            {
                const float ConsumerX = OutX[Consumer];
                if (ConsumerX >= TNumericLimits<float>::Max() * 0.5f)
                {
                    continue;
                }
                const FShape& Reader = P.Shapes[Consumer];
                for (const int32 Link : In[Consumer])
                {
                    const FLink& Each = P.Links[Link];
                    if (!Each.bExec && Seen.Contains(Each.From) && P.Shapes[Each.From].DataOut.IsValidIndex(Each.FromPin) &&
                        Reader.DataIn.IsValidIndex(Each.ToPin))
                    {
                        const float WantX = ConsumerX - K.DataGap - Width(Each.From);
                        if (WantX < OutX[Each.From] - 0.5f)
                        {
                            OutX[Each.From] = WantX;
                            bChanged = true;
                        }
                    }
                }
            }
            if (!bChanged)
            {
                break;
            }
        }
    }

    // Recursive worker for Feeds: places unclaimed pure inputs using precomputed DAG X positions (FixedX).
    // Single-column leaves keep uniform centered pitch; multi-node subtrees pack using exact 2D box clearance so
    // distant X columns do not force giant vertical gaps.
    void FeedsWithX(int32 Consumer, float AtY, const TMap<int32, float>& FixedX, FPart& Part)
    {
        struct FChild
        {
            FPart Tree;
            FBox2D Box;
            float Anchor = 0.0f;
            float Target = 0.0f;
        };
        TArray<FChild> Kids;
        const FShape& Reader = P.Shapes[Consumer];
        for (const int32 Link : In[Consumer])
        {
            const FLink& Each = P.Links[Link];
            if (Each.bExec || Done[Each.From] || IsExecShape(P.Shapes[Each.From]) ||
                !P.Shapes[Each.From].DataOut.IsValidIndex(Each.FromPin) || !Reader.DataIn.IsValidIndex(Each.ToPin))
            {
                continue;
            }
            const float* NodeX = FixedX.Find(Each.From);
            if (!NodeX)
            {
                continue;
            }
            Done[Each.From] = true;
            FChild& Kid = Kids.AddDefaulted_GetRef();
            Kid.Tree.Add(Each.From, FVector2D(*NodeX, 0.0));
            FeedsWithX(Each.From, 0.0f, FixedX, Kid.Tree);
            Kid.Box = Bounds(P, Kid.Tree);
            Kid.Anchor = P.Shapes[Each.From].DataOut[Each.FromPin];
            Kid.Target = Reader.DataIn[Each.ToPin];
        }
        if (Kids.Num() == 0)
        {
            return;
        }
        if (Kids.Num() == 1)
        {
            Part.Append(Kids[0].Tree, FVector2D(0.0, AtY + Kids[0].Target - Kids[0].Anchor));
            return;
        }
        bool bAllLeaves = true;
        for (const FChild& Kid : Kids)
        {
            bAllLeaves &= (Kid.Tree.Nodes.Num() == 1);
        }
        if (bAllLeaves)
        {
            // Output pins an even pitch apart, the least that keeps the subtrees InputGap apart, centred on the input pins.
            float Pitch = 0.0f;
            for (int32 Index = 0; Index + 1 < Kids.Num(); ++Index)
            {
                const FChild& Upper = Kids[Index];
                const FChild& Lower = Kids[Index + 1];
                Pitch = FMath::Max(Pitch, (Upper.Box.Max.Y - Upper.Anchor) + K.InputGap + (Lower.Anchor - Lower.Box.Min.Y));
            }
            const float First = (Kids[0].Target + Kids.Last().Target) * 0.5f - (Kids.Num() - 1) * Pitch * 0.5f;
            for (int32 Index = 0; Index < Kids.Num(); ++Index)
            {
                const FChild& Kid = Kids[Index];
                const float Line = First + Index * Pitch;
                Part.Append(Kid.Tree, FVector2D(0.0, AtY + Line - Kid.Anchor));
            }
            return;
        }
        // Multi-node subtrees: pack each sibling against earlier siblings using actual 2D X-overlap clearance while
        // preserving top-to-bottom pin order.
        TArray<float> RelY;
        RelY.SetNum(Kids.Num());
        TArray<FBox2D> Placed;
        RelY[0] = 0.0f;
        for (int32 NodeAt = 0; NodeAt < Kids[0].Tree.Nodes.Num(); ++NodeAt)
        {
            const FVector2D Pos0(Kids[0].Tree.At[NodeAt].X, Kids[0].Tree.At[NodeAt].Y - Kids[0].Anchor);
            Placed.Add(BoxAt(Kids[0].Tree.Nodes[NodeAt], Pos0));
        }
        for (int32 Index = 1; Index < Kids.Num(); ++Index)
        {
            const FChild& Kid = Kids[Index];
            float Line = RelY[Index - 1] + FMath::Max(0.0f, Kid.Target - Kids[Index - 1].Target);
            for (int32 NodeAt = 0; NodeAt < Kid.Tree.Nodes.Num(); ++NodeAt)
            {
                const int32 KidNode = Kid.Tree.Nodes[NodeAt];
                const float MinX = static_cast<float>(Kid.Tree.At[NodeAt].X);
                const float MaxX = MinX + P.Shapes[KidNode].Width;
                const float LocalMinY = static_cast<float>(Kid.Tree.At[NodeAt].Y);
                for (const FBox2D& Earlier : Placed)
                {
                    if (Earlier.Min.X - K.NodePad < MaxX && MinX - K.NodePad < Earlier.Max.X)
                    {
                        Line = FMath::Max(Line, static_cast<float>(Earlier.Max.Y + K.InputGap + Kid.Anchor - LocalMinY));
                    }
                }
            }
            RelY[Index] = Line;
            for (int32 NodeAt = 0; NodeAt < Kid.Tree.Nodes.Num(); ++NodeAt)
            {
                const FVector2D PosK(Kid.Tree.At[NodeAt].X, Line - Kid.Anchor + Kid.Tree.At[NodeAt].Y);
                Placed.Add(BoxAt(Kid.Tree.Nodes[NodeAt], PosK));
            }
        }
        const float Shift = (Kids[0].Target + Kids.Last().Target) * 0.5f - (RelY[0] + RelY.Last()) * 0.5f;
        for (int32 Index = 0; Index < Kids.Num(); ++Index)
        {
            Part.Append(Kids[Index].Tree, FVector2D(0.0, AtY + Shift + RelY[Index] - Kids[Index].Anchor));
        }
    }

    // Lays out the producers of Consumer's data inputs that are still to place (claiming them), Consumer at At.
    void Feeds(int32 Consumer, const FVector2D& At, FPart& Part)
    {
        TMap<int32, float> FixedX;
        ComputeDataX(Consumer, static_cast<float>(At.X), FixedX);
        if (FixedX.Num() > 0)
        {
            FeedsWithX(Consumer, static_cast<float>(At.Y), FixedX, Part);
        }
    }

    static bool Cross(const FBox2D& A, const FBox2D& B)
    {
        return A.Min.X < B.Max.X && B.Min.X < A.Max.X && A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y;
    }

    int32 ObstacleRegion(int32 Index) const
    {
        const int32 Owner = P.ObstacleOwner.IsValidIndex(Index) ? P.ObstacleOwner[Index] : INDEX_NONE;
        return Region.IsValidIndex(Owner) ? Region[Owner] : -2 - Index;
    }

    void AddPlaced(const FBox2D& Box, int32 InRegion)
    {
        PlacedBox.Add(Box);
        PlacedRegion.Add(InRegion);
        if (FBox2D* Existing = RegionBox.Find(InRegion))
        {
            *Existing += Box;
        }
        else
        {
            RegionBox.Add(InRegion, Box);
        }
    }

    // Every placed box again, from Pos (after room was made) and the obstacles.
    void RebuildPlaced()
    {
        PlacedBox.Reset();
        PlacedRegion.Reset();
        RegionBox.Reset();
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (Done[Node] && (!IsFree(Node) || Settled.Contains(Node)))
            {
                AddPlaced(BoxAt(Node, Pos[Node]), Region[Node]);
            }
        }
        for (int32 Index = 0; Index < ObstacleAt.Num(); ++Index)
        {
            AddPlaced(ObstacleAt[Index], ObstacleRegion(Index));
        }
    }

    // How far Part at At (in region InRegion) must move down to touch nothing: its nodes keep NodePad from the nodes
    // of its own region, and its whole extent, swept from SweepX when it slid there, RegionGap from every other region.
    float NeedDown(const FPart& Part, const FVector2D& At, int32 InRegion, float SweepX) const
    {
        float Need = 0.0f;
        FBox2D Extent(ForceInit);
        for (int32 Index = 0; Index < Part.Nodes.Num(); ++Index)
        {
            const FBox2D Box = BoxAt(Part.Nodes[Index], At + Part.At[Index]);
            Extent += Box;
            const FBox2D Grown(Box.Min - FVector2D(K.NodePad), Box.Max + FVector2D(K.NodePad));
            for (int32 Other = 0; Other < PlacedBox.Num(); ++Other)
            {
                if (PlacedRegion[Other] == InRegion && Cross(Grown, PlacedBox[Other]))
                {
                    Need = FMath::Max(Need, static_cast<float>(PlacedBox[Other].Max.Y + K.RowGap - Box.Min.Y));
                }
            }
        }
        if (Extent.bIsValid && InRegion != Floating)
        {
            const FBox2D Grown(Extent.Min - FVector2D(K.RegionGap), Extent.Max + FVector2D(K.RegionGap));
            for (const TPair<int32, FBox2D>& Other : RegionBox)
            {
                if (Other.Key != InRegion && Cross(Grown, Other.Value))
                {
                    Need = FMath::Max(Need, static_cast<float>(Other.Value.Max.Y + K.RegionGap - Extent.Min.Y));
                }
            }
            // A row that slid here: its white line, from where it started, must not run through another region.
            FBox2D Band(ForceInit);
            for (int32 Index = 0; Index < Part.Nodes.Num() && SweepX < TNumericLimits<float>::Max(); ++Index)
            {
                if (IsExecShape(P.Shapes[Part.Nodes[Index]]))
                {
                    Band += BoxAt(Part.Nodes[Index], At + Part.At[Index]);
                }
            }
            if (Band.bIsValid)
            {
                Band.Min.X = FMath::Min(Band.Min.X, static_cast<double>(SweepX));
                for (const TPair<int32, FBox2D>& Other : RegionBox)
                {
                    if (Other.Key != InRegion && Cross(Band, Other.Value))
                    {
                        Need = FMath::Max(Need, 1.0f);
                    }
                }
            }
        }
        return Need;
    }

    // How far Part at At must move down to clear ONLY placed boxes of its own region.
    float NeedDownOwnRegion(const FPart& Part, const FVector2D& At, int32 InRegion) const
    {
        float Need = 0.0f;
        for (int32 Index = 0; Index < Part.Nodes.Num(); ++Index)
        {
            const FBox2D Box = BoxAt(Part.Nodes[Index], At + Part.At[Index]);
            const FBox2D Grown(Box.Min - FVector2D(K.NodePad), Box.Max + FVector2D(K.NodePad));
            for (int32 Other = 0; Other < PlacedBox.Num(); ++Other)
            {
                if (PlacedRegion[Other] == InRegion && Cross(Grown, PlacedBox[Other]))
                {
                    Need = FMath::Max(Need, static_cast<float>(PlacedBox[Other].Max.Y + K.RowGap - Box.Min.Y));
                }
            }
        }
        return Need;
    }

    // How far Part at At must move down to clear unpushable blockers: own-region boxes, unowned obstacles,
    // and other regions whose top lies at or above PushLine.
    float NeedDownFixed(const FPart& Part, const FVector2D& At, int32 InRegion, float PushLine) const
    {
        float Need = NeedDownOwnRegion(Part, At, InRegion);
        FBox2D Extent(ForceInit);
        for (int32 Index = 0; Index < Part.Nodes.Num(); ++Index)
        {
            Extent += BoxAt(Part.Nodes[Index], At + Part.At[Index]);
        }
        if (Extent.bIsValid && InRegion != Floating)
        {
            const FBox2D Grown(Extent.Min - FVector2D(K.RegionGap), Extent.Max + FVector2D(K.RegionGap));
            for (const TPair<int32, FBox2D>& Other : RegionBox)
            {
                if (Other.Key != InRegion && (Other.Key < 0 || Other.Value.Min.Y <= PushLine) && Cross(Grown, Other.Value))
                {
                    Need = FMath::Max(Need, static_cast<float>(Other.Value.Max.Y + K.RegionGap - Extent.Min.Y));
                }
            }
        }
        return Need;
    }

    bool HasPendingFreeInRegion(int32 QueryRegion) const
    {
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (Region[Node] == QueryRegion && IsFree(Node) && !Settled.Contains(Node))
            {
                return true;
            }
        }
        return false;
    }

    bool IsPlaced(int32 Node) const
    {
        return Done[Node] && (!IsFree(Node) || Settled.Contains(Node));
    }

    struct FSnapshot
    {
        TArray<FVector2D> Pos;
        TArray<FBox2D> Obstacles;
        TArray<FRowBreak> Breaks;
    TSet<int32> TerminalRows;
    };
    FSnapshot Save() const
    {
        return FSnapshot{Pos, ObstacleAt, Breaks};
    }
    void Restore(const FSnapshot& Snapshot)
    {
        Pos = Snapshot.Pos;
        ObstacleAt = Snapshot.Obstacles;
        Breaks = Snapshot.Breaks;
        RebuildPlaced();
    }

    bool IsInsideLockedObstacle(int32 Node) const
    {
        if (!IsPlaced(Node))
        {
            return false;
        }
        const FBox2D Box = BoxAt(Node, Pos[Node]);
        const FVector2D Center = Box.GetCenter();
        for (int32 Index = 0; Index < ObstacleAt.Num(); ++Index)
        {
            const int32 Owner = P.ObstacleOwner.IsValidIndex(Index) ? P.ObstacleOwner[Index] : INDEX_NONE;
            if (Owner == INDEX_NONE && ObstacleAt[Index].IsInsideOrOn(Center))
            {
                return true;
            }
        }
        return false;
    }

    // Makes room for Part at Want the way a person does: what lies below Line (the row's white line, or the top of the
    // reader a value block goes beside) and is in the way moves down, so the new nodes keep their line. Every node of
    // the part's own region whose top is below the highest node in the way moves down by one amount, so that region keeps
    // its shape; another region in the way moves down whole, and so does any region the moved nodes would then run into.
    // Nothing moves (false) when something in the way reaches up to the line (a node of the row itself, or a region
    // beside it), or is an obstacle no node owns.
    bool PushRoomDown(const FPart& Part, const FVector2D& Want, int32 InRegion, float Line)
    {
        if (Part.Nodes.Num() == 0)
        {
            return false;
        }
        const TSet<int32> InPart(Part.Nodes);
        TArray<FBox2D> PartBoxes;
        FBox2D Extent(ForceInit);
        for (int32 Index = 0; Index < Part.Nodes.Num(); ++Index)
        {
            PartBoxes.Add(BoxAt(Part.Nodes[Index], Want + Part.At[Index]));
            Extent += PartBoxes.Last();
        }
        // What is in the way, as NeedDown sees it: nodes of the part's own region within NodePad of a part node, and
        // other regions within RegionGap of the part's extent.
        float Cut = TNumericLimits<float>::Max();
        float Down = 0.0f;
        bool bOwnRegion = false;
        TSet<int32> MoveRegions;
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (!IsPlaced(Node) || InPart.Contains(Node) || Region[Node] != InRegion)
            {
                continue;
            }
            const FBox2D Box = BoxAt(Node, Pos[Node]);
            for (const FBox2D& Mine : PartBoxes)
            {
                if (Cross(FBox2D(Mine.Min - FVector2D(K.NodePad), Mine.Max + FVector2D(K.NodePad)), Box))
                {
                    if (Box.Min.Y <= Line)
                    {
                        return false;
                    }
                    bOwnRegion = true;
                    Cut = FMath::Min(Cut, static_cast<float>(Box.Min.Y));
                    Down = FMath::Max(Down, static_cast<float>(Mine.Max.Y + K.LaneGap - Box.Min.Y));
                }
            }
        }
        for (int32 Index = 0; Index < ObstacleAt.Num(); ++Index)
        {
            const int32 Owner = P.ObstacleOwner.IsValidIndex(Index) ? P.ObstacleOwner[Index] : INDEX_NONE;
            if (Owner == INDEX_NONE && Cross(FBox2D(Extent.Min - FVector2D(K.RegionGap), Extent.Max + FVector2D(K.RegionGap)), ObstacleAt[Index]))
            {
                return false;
            }
        }
        const FBox2D Grown(Extent.Min - FVector2D(K.RegionGap), Extent.Max + FVector2D(K.RegionGap));
        for (const TPair<int32, FBox2D>& Other : RegionBox)
        {
            if (Other.Key == InRegion || !Cross(Grown, Other.Value))
            {
                continue;
            }
            if (Other.Value.Min.Y <= Line)
            {
                return false;
            }
            MoveRegions.Add(Other.Key);
            Cut = FMath::Min(Cut, static_cast<float>(Other.Value.Min.Y));
            Down = FMath::Max(Down, static_cast<float>(Extent.Max.Y + K.RegionGap + 1.0f - Other.Value.Min.Y));
        }
        if (Down <= 0.0f)
        {
            return false;
        }
        if (bOwnRegion)
        {
            // Everything of the own region below the cut moves, and every other region lying wholly below the cut that
            // a moved node would come near goes with it (found below).
            for (const TPair<int32, FBox2D>& Other : RegionBox)
            {
                if (Other.Key != InRegion && Other.Value.Min.Y >= Cut && Other.Value.Min.X < Extent.Max.X + K.RegionGap &&
                    Other.Value.Max.X > Extent.Min.X - K.RegionGap)
                {
                    MoveRegions.Add(Other.Key);
                }
            }
        }
        const auto Moves = [&](int32 Node)
        {
            return !InPart.Contains(Node) && IsPlaced(Node) &&
                   ((bOwnRegion && Region[Node] == InRegion && Pos[Node].Y >= Cut) || MoveRegions.Contains(Region[Node]));
        };
        // A moved node that would now come near one that stays drags that one's region along, when it lies wholly
        // below the cut; anything else it would hit means there is no clean way to make room.
        for (int32 Round = 0; Round < 64; ++Round)
        {
            int32 Hit = INDEX_NONE;
            for (int32 Node = 0; Node < Count && Hit == INDEX_NONE; ++Node)
            {
                if (!Moves(Node))
                {
                    continue;
                }
                const FBox2D Before = BoxAt(Node, Pos[Node]);
                const FBox2D After = Before.ShiftBy(FVector2D(0.0f, Down));
                const FBox2D Near(After.Min - FVector2D(K.NodePad), After.Max + FVector2D(K.NodePad));
                const FBox2D WasNear(Before.Min - FVector2D(K.NodePad), Before.Max + FVector2D(K.NodePad));
                const FBox2D NearRegion(After.Min - FVector2D(K.RegionGap), After.Max + FVector2D(K.RegionGap));
                const FBox2D WasNearRegion(Before.Min - FVector2D(K.RegionGap), Before.Max + FVector2D(K.RegionGap));
                for (int32 Other = 0; Other < Count; ++Other)
                {
                    if (Other != Node && IsPlaced(Other) && !InPart.Contains(Other) && !Moves(Other))
                    {
                        const FBox2D Box = BoxAt(Other, Pos[Other]);
                        const bool bNewOverlap = !Cross(Before, Box) && Cross(After, Box);
                        const bool bNewPadHit = Cross(Near, Box) && !Cross(WasNear, Box);
                        const bool bNewRegionHit = Region[Other] != Region[Node] && Cross(NearRegion, Box) && !Cross(WasNearRegion, Box);
                        if (bNewOverlap || bNewPadHit || bNewRegionHit)
                        {
                            Hit = Other;
                            break;
                        }
                    }
                }
            }
            if (Hit == INDEX_NONE)
            {
                break;
            }
            const FBox2D* HitRegion = RegionBox.Find(Region[Hit]);
            if (Region[Hit] == InRegion || !HitRegion || HitRegion->Min.Y < Cut || Round == 63)
            {
                return false;
            }
            MoveRegions.Add(Region[Hit]);
        }
        TSet<int32> Moved;
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (Moves(Node))
            {
                if (IsInsideLockedObstacle(Node))
                {
                    return false;
                }
                Moved.Add(Node);
            }
        }
        for (const int32 Node : Moved)
        {
            Pos[Node].Y += Down;
        }
        for (int32 Index = 0; Index < ObstacleAt.Num(); ++Index)
        {
            const int32 Owner = P.ObstacleOwner.IsValidIndex(Index) ? P.ObstacleOwner[Index] : INDEX_NONE;
            if (Moved.Contains(Owner))
            {
                ObstacleAt[Index] = ObstacleAt[Index].ShiftBy(FVector2D(0.0f, Down));
            }
        }
        for (FRowBreak& Each : Breaks)
        {
            if (Moved.Contains(P.Links[Each.Link].From))
            {
                Each.A.Y += Down;
                Each.B.Y += Down;
            }
        }
        RebuildPlaced();
        return true;
    }

    // Puts Part's local origin at Want. With Line set (a row going on from a node, a value beside its reader, or a
    // branch/row-break belonging to this region), what lies below that line and is in the way moves down (PushRoomDown)
    // so an event stays cohesive instead of jumping below other events. With bSlide a row that goes on from a node may
    // keep its white line level by moving right when a free spot is near enough and no other region lies between.
    // Whatever is still in the way after that moves Part down until it touches nothing (NeedDown).
    FVector2D PlaceBlock(const FPart& Part, FVector2D Want, bool bSlide = false, float Line = NoLine, bool bCohesivePush = false)
    {
        constexpr float SlideStep = 32.0f;
        constexpr float MaxSlide = 1600.0f;
        constexpr float PreferredSlideLimit = 480.0f;
        const int32 InRegion = Part.Nodes.Num() > 0 ? Region[Part.Nodes[0]] : INDEX_NONE;
        const float Left = Part.Nodes.Num() > 0 ? static_cast<float>(Want.X + Bounds(P, Part).Min.X) : static_cast<float>(Want.X);

        auto TryPushDown = [&](const FVector2D& Target, float PushLine) -> bool
        {
            if (!K.bAllowMovingExisting || PushLine <= NoLine || InRegion == Floating)
            {
                return false;
            }
            if (NeedDown(Part, Target, InRegion, TNumericLimits<float>::Max()) <= 0.0f)
            {
                return true;
            }
            const FSnapshot Before = Save();
            if (PushRoomDown(Part, Target, InRegion, PushLine) &&
                NeedDown(Part, Target, InRegion, TNumericLimits<float>::Max()) <= 0.0f)
            {
                return true;
            }
            Restore(Before);
            return false;
        };

        auto TrySlideRight = [&](float SlideLimit, bool bCheckLaneGuard) -> bool
        {
            if (!bSlide)
            {
                return false;
            }
            const FBox2D PartExtent = Bounds(P, Part);
            for (float Slide = 0.0f; Slide <= SlideLimit; Slide += SlideStep)
            {
                const FVector2D Candidate = Want + FVector2D(Slide, 0.0f);
                if (NeedDown(Part, Candidate, InRegion, Left) <= 0.0f)
                {
                    if (bCheckLaneGuard && Slide > 0.0f && PartExtent.bIsValid)
                    {
                        const double CandidateBottom = Candidate.Y + PartExtent.Max.Y;
                        bool bBlocksPendingEvent = false;
                        for (const TPair<int32, FBox2D>& Other : RegionBox)
                        {
                            if (Other.Key != InRegion && Other.Key >= 0 && HasPendingFreeInRegion(Other.Key) &&
                                Other.Value.Min.Y > Line && CandidateBottom + K.RegionGap > Other.Value.Min.Y)
                            {
                                bBlocksPendingEvent = true;
                                break;
                            }
                        }
                        if (bBlocksPendingEvent)
                        {
                            return false;
                        }
                    }
                    Want.X += Slide;
                    return true;
                }
            }
            return false;
        };

        bool bPlaced = NeedDown(Part, Want, InRegion, TNumericLimits<float>::Max()) <= 0.0f;
        if (!bPlaced && !bCohesivePush)
        {
            if (K.bPreferSlideBeforePush && bSlide)
            {
                bPlaced = TrySlideRight(PreferredSlideLimit, true) || TryPushDown(Want, Line) || TrySlideRight(MaxSlide, false);
            }
            else
            {
                bPlaced = TryPushDown(Want, Line) || TrySlideRight(MaxSlide, false);
            }
        }
        if (!bPlaced && K.bAllowMovingExisting && Line > NoLine && InRegion != Floating)
        {
            FVector2D Cohesive = Want;
            for (int32 Step = 0; Step < 400; ++Step)
            {
                const float FixedDown = NeedDownFixed(Part, Cohesive, InRegion, Line);
                if (FixedDown <= 0.0f)
                {
                    break;
                }
                Cohesive.Y += FixedDown;
            }
            if (TryPushDown(Cohesive, Line))
            {
                Want = Cohesive;
                bPlaced = true;
            }
        }
        if (!bPlaced)
        {
            for (int32 Step = 0; Step < 400; ++Step)
            {
                const float Down = NeedDown(Part, Want, InRegion, TNumericLimits<float>::Max());
                if (Down <= 0.0f)
                {
                    break;
                }
                Want.Y += Down;
            }
        }
        for (int32 Index = 0; Index < Part.Nodes.Num(); ++Index)
        {
            const int32 Node = Part.Nodes[Index];
            Pos[Node] = Want + Part.At[Index];
            Done[Node] = true;
            Settled.Add(Node);
            if (InRegion == Floating)
            {
                FloatingNodes.Add(Node);
            }
            AddPlaced(BoxAt(Node, Pos[Node]), InRegion);
        }
        return Want;
    }

    // A new chain put between two nodes that stay (A -> new -> B): everything of A's region right of A moves right
    // as far as the chain needs, so B follows it; nothing else moves.
    bool MakeRoom(const FStart& Start, const FPart& Part, const TArray<int32>& Row, const FVector2D& Want)
    {
        if (!K.bAllowMovingExisting || Start.Link == INDEX_NONE || Start.bSide || Start.bBreak || Row.Num() == 0)
        {
            return false;
        }
        const int32 From = P.Links[Start.Link].From;
        const float FromLineY = static_cast<float>(Pos[From].Y + ExecLine(P.Shapes[From]));
        int32 Next = INDEX_NONE;
        for (const int32 Link : Out[Row.Last()])
        {
            const FLink& Each = P.Links[Link];
            if (!Each.bExec || IsFree(Each.To) || Region[Each.To] != Region[From] || Pos[Each.To].X <= Pos[From].X)
            {
                continue;
            }
            // Only treat as an inline same-row insert if Next is roughly on the same row, not inside a locked
            // comment box, and not an existing multi-entry merge point (EW-007).
            const float NextLineY = static_cast<float>(Pos[Each.To].Y + ExecLine(P.Shapes[Each.To]));
            if (FMath::Abs(NextLineY - FromLineY) > 160.0f || IsInsideLockedObstacle(Each.To))
            {
                continue;
            }
            bool bMultiEntryMerge = false;
            for (const int32 InLink : In[Each.To])
            {
                const FLink& InWire = P.Links[InLink];
                if (InWire.bExec && InWire.From != Row.Last() && !Part.Nodes.Contains(InWire.From))
                {
                    bMultiEntryMerge = true;
                    break;
                }
            }
            if (bMultiEntryMerge)
            {
                continue;
            }
            Next = Each.To;
            break;
        }
        if (Next == INDEX_NONE)
        {
            return false;
        }
        const float Threshold = Pos[From].X + Width(From);
        const float Need = Want.X + Bounds(P, Part).Max.X + K.ExecGap - Pos[Next].X;
        if (Need > 0.0f)
        {
            // B and what runs after it (every exec path on from B, right of A): a row of another exec output of an
            // earlier node is no part of the insert and stays where it is.
            TSet<int32> Shift;
            TArray<int32> Open = {Next};
            while (Open.Num() > 0)
            {
                const int32 Node = Open.Pop(EAllowShrinking::No);
                if (Shift.Contains(Node) || !IsPlaced(Node) || Region[Node] != Region[From] || Pos[Node].X < Threshold)
                {
                    continue;
                }
                Shift.Add(Node);
                for (const int32 Link : Out[Node])
                {
                    if (P.Links[Link].bExec)
                    {
                        Open.Add(P.Links[Link].To);
                    }
                }
            }
            // A value right of A that only nodes moving read moves with them, so its wires do not stretch.
            const auto AddValues = [this, &Shift, From]()
            {
                for (bool bGrew = true; bGrew;)
                {
                    bGrew = false;
                    for (int32 Node = 0; Node < Count; ++Node)
                    {
                        if (Shift.Contains(Node) || !Done[Node] || IsExecShape(P.Shapes[Node]) || Region[Node] != Region[From] ||
                            Pos[Node].X <= Pos[From].X || Out[Node].Num() == 0)
                        {
                            continue;
                        }
                        if (!Out[Node].ContainsByPredicate([this, &Shift](int32 Link) { return !Shift.Contains(P.Links[Link].To); }))
                        {
                            Shift.Add(Node);
                            bGrew = true;
                        }
                    }
                }
            };
            AddValues();
            // A node that stays and that the moved ones would now run into: fall back to moving all of A's region right
            // of A, as before, so nothing ends up on top of anything.
            bool bClash = false;
            for (const int32 Node : Shift)
            {
                const FBox2D Was = BoxAt(Node, Pos[Node]);
                const FBox2D WasNear(Was.Min - FVector2D(K.NodePad), Was.Max + FVector2D(K.NodePad));
                const FBox2D Near = WasNear.ShiftBy(FVector2D(Need, 0.0f));
                for (int32 Other = 0; Other < Count && !bClash; ++Other)
                {
                    if (Shift.Contains(Other) || !IsPlaced(Other) || Region[Other] != Region[From])
                    {
                        continue;
                    }
                    const FBox2D Box = BoxAt(Other, Pos[Other]);
                    bClash = Cross(Near, Box) && !Cross(WasNear, Box);
                }
            }
            if (bClash)
            {
                Shift.Reset();
                for (int32 Node = 0; Node < Count; ++Node)
                {
                    if (Done[Node] && Region[Node] == Region[From] && Pos[Node].X >= Threshold)
                    {
                        Shift.Add(Node);
                    }
                }
                AddValues();
            }
            for (const int32 Node : Shift)
            {
                if (IsInsideLockedObstacle(Node))
                {
                    return false;
                }
            }
            for (const int32 Node : Shift)
            {
                Pos[Node].X += Need;
            }
            for (int32 Index = 0; Index < ObstacleAt.Num(); ++Index)
            {
                const int32 Owner = P.ObstacleOwner.IsValidIndex(Index) ? P.ObstacleOwner[Index] : INDEX_NONE;
                if (Shift.Contains(Owner))
                {
                    ObstacleAt[Index] = ObstacleAt[Index].ShiftBy(FVector2D(Need, 0.0f));
                }
            }
            RebuildPlaced();
        }
        return true;
    }

    void Drain()
    {
        while (Queue.Num() > 0)
        {
            const FStart Start = Queue[0];
            Queue.RemoveAt(0);
            if (!Done[Start.Node])
            {
                if (Start.Link == INDEX_NONE && Floating == INDEX_NONE && !RegionBox.Contains(Region[Start.Node]))
                {
                    Floating = Region[Start.Node];
                    FloatingNodes.Reset();
                    FloatingBreaks = Breaks.Num();
                }
                PlaceRow(Start);
            }
            if (Floating != INDEX_NONE && (Queue.Num() == 0 || Region[Queue[0].Node] != Floating))
            {
                SettleFloating();
            }
        }
    }

    // The new event's region, laid out among itself, moves down as one until it keeps RegionGap from every other.
    void SettleFloating()
    {
        FBox2D Box(ForceInit);
        for (const int32 Node : FloatingNodes)
        {
            Box += BoxAt(Node, Pos[Node]);
        }
        float Down = 0.0f;
        for (int32 Step = 0; Box.bIsValid && Step < 400; ++Step)
        {
            const FBox2D Moved = Box.ShiftBy(FVector2D(0.0f, Down));
            const FBox2D Grown(Moved.Min - FVector2D(K.RegionGap), Moved.Max + FVector2D(K.RegionGap));
            float Need = 0.0f;
            for (const TPair<int32, FBox2D>& Other : RegionBox)
            {
                if (Other.Key != Floating && Cross(Grown, Other.Value))
                {
                    Need = FMath::Max(Need, static_cast<float>(Other.Value.Max.Y + K.RegionGap - Moved.Min.Y));
                }
            }
            if (Need <= 0.0f)
            {
                break;
            }
            Down += Need;
        }
        for (const int32 Node : FloatingNodes)
        {
            Pos[Node].Y += Down;
        }
        for (int32 Index = FloatingBreaks; Index < Breaks.Num(); ++Index)
        {
            Breaks[Index].A.Y += Down;
            Breaks[Index].B.Y += Down;
        }
        if (Box.bIsValid)
        {
            NextRootY = FMath::Max(NextRootY, static_cast<float>(Box.Max.Y + Down + K.RowGap));
        }
        Floating = INDEX_NONE;
        FloatingNodes.Reset();
        RebuildPlaced();
    }

    // One row from Start.Node along the first exec output of each node, with the data each node reads.
    void PlaceRow(const FStart& Start)
    {
        FPart Part;
        TArray<int32> Row;
        TArray<FStart> Branches;
        FStart Break;
        float Band = 0.0f;
        float PrevRight = 0.0f;
        TArray<FBox2D> Local;
        int32 Node = Start.Node;
        int32 InLink = Start.Link;
        while (Node != INDEX_NONE)
        {
            const FShape& Shape = P.Shapes[Node];
            if (Row.Num() > 0 && PrevRight + K.ExecGap + Shape.Width > K.MaxRowWidth)
            {
                Break = FStart{Node, InLink, true};
                break;
            }
            float Top = -ExecLine(Shape);
            if (Row.Num() > 0 && InLink != INDEX_NONE)
            {
                const FLink& Incoming = P.Links[InLink];
                const FShape& Source = P.Shapes[Incoming.From];
                const float Output = Source.ExecOut.IsValidIndex(Incoming.FromPin) ? Source.ExecOut[Incoming.FromPin] : ExecLine(Source);
                Top += static_cast<float>(Part.At[Part.Nodes.Find(Incoming.From)].Y) + Output;
            }
            Done[Node] = true;
            Band = FMath::Max(Band, Top + Shape.Height);
            FPart Data;
            Feeds(Node, FVector2D::ZeroVector, Data);
            // Data of one column stays level with the pins it feeds; deeper data goes under the row.
            bool bOneColumn = true;
            float DataTop = TNumericLimits<float>::Max();
            for (int32 Index = 0; Index < Data.Nodes.Num(); ++Index)
            {
                bOneColumn &= FMath::IsNearlyEqual(static_cast<float>(Data.At[Index].X) + Width(Data.Nodes[Index]), -K.DataGap, 1.0f);
                DataTop = FMath::Min(DataTop, static_cast<float>(Top + Data.At[Index].Y));
            }
            // One column stays level with its pins, but never under the white wire: its top keeps LineClear below it.
            constexpr float LineClear = 16.0f;
            const float DataDown = Data.Nodes.Num() == 0 ? 0.0f
                                   : bOneColumn ? FMath::Max(0.0f, LineClear - DataTop)
                                                : FMath::Max(0.0f, Band + K.LaneGap - DataTop);
            // Right of the node before; its data clear of every box already in the row that it is level with.
            float X = Row.Num() == 0 ? 0.0f : PrevRight + K.ExecGap;
            for (int32 Index = 0; Index < Data.Nodes.Num(); ++Index)
            {
                const FBox2D Box = BoxAt(Data.Nodes[Index], FVector2D(Data.At[Index].X, Top + DataDown + Data.At[Index].Y));
                for (const FBox2D& Other : Local)
                {
                    if (Box.Min.Y < Other.Max.Y + K.InputGap && Other.Min.Y < Box.Max.Y + K.InputGap)
                    {
                        X = FMath::Max(X, static_cast<float>(Other.Max.X + K.DataGap - Box.Min.X));
                    }
                }
            }
            // Data computed from an exec node's output (loop Index, spawned Actor, ...) must follow that
            // producer. Deep trees otherwise fit beneath the preceding column and send the fresh value leftwards.
            for (int32 DataIndex = 0; DataIndex < Data.Nodes.Num(); ++DataIndex)
            {
                for (const int32 Link : In[Data.Nodes[DataIndex]])
                {
                    const FLink& Incoming = P.Links[Link];
                    const int32 Producer = Part.Nodes.Find(Incoming.From);
                    if (!Incoming.bExec && Producer != INDEX_NONE && IsExecShape(P.Shapes[Incoming.From]))
                    {
                        X = FMath::Max(X, static_cast<float>(Part.At[Producer].X + P.Shapes[Incoming.From].Width +
                            K.DataGap - Data.At[DataIndex].X));
                    }
                }
            }
            // Keep a row that begins at an entry/event from sending its pure data branches behind the entry's left edge.
            if (Row.Num() > 0 && P.Shapes[Start.Node].ExecIn < 0.0f)
            {
                for (int32 DataIndex = 0; DataIndex < Data.Nodes.Num(); ++DataIndex)
                {
                    X = FMath::Max(X, -static_cast<float>(Data.At[DataIndex].X));
                }
            }
            if (Row.Num() > 0 && X + Shape.Width > K.MaxRowWidth)
            {
                Done[Node] = false;
                for (int32 Value : Data.Nodes)
                {
                    Done[Value] = false;
                }
                Break = FStart{Node, InLink, true};
                break;
            }
            Part.Add(Node, FVector2D(X, Top));
            Local.Add(BoxAt(Node, FVector2D(X, Top)));
            for (int32 Index = 0; Index < Data.Nodes.Num(); ++Index)
            {
                const FVector2D At(X + Data.At[Index].X, Top + DataDown + Data.At[Index].Y);
                Part.Add(Data.Nodes[Index], At);
                Local.Add(BoxAt(Data.Nodes[Index], At));
            }
            Row.Add(Node);
            PrevRight = X + Shape.Width;
            // The first exec output still to place goes on in this row; the others start rows of their own.
            // If an earlier exec output already goes to a fixed/done node (e.g. inserting a Branch whose True pin
            // continues into an existing fixed node while False goes to a new node), do not hijack False onto the
            // primary row: end this row here so MakeRoom shifts the fixed continuation right and False drops below.
            int32 Next = INDEX_NONE;
            int32 NextLink = INDEX_NONE;
            bool bPrimaryExecTaken = false;
            for (const int32 Link : Out[Node])
            {
                const FLink& Each = P.Links[Link];
                if (!Each.bExec || !IsExecShape(P.Shapes[Each.To]))
                {
                    continue;
                }
                if (Done[Each.To] || !IsFree(Each.To))
                {
                    if (Next == INDEX_NONE)
                    {
                        bPrimaryExecTaken = true;
                    }
                    continue;
                }
                if (Next == INDEX_NONE && !bPrimaryExecTaken)
                {
                    Next = Each.To;
                    NextLink = Link;
                }
                else
                {
                    Branches.Add(FStart{Each.To, Link, false, true});
                }
            }
            Node = Next;
            InLink = NextLink;
        }
        Branches.RemoveAll([&Row](const FStart& Each) { return Row.Contains(Each.Node); });

        FVector2D Want(P.Origin.X, NextRootY);
        if (Start.Link == INDEX_NONE)
        {
            // Delegate callback affinity (EW-005): place a floating CustomEvent near its BindEvent partner
            // (and balanced against any fixed downstream exec merge target) instead of dumping it at Origin.
            int32 BindPartner = INDEX_NONE;
            for (const int32 Link : Out[Start.Node])
            {
                const FLink& Each = P.Links[Link];
                if (Each.bDelegate && IsPlaced(Each.To))
                {
                    BindPartner = Each.To;
                    break;
                }
            }
            if (BindPartner == INDEX_NONE)
            {
                for (const int32 Link : In[Start.Node])
                {
                    const FLink& Each = P.Links[Link];
                    if (Each.bDelegate && IsPlaced(Each.From))
                    {
                        BindPartner = Each.From;
                        break;
                    }
                }
            }
            if (BindPartner != INDEX_NONE)
            {
                double AnchorX = Pos[BindPartner].X;
                double AnchorY = Pos[BindPartner].Y + P.Shapes[BindPartner].Height + K.RegionGap;
                // If this callback row directly feeds an already-placed merge node, keep its right edge
                // strictly left of that merge target so the merge wire stays forward (never overshoots into a U-turn).
                if (Row.Num() > 0)
                {
                    for (const int32 Link : Out[Row.Last()])
                    {
                        const FLink& Each = P.Links[Link];
                        if (Each.bExec && IsPlaced(Each.To) && !Part.Nodes.Contains(Each.To))
                        {
                            const FBox2D PartBounds = Bounds(P, Part);
                            const double ChainLeftOffset = PartBounds.bIsValid ? FMath::Min(0.0, PartBounds.Min.X) : 0.0;
                            const double MinWantX = P.Origin.X - ChainLeftOffset;
                            const double IdealLeftOfTarget = Pos[Each.To].X - PartBounds.Max.X - K.ExecGap;
                            AnchorX = FMath::Max(MinWantX, FMath::Min(AnchorX, IdealLeftOfTarget));
                            break;
                        }
                    }
                }
                Want = FVector2D(AnchorX, AnchorY);
            }
            else if (Row.Num() > 0)
            {
                // Stage 2.1 (EW-002 / EW-005): if a secondary entry chain flows into an already-placed
                // shared merge node M, align it immediately left of M on the first free row below M
                // (clamped to LeftX = P.Origin.X). Fall back to flush-left if pushed > 2 row heights below M.
                int32 MergeTarget = INDEX_NONE;
                for (const int32 Link : Out[Row.Last()])
                {
                    const FLink& Each = P.Links[Link];
                    if (Each.bExec && IsPlaced(Each.To) && !Part.Nodes.Contains(Each.To))
                    {
                        MergeTarget = Each.To;
                        break;
                    }
                }
                if (MergeTarget != INDEX_NONE)
                {
                    const FBox2D PartBounds = Bounds(P, Part);
                    const double ChainRight = PartBounds.bIsValid ? PartBounds.Max.X : static_cast<double>(PrevRight);
                    const double ChainLeftOffset = PartBounds.bIsValid ? FMath::Min(0.0, PartBounds.Min.X) : 0.0;
                    const double MinWantX = P.Origin.X - ChainLeftOffset;
                    const double TargetX = FMath::Max(MinWantX, Pos[MergeTarget].X - K.ExecGap - ChainRight);
                    const double TargetY = Pos[MergeTarget].Y + P.Shapes[MergeTarget].Height + K.RowGap -
                                           (PartBounds.bIsValid ? PartBounds.Min.Y : 0.0);
                    FVector2D Candidate(TargetX, TargetY);
                    const int32 InRegion = Region[Start.Node];
                    for (int32 Step = 0; Step < 400; ++Step)
                    {
                        const float Down = NeedDown(Part, Candidate, InRegion, TNumericLimits<float>::Max());
                        if (Down <= 0.0f)
                        {
                            break;
                        }
                        Candidate.Y += Down;
                    }
                    const double RowHeight = FMath::Max(static_cast<double>(P.Shapes[MergeTarget].Height),
                                                        PartBounds.bIsValid ? PartBounds.GetSize().Y : 120.0) + K.RowGap;
                    if (Candidate.Y <= Pos[MergeTarget].Y + 2.0 * RowHeight)
                    {
                        Want = Candidate;
                    }
                }
            }
        }
        else if (Start.Link != INDEX_NONE)
        {
            const FLink& From = P.Links[Start.Link];
            const FShape& Source = P.Shapes[From.From];
            const float Line = Pos[From.From].Y + (Source.ExecOut.IsValidIndex(From.FromPin) ? Source.ExecOut[From.FromPin] : ExecLine(Source));
            const float* Left = Start.bBreak ? RowLeftOfBreak.Find(Start.Link) : nullptr;
            Want = Left ? FVector2D(*Left, Line) : FVector2D(Pos[From.From].X + Source.Width + K.ExecGap, Line);
        }
        // A source in a fixed node or an earlier branch is not in this row's local Part. Constrain
        // against its world position too. An explicit row break is allowed to wrap back to the left.
        if (!Start.bBreak)
        {
            for (int32 PartIndex = 0; PartIndex < Part.Nodes.Num(); ++PartIndex)
            {
                const int32 DataNode = Part.Nodes[PartIndex];
                if (IsExecShape(P.Shapes[DataNode])) { continue; }
                for (const int32 Link : In[DataNode])
                {
                    const FLink& Incoming = P.Links[Link];
                    if (!Incoming.bExec && Done[Incoming.From] && !Part.Nodes.Contains(Incoming.From) &&
                        IsExecShape(P.Shapes[Incoming.From]))
                    {
                        Want.X = FMath::Max(Want.X, Pos[Incoming.From].X + P.Shapes[Incoming.From].Width +
                            K.DataGap - Part.At[PartIndex].X);
                    }
                }
            }
            // A row continuing directly from a fixed entry/event keeps its pure data branches from overhanging
            // to the left of the entry's left edge.
            if (Start.Link != INDEX_NONE && !Start.bSide)
            {
                const int32 EntryNode = P.Links[Start.Link].From;
                if (P.Shapes[EntryNode].ExecIn < 0.0f)
                {
                    const FBox2D PartBounds = Bounds(P, Part);
                    if (PartBounds.bIsValid)
                    {
                        Want.X = FMath::Max(Want.X, static_cast<float>(Pos[EntryNode].X) - static_cast<float>(PartBounds.Min.X));
                    }
                }
            }
        }
        // A function result reached from a loop's Completed (or an early exit sharing that result) belongs
        // after the already placed execution body. Placing it beside the loop puts its terminal back inside it.
        const FShape& FirstShape = P.Shapes[Start.Node];
        if (Start.bSide && FirstShape.ExecIn >= 0.0f && FirstShape.ExecOut.Num() == 0)
        {
            TerminalRows.Add(Start.Node);
            for (int32 PlacedNode = 0; PlacedNode < Count; ++PlacedNode)
            {
                if (Done[PlacedNode] && !Part.Nodes.Contains(PlacedNode) && Region[PlacedNode] == Region[Start.Node])
                {
                    Want.X = FMath::Max(Want.X, Pos[PlacedNode].X + P.Shapes[PlacedNode].Width + K.ExecGap);
                    Want.Y = FMath::Max(Want.Y, Pos[PlacedNode].Y + P.Shapes[PlacedNode].Height + K.RowGap + FirstShape.ExecIn);
                }
            }
        }
        const bool bInsert = MakeRoom(Start, Part, Row, Want);
        const bool bKeepLine = Start.Link != INDEX_NONE && !Start.bSide && !Start.bBreak;
        float PushLine = NoLine;
        if (bKeepLine)
        {
            PushLine = static_cast<float>(Want.Y);
        }
        else if (Start.Link != INDEX_NONE)
        {
            const int32 FromNode = P.Links[Start.Link].From;
            PushLine = static_cast<float>(Pos[FromNode].Y + ExecLine(P.Shapes[FromNode]));
        }
        const FVector2D At = PlaceBlock(Part, Want, !bInsert && bKeepLine, PushLine, !bKeepLine && Start.Link != INDEX_NONE);
        if (Start.Link == INDEX_NONE)
        {
            NextRootY = FMath::Max(NextRootY, static_cast<float>(Bounds(P, Part).Max.Y + At.Y + K.RowGap));
        }
        if (Start.bBreak)
        {
            AddBreak(Start, Row);
        }
        if (Break.Node != INDEX_NONE)
        {
            RowOfBreak.Add(Break.Link, Part.Nodes);
            RowLeftOfBreak.Add(Break.Link, Pos[Row[0]].X);
            Branches.Insert(Break, 0);
        }
        Queue.Insert(Branches, 0);
    }

    // The two reroutes of a break: right of the row it leaves, then left of the row it enters, both in the gap between.
    void AddBreak(const FStart& Start, const TArray<int32>& Row)
    {
        const TArray<int32>* Above = RowOfBreak.Find(Start.Link);
        if (!Above || Row.Num() == 0)
        {
            return;
        }
        float Bottom = -TNumericLimits<float>::Max();
        for (const int32 Node : *Above)
        {
            Bottom = FMath::Max(Bottom, static_cast<float>(Pos[Node].Y + P.Shapes[Node].Height));
        }
        float Top = TNumericLimits<float>::Max();
        for (const int32 Node : Row)
        {
            Top = FMath::Min(Top, static_cast<float>(Pos[Node].Y));
        }
        const FLink& Wire = P.Links[Start.Link];
        FRowBreak Each;
        Each.Link = Start.Link;
        const float Mid = Top > Bottom ? (Bottom + Top) * 0.5f : Top - K.NodePad;
        Each.A = FVector2D(Pos[Wire.From].X + P.Shapes[Wire.From].Width + K.DataGap, Mid);
        Each.B = FVector2D(Pos[Start.Node].X - K.DataGap, Mid);
        Breaks.Add(Each);
    }

    // A pure node no free node reads: its tree beside the placed node that reads it, else at the origin.
    void PlaceValue(int32 Node)
    {
        FPart Part;
        Done[Node] = true;
        Part.Add(Node, FVector2D::ZeroVector);
        Feeds(Node, FVector2D::ZeroVector, Part);
        FVector2D Want(P.Origin.X, NextRootY);
        bool bBeside = false;
        float Line = NoLine;
        for (const int32 Link : Out[Node])
        {
            const FLink& Each = P.Links[Link];
            const FShape& Reader = P.Shapes[Each.To];
            if (!Each.bExec && Each.To != Node && Done[Each.To] && !Part.Nodes.Contains(Each.To) && Reader.DataIn.IsValidIndex(Each.ToPin) &&
                P.Shapes[Node].DataOut.IsValidIndex(Each.FromPin))
            {
                Want = FVector2D(Pos[Each.To].X - K.DataGap - Width(Node), Pos[Each.To].Y + Reader.DataIn[Each.ToPin] - P.Shapes[Node].DataOut[Each.FromPin]);
                bBeside = true;
                Line = static_cast<float>(Pos[Each.To].Y);
                break;
            }
        }
        const FVector2D At = PlaceBlock(Part, Want, false, Line);
        if (!bBeside)
        {
            NextRootY = FMath::Max(NextRootY, static_cast<float>(Bounds(P, Part).Max.Y + At.Y + K.RowGap));
        }
    }

    // Route leftward / looping exec wires (e.g. Delay re-fire back to an earlier Branch) through a clear
    // outer horizontal corridor below all intervening nodes (including pure data sub-rows) and comment obstacles,
    // staggering parallel return lanes in the same region from right to left by +32 (EW-002).
    void RouteBackwardExecEdges()
    {
        TSet<int32> AlreadyRouted;
        for (const FRowBreak& Existing : Breaks)
        {
            AlreadyRouted.Add(Existing.Link);
        }
        TArray<FBox2D> KnotBoxes;
        for (const FRowBreak& Existing : Breaks)
        {
            for (const FVector2D& Pt : {Existing.A, Existing.B})
            {
                const FVector2D At(FMath::RoundToInt(Pt.X - 16.0), FMath::RoundToInt(Pt.Y - 12.0));
                KnotBoxes.Emplace(At, At + FVector2D(32.0, 24.0));
            }
        }
        auto IsKnotPointFree = [&](const FVector2D& Centre) -> bool
        {
            const FVector2D At(FMath::RoundToInt(Centre.X - 16.0), FMath::RoundToInt(Centre.Y - 12.0));
            const FBox2D Candidate(At - FVector2D(4.0, 4.0), At + FVector2D(36.0, 28.0));
            for (int32 Node = 0; Node < Count; ++Node)
            {
                if (IsPlaced(Node) && Cross(Candidate, BoxAt(Node, Pos[Node])))
                {
                    return false;
                }
            }
            for (const FBox2D& Obstacle : ObstacleAt)
            {
                if (Obstacle.bIsValid && Cross(Candidate, Obstacle))
                {
                    return false;
                }
            }
            for (const FBox2D& ExistingKnot : KnotBoxes)
            {
                if (Cross(Candidate, ExistingKnot))
                {
                    return false;
                }
            }
            return true;
        };

        TArray<int32> Candidates;
        for (int32 Link = 0; Link < P.Links.Num(); ++Link)
        {
            const FLink& Each = P.Links[Link];
            if (!Each.bExec || AlreadyRouted.Contains(Link) || !IsPlaced(Each.From) || !IsPlaced(Each.To))
            {
                continue;
            }
            if (!IsFree(Each.From) && !IsFree(Each.To))
            {
                continue;
            }
            const double FromRight = Pos[Each.From].X + P.Shapes[Each.From].Width;
            const double ToLeft = Pos[Each.To].X;
            if (ToLeft >= FromRight - 16.0)
            {
                continue;
            }
            Candidates.Add(Link);
        }
        // Order per region by source X right-to-left so multiple back-edges stagger cleanly without crossing.
        Candidates.Sort([this](int32 A, int32 B)
        {
            const FLink& LA = P.Links[A];
            const FLink& LB = P.Links[B];
            if (Region[LA.From] != Region[LB.From])
            {
                return Region[LA.From] < Region[LB.From];
            }
            const double RightA = Pos[LA.From].X + P.Shapes[LA.From].Width;
            const double RightB = Pos[LB.From].X + P.Shapes[LB.From].Width;
            if (!FMath::IsNearlyEqual(RightA, RightB, 0.5))
            {
                return RightA > RightB;
            }
            return A < B;
        });

        TMap<int32, int32> RegionStagger;
        for (const int32 Link : Candidates)
        {
            const FLink& Each = P.Links[Link];
            const FShape& FromShape = P.Shapes[Each.From];
            const FShape& ToShape = P.Shapes[Each.To];
            const double FromRight = Pos[Each.From].X + FromShape.Width;
            const double ToLeft = Pos[Each.To].X;
            const double HorizontalBackSpan = FromRight - ToLeft;
            const double ExitX = FromRight + FMath::Max(32.0f, K.DataGap * 0.75f);
            const double EnterX = ToLeft - FMath::Max(32.0f, K.DataGap * 0.75f);
            const double MinX = FMath::Min(EnterX - 24.0, FMath::Min(ToLeft, Pos[Each.From].X));
            const double MaxX = FMath::Max(ExitX + 24.0, FMath::Max(FromRight, Pos[Each.To].X + ToShape.Width));
            const double MinWireY = FMath::Min(Pos[Each.From].Y, Pos[Each.To].Y) - 32.0;
            double BandBottom = FMath::Max(Pos[Each.From].Y + FromShape.Height, Pos[Each.To].Y + ToShape.Height);
            bool bCrossesIntervening = false;

            // 1. Cover all placed nodes in the same region(s) across [MinX, MaxX], including pure data sub-rows.
            for (int32 Other = 0; Other < Count; ++Other)
            {
                if (!IsPlaced(Other))
                {
                    continue;
                }
                if (Region[Other] != Region[Each.From] && Region[Other] != Region[Each.To])
                {
                    continue;
                }
                const FBox2D OtherBox = BoxAt(Other, Pos[Other]);
                if (OtherBox.Max.X >= MinX && OtherBox.Min.X <= MaxX && OtherBox.Max.Y >= MinWireY)
                {
                    BandBottom = FMath::Max(BandBottom, OtherBox.Max.Y);
                    if (Other != Each.From && Other != Each.To)
                    {
                        bCrossesIntervening = true;
                    }
                }
            }

            // 2. Iteratively expand BandBottom past any intervening placed node (any region) or comment obstacle
            //    whose full measured box overlaps [MinX, MaxX] and reaches into the corridor band.
            for (int32 Pass = 0; Pass < 16; ++Pass)
            {
                bool bExpanded = false;
                const double ReachY = BandBottom + K.RegionGap * 0.5;
                for (int32 Other = 0; Other < Count; ++Other)
                {
                    if (!IsPlaced(Other))
                    {
                        continue;
                    }
                    const FBox2D OtherBox = BoxAt(Other, Pos[Other]);
                    if (OtherBox.Max.X >= MinX && OtherBox.Min.X <= MaxX &&
                        OtherBox.Max.Y >= MinWireY && OtherBox.Min.Y <= ReachY)
                    {
                        if (Other != Each.From && Other != Each.To)
                        {
                            bCrossesIntervening = true;
                        }
                        if (OtherBox.Max.Y > BandBottom + 0.5)
                        {
                            BandBottom = OtherBox.Max.Y;
                            bExpanded = true;
                        }
                    }
                }
                for (const FBox2D& Obstacle : ObstacleAt)
                {
                    if (!Obstacle.bIsValid)
                    {
                        continue;
                    }
                    if (Obstacle.Max.X >= MinX && Obstacle.Min.X <= MaxX &&
                        Obstacle.Max.Y >= MinWireY && Obstacle.Min.Y <= ReachY)
                    {
                        bCrossesIntervening = true;
                        if (Obstacle.Max.Y > BandBottom + 0.5)
                        {
                            BandBottom = Obstacle.Max.Y;
                            bExpanded = true;
                        }
                    }
                }
                if (!bExpanded)
                {
                    break;
                }
            }

            if (HorizontalBackSpan < 240.0 && !bCrossesIntervening)
            {
                continue;
            }

            int32& Stagger = RegionStagger.FindOrAdd(Region[Each.From]);
            double CorridorY = BandBottom + K.RegionGap * 0.5 + Stagger * 32.0;
            bool bFoundClear = false;
            for (int32 Try = 0; Try < 48; ++Try)
            {
                const FVector2D CandA(ExitX, CorridorY);
                const FVector2D CandB(EnterX, CorridorY);
                if (IsKnotPointFree(CandA) && IsKnotPointFree(CandB))
                {
                    // Ensure the horizontal corridor segment itself clears all placed nodes and comment obstacles.
                    const FBox2D CorridorBar(FVector2D(FMath::Min(ExitX, EnterX), CorridorY - 14.0),
                                             FVector2D(FMath::Max(ExitX, EnterX), CorridorY + 14.0));
                    bool bBarClear = true;
                    for (int32 Other = 0; Other < Count; ++Other)
                    {
                        if (IsPlaced(Other) && Cross(CorridorBar, BoxAt(Other, Pos[Other])))
                        {
                            CorridorY = FMath::Max(CorridorY + 32.0, BoxAt(Other, Pos[Other]).Max.Y + 32.0);
                            bBarClear = false;
                            break;
                        }
                    }
                    if (bBarClear)
                    {
                        for (const FBox2D& Obstacle : ObstacleAt)
                        {
                            if (Obstacle.bIsValid && Cross(CorridorBar, Obstacle))
                            {
                                CorridorY = FMath::Max(CorridorY + 32.0, Obstacle.Max.Y + 32.0);
                                bBarClear = false;
                                break;
                            }
                        }
                    }
                    if (bBarClear)
                    {
                        FRowBreak Route;
                        Route.Link = Link;
                        Route.A = CandA;
                        Route.B = CandB;
                        Breaks.Add(Route);
                        const FVector2D BoxA(FMath::RoundToInt(CandA.X - 16.0), FMath::RoundToInt(CandA.Y - 12.0));
                        const FVector2D BoxB(FMath::RoundToInt(CandB.X - 16.0), FMath::RoundToInt(CandB.Y - 12.0));
                        KnotBoxes.Emplace(BoxA, BoxA + FVector2D(32.0, 24.0));
                        KnotBoxes.Emplace(BoxB, BoxB + FVector2D(32.0, 24.0));
                        ++Stagger;
                        bFoundClear = true;
                        break;
                    }
                }
                else
                {
                    CorridorY += 32.0;
                }
            }
            (void)bFoundClear;
        }
    }
};
} // namespace RuledSteps

FSolution Solve(const FProblem& Problem, const FParams& Params)
{
    RuledSteps::FSolver Solver(Problem, Params);
    return Solver.Run();
}
} // namespace McpRuled
