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
        // Regions: the nodes wired together, one event's or function's logic each.
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
        for (const FLink& Each : P.Links)
        {
            if (Region.IsValidIndex(Each.From) && Region.IsValidIndex(Each.To))
            {
                Region[Root(Each.From)] = Root(Each.To);
            }
        }
        for (int32 Node = 0; Node < Count; ++Node)
        {
            Region[Node] = Root(Node);
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
        for (int32 Node = 0; Node < Count; ++Node)
        {
            if (IsFree(Node) && IsExecShape(P.Shapes[Node]) &&
                !In[Node].ContainsByPredicate([this](int32 Link) { return P.Links[Link].bExec; }))
            {
                Queue.Add({Node, INDEX_NONE, false});
            }
        }
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
                PlaceBlock(Inputs, Pos[Node]);
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
    // For a break: the row it leaves, so the reroutes go between that row and the next.
    TMap<int32, TArray<int32>> RowOfBreak;
    TMap<int32, float> RowLeftOfBreak;
    float NextRootY = 0.0f;

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

    // Lays out the producers of Consumer's data inputs that are still to place (claiming them), Consumer at At.
    void Feeds(int32 Consumer, const FVector2D& At, FPart& Part)
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
            Done[Each.From] = true;
            FChild& Kid = Kids.AddDefaulted_GetRef();
            Kid.Tree.Add(Each.From, FVector2D::ZeroVector);
            Feeds(Each.From, FVector2D::ZeroVector, Kid.Tree);
            Kid.Box = Bounds(P, Kid.Tree);
            Kid.Anchor = P.Shapes[Each.From].DataOut[Each.FromPin];
            Kid.Target = Reader.DataIn[Each.ToPin];
        }
        if (Kids.Num() == 0)
        {
            return;
        }
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
            const float Line = Kids.Num() == 1 ? Kid.Target : First + Index * Pitch;
            const FVector2D Offset(At.X - K.DataGap - Width(Kid.Tree.Nodes[0]), At.Y + Line - Kid.Anchor);
            Part.Append(Kid.Tree, Offset);
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

    // Puts Part's local origin at Want, moved down until it touches nothing (NeedDown). With bSlide a row that goes on
    // from a node keeps its white line level by moving right instead, when a free spot is near enough and no other
    // region lies between.
    FVector2D PlaceBlock(const FPart& Part, FVector2D Want, bool bSlide = false)
    {
        constexpr float SlideStep = 32.0f;
        constexpr float MaxSlide = 1600.0f;
        const int32 InRegion = Part.Nodes.Num() > 0 ? Region[Part.Nodes[0]] : INDEX_NONE;
        const float Left = Part.Nodes.Num() > 0 ? static_cast<float>(Want.X + Bounds(P, Part).Min.X) : static_cast<float>(Want.X);
        for (float Slide = 0.0f; bSlide && Slide <= MaxSlide; Slide += SlideStep)
        {
            if (NeedDown(Part, Want + FVector2D(Slide, 0.0f), InRegion, Left) <= 0.0f)
            {
                Want.X += Slide;
                break;
            }
        }
        for (int32 Step = 0; Step < 400; ++Step)
        {
            const float Down = NeedDown(Part, Want, InRegion, TNumericLimits<float>::Max());
            if (Down <= 0.0f)
            {
                break;
            }
            Want.Y += Down;
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
        if (Start.Link == INDEX_NONE || Start.bSide || Start.bBreak || Row.Num() == 0)
        {
            return false;
        }
        const int32 From = P.Links[Start.Link].From;
        int32 Next = INDEX_NONE;
        for (const int32 Link : Out[Row.Last()])
        {
            const FLink& Each = P.Links[Link];
            if (Each.bExec && !IsFree(Each.To) && Region[Each.To] == Region[From] && Pos[Each.To].X > Pos[From].X)
            {
                Next = Each.To;
                break;
            }
        }
        if (Next == INDEX_NONE)
        {
            return false;
        }
        const float Threshold = Pos[From].X + Width(From);
        const float Need = Want.X + Bounds(P, Part).Max.X + K.ExecGap - Pos[Next].X;
        if (Need > 0.0f)
        {
            TSet<int32> Shift;
            for (int32 Node = 0; Node < Count; ++Node)
            {
                if (Done[Node] && Region[Node] == Region[From] && Pos[Node].X >= Threshold)
                {
                    Shift.Add(Node);
                }
            }
            // A value right of A that only nodes moving read moves with them, so its wires do not stretch.
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
                if (Start.Link == INDEX_NONE && Floating == INDEX_NONE)
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
            const float Top = -ExecLine(Shape);
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
            int32 Next = INDEX_NONE;
            int32 NextLink = INDEX_NONE;
            for (const int32 Link : Out[Node])
            {
                const FLink& Each = P.Links[Link];
                if (!Each.bExec || Done[Each.To] || !IsFree(Each.To) || !IsExecShape(P.Shapes[Each.To]))
                {
                    continue;
                }
                if (Next == INDEX_NONE)
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
        if (Start.Link != INDEX_NONE)
        {
            const FLink& From = P.Links[Start.Link];
            const FShape& Source = P.Shapes[From.From];
            const float Line = Pos[From.From].Y + (Source.ExecOut.IsValidIndex(From.FromPin) ? Source.ExecOut[From.FromPin] : ExecLine(Source));
            const float* Left = Start.bBreak ? RowLeftOfBreak.Find(Start.Link) : nullptr;
            Want = Left ? FVector2D(*Left, Line) : FVector2D(Pos[From.From].X + Source.Width + K.ExecGap, Line);
        }
        const bool bInsert = MakeRoom(Start, Part, Row, Want);
        const FVector2D At = PlaceBlock(Part, Want, !bInsert && Start.Link != INDEX_NONE && !Start.bSide && !Start.bBreak);
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
        for (const int32 Link : Out[Node])
        {
            const FLink& Each = P.Links[Link];
            const FShape& Reader = P.Shapes[Each.To];
            if (!Each.bExec && Each.To != Node && Done[Each.To] && !Part.Nodes.Contains(Each.To) && Reader.DataIn.IsValidIndex(Each.ToPin) &&
                P.Shapes[Node].DataOut.IsValidIndex(Each.FromPin))
            {
                Want = FVector2D(Pos[Each.To].X - K.DataGap - Width(Node), Pos[Each.To].Y + Reader.DataIn[Each.ToPin] - P.Shapes[Node].DataOut[Each.FromPin]);
                bBeside = true;
                break;
            }
        }
        const FVector2D At = PlaceBlock(Part, Want);
        if (!bBeside)
        {
            NextRootY = FMath::Max(NextRootY, static_cast<float>(Bounds(P, Part).Max.Y + At.Y + K.RowGap));
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
