#include "Foundation/GraphLayout/McpRuled.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRuledTest, "EngineWire.Foundation.GraphLayout.Ruled",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace McpRuledTestShapes
{
using McpRuled::FShape;

// An exec node: exec in and out at Line, data inputs one row apart below it.
FShape Exec(float Width, float Line, int32 Inputs = 0, int32 ExecOuts = 1)
{
    FShape Shape;
    Shape.Width = Width;
    Shape.ExecIn = Line;
    for (int32 Out = 0; Out < ExecOuts; ++Out)
    {
        Shape.ExecOut.Add(Line + Out * 26.0f);
    }
    for (int32 In = 0; In < Inputs; ++In)
    {
        Shape.DataIn.Add(Line + (In + 1) * 26.0f);
    }
    Shape.Height = Line + FMath::Max(Inputs, ExecOuts - 1) * 26.0f + 23.0f;
    return Shape;
}

FShape Event(float Width)
{
    FShape Shape = Exec(Width, 44.0f);
    Shape.ExecIn = -1.0f;
    return Shape;
}

// A variable read: one output in the middle of a 32 tall pill.
FShape Pill(float Width = 120.0f)
{
    FShape Shape;
    Shape.Width = Width;
    Shape.Height = 32.0f;
    Shape.DataOut.Add(16.0f);
    return Shape;
}

// A pure node with Inputs inputs 26 apart from 44 and one output.
FShape Pure(int32 Inputs)
{
    FShape Shape;
    Shape.Width = 160.0f;
    for (int32 In = 0; In < Inputs; ++In)
    {
        Shape.DataIn.Add(44.0f + In * 26.0f);
    }
    Shape.DataOut.Add(44.0f);
    Shape.Height = 44.0f + (Inputs - 1) * 26.0f + 23.0f;
    return Shape;
}
} // namespace McpRuledTestShapes

// Known answers for the house style: a row's exec pins level whatever each node's pin offset; inputs one column left
// of their reader, a pair centred on its pins, three evenly spread with the middle level with the second pin; deeper
// data under the row; a node that stays is not moved and a new chain goes on right of it; a second exec output starts
// a row of its own below; an over-long row breaks with two reroutes between the rows.
bool FMcpRuledTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace McpRuledTestShapes;
    using McpRuled::FLink;
    using McpRuled::FProblem;
    using McpRuled::FSolution;
    const McpRuled::FParams Params;

    // Event -> Set (exec at 28) -> call with a subtitle (exec at 60): one white line.
    {
        FProblem Chain;
        Chain.Shapes = {Event(200.0f), Exec(180.0f, 28.0f), Exec(260.0f, 60.0f)};
        Chain.Links = {{0, 0, 1, 0, true}, {1, 0, 2, 0, true}};
        Chain.Free = {true, true, true};
        Chain.Pos.Init(FVector2D::ZeroVector, 3);
        Chain.Origin = FVector2D(100.0, 300.0);
        const FSolution Out = McpRuled::Solve(Chain, Params);
        TestEqual(TEXT("the root's white line is at the origin"), Out.Pos[0].Y + 44.0, 300.0, 1e-3);
        TestEqual(TEXT("Set's exec pin level with the event's"), Out.Pos[1].Y + 28.0, 300.0, 1e-3);
        TestEqual(TEXT("the subtitled call's exec pin level too"), Out.Pos[2].Y + 60.0, 300.0, 1e-3);
        TestEqual(TEXT("exec nodes ExecGap apart"), Out.Pos[1].X, 100.0 + 200.0 + Params.ExecGap, 1e-3);
        TestEqual(TEXT("and the next one"), Out.Pos[2].X, Out.Pos[1].X + 180.0 + Params.ExecGap, 1e-3);
    }

    // A pure node reading two variables: the pair centred on its two pins, one column left of it.
    {
        FProblem Pair;
        Pair.Shapes = {Pure(2), Pill(), Pill()};
        Pair.Links = {{1, 0, 0, 0, false}, {2, 0, 0, 1, false}};
        Pair.Free = {false, true, true};
        Pair.Pos = {FVector2D(1000.0, 0.0), FVector2D::ZeroVector, FVector2D::ZeroVector};
        const FSolution Out = McpRuled::Solve(Pair, Params);
        TestTrue(TEXT("a node that stays is not moved"), Out.Pos[0].Equals(FVector2D(1000.0, 0.0)));
        TestEqual(TEXT("the inputs' right edge DataGap left of the reader"), Out.Pos[1].X + 120.0, 1000.0 - Params.DataGap, 1e-3);
        TestEqual(TEXT("the two in one column"), Out.Pos[2].X, Out.Pos[1].X, 1e-3);
        const double Middle = (Out.Pos[1].Y + 16.0 + Out.Pos[2].Y + 16.0) * 0.5;
        TestEqual(TEXT("the pair's outputs centred on the two input pins"), Middle, (44.0 + 70.0) * 0.5, 1e-3);
        TestTrue(TEXT("the two do not overlap"), Out.Pos[2].Y >= Out.Pos[1].Y + 32.0 + Params.InputGap - 1e-3);
    }

    // Three inputs: evenly spread, the middle one level with the second pin.
    {
        FProblem Three;
        Three.Shapes = {Pure(3), Pill(), Pill(), Pill()};
        Three.Links = {{1, 0, 0, 0, false}, {2, 0, 0, 1, false}, {3, 0, 0, 2, false}};
        Three.Free = {false, true, true, true};
        Three.Pos = {FVector2D(0.0, 0.0), FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector};
        const FSolution Out = McpRuled::Solve(Three, Params);
        TestEqual(TEXT("the middle input level with the second pin"), Out.Pos[2].Y + 16.0, 70.0, 1e-3);
        TestEqual(TEXT("evenly spread"), Out.Pos[2].Y - Out.Pos[1].Y, Out.Pos[3].Y - Out.Pos[2].Y, 1e-3);
    }

    // An exec node reading a pure node that reads a variable: that data goes below the row.
    {
        FProblem Deep;
        Deep.Shapes = {Event(200.0f), Exec(200.0f, 44.0f, 1), Pure(1), Pill()};
        Deep.Links = {{0, 0, 1, 0, true}, {2, 0, 1, 0, false}, {3, 0, 2, 0, false}};
        Deep.Free = {true, true, true, true};
        Deep.Pos.Init(FVector2D::ZeroVector, 4);
        const FSolution Out = McpRuled::Solve(Deep, Params);
        const double RowBottom = FMath::Max(Out.Pos[0].Y + Deep.Shapes[0].Height, Out.Pos[1].Y + Deep.Shapes[1].Height);
        TestTrue(TEXT("two columns of data sit under the exec row"), Out.Pos[2].Y >= RowBottom + Params.LaneGap - 1e-3 && Out.Pos[3].Y >= RowBottom);
        TestTrue(TEXT("each producer left of its reader"), Out.Pos[2].X + 160.0 <= Out.Pos[1].X && Out.Pos[3].X + 120.0 <= Out.Pos[2].X);
        TestTrue(TEXT("the reader still clears the event"), Out.Pos[1].X >= Out.Pos[0].X + 200.0 + Params.ExecGap - 1e-3);
    }

    // A new chain hung off a node that stays goes on right of it, exec pins level; a second exec output gets a row below.
    {
        FProblem Branch;
        Branch.Shapes = {Exec(200.0f, 44.0f), Exec(176.0f, 44.0f, 0, 2), Exec(200.0f, 28.0f), Exec(200.0f, 28.0f)};
        Branch.Links = {{0, 0, 1, 0, true}, {1, 0, 2, 0, true}, {1, 1, 3, 0, true}};
        Branch.Free = {false, true, true, true};
        Branch.Pos = {FVector2D(500.0, 500.0), FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector};
        const FSolution Out = McpRuled::Solve(Branch, Params);
        TestEqual(TEXT("the new node right of the one that stays"), Out.Pos[1].X, 500.0 + 200.0 + Params.ExecGap, 1e-3);
        TestEqual(TEXT("its exec pin level with it"), Out.Pos[1].Y + 44.0, 544.0, 1e-3);
        TestEqual(TEXT("True goes on in the row"), Out.Pos[2].Y + 28.0, 544.0, 1e-3);
        TestTrue(TEXT("False starts right of the branch"), Out.Pos[3].X >= Out.Pos[1].X + 176.0);
        TestTrue(TEXT("below the row"), Out.Pos[3].Y >= Out.Pos[2].Y + Branch.Shapes[2].Height + Params.RowGap - 1e-3);
    }

    // A chain going on from a node that stays, its data hanging over another region below the row: it slides right,
    // keeps the line level, and its data clears that region.
    {
        FProblem Blocked;
        Blocked.Shapes = {Exec(200.0f, 44.0f), Exec(200.0f, 44.0f, 1), Pure(1), Pill()};
        Blocked.Links = {{0, 0, 1, 0, true}, {2, 0, 1, 0, false}, {3, 0, 2, 0, false}};
        Blocked.Free = {false, true, true, true};
        Blocked.Pos.Init(FVector2D::ZeroVector, 4);
        const FBox2D Other(FVector2D(0.0, 160.0), FVector2D(400.0, 300.0));
        Blocked.Obstacles = {Other};
        const FSolution Out = McpRuled::Solve(Blocked, Params);
        TestEqual(TEXT("its exec pin still level with the node it goes on from"), Out.Pos[1].Y, 0.0, 1e-3);
        bool bClear = true;
        for (int32 Node = 1; Node < 4; ++Node)
        {
            const FBox2D Box(Out.Pos[Node], Out.Pos[Node] + FVector2D(Blocked.Shapes[Node].Width, Blocked.Shapes[Node].Height));
            bClear &= !Box.ExpandBy(Params.RegionGap - 1.0).Intersect(Other);
        }
        TestTrue(TEXT("its data keeps RegionGap from the other region"), bClear);
    }

    // A lone variable read into an exec node sits in the gap before it, below the white wire, not across it.
    {
        FProblem Lone;
        Lone.Shapes = {Event(200.0f), Exec(200.0f, 44.0f, 1), Pill()};
        Lone.Links = {{0, 0, 1, 0, true}, {2, 0, 1, 0, false}};
        Lone.Free.Init(true, 3);
        Lone.Pos.Init(FVector2D::ZeroVector, 3);
        const FSolution Out = McpRuled::Solve(Lone, Params);
        const double Line = Out.Pos[1].Y + 44.0;
        TestTrue(TEXT("the read's top keeps clear below the white wire"), Out.Pos[2].Y >= Line + 16.0 - 1e-3);
        TestTrue(TEXT("one column left of its reader"), FMath::IsNearlyEqual(Out.Pos[2].X + 120.0, Out.Pos[1].X - Params.DataGap, 1e-3));
    }

    // Two events: the second one's logic keeps RegionGap clear of the first's, never in among it.
    {
        FProblem Two;
        Two.Shapes = {Event(200.0f), Exec(200.0f, 44.0f, 1), Pure(1), Pill(), Event(200.0f), Exec(200.0f, 44.0f)};
        Two.Links = {{0, 0, 1, 0, true}, {2, 0, 1, 0, false}, {3, 0, 2, 0, false}, {4, 0, 5, 0, true}};
        Two.Free.Init(true, 6);
        Two.Pos.Init(FVector2D::ZeroVector, 6);
        const FSolution Out = McpRuled::Solve(Two, Params);
        double FirstBottom = 0.0;
        for (int32 Node = 0; Node < 4; ++Node)
        {
            FirstBottom = FMath::Max(FirstBottom, Out.Pos[Node].Y + Two.Shapes[Node].Height);
        }
        TestTrue(TEXT("the second event's region starts RegionGap below the first's"),
                 FMath::Min(Out.Pos[4].Y, Out.Pos[5].Y) >= FirstBottom + Params.RegionGap - 1e-3);
    }

    // An insert between two nodes that stay (A -> new -> B -> C): B and C move right to make room, on their line;
    // a node of another region stays.
    {
        FProblem Insert;
        Insert.Shapes = {Exec(200.0f, 44.0f), Exec(200.0f, 44.0f), Exec(200.0f, 44.0f, 1), Exec(200.0f, 44.0f), Exec(200.0f, 44.0f), Pill()};
        Insert.Links = {{0, 0, 3, 0, true}, {3, 0, 1, 0, true}, {1, 0, 2, 0, true}, {5, 0, 2, 0, false}};
        Insert.Free = {false, false, false, true, false, false};
        Insert.Pos = {FVector2D(0.0, 0.0), FVector2D(296.0, 0.0), FVector2D(592.0, 0.0), FVector2D::ZeroVector, FVector2D(3000.0, 0.0),
                      FVector2D(150.0, 150.0)};
        const FSolution Out = McpRuled::Solve(Insert, Params);
        TestEqual(TEXT("the new node right after A"), Out.Pos[3].X, 296.0, 1e-3);
        TestEqual(TEXT("level with A"), Out.Pos[3].Y, 0.0, 1e-3);
        TestEqual(TEXT("B moved right to make room"), Out.Pos[1].X, 296.0 + 200.0 + Params.ExecGap, 1e-3);
        TestEqual(TEXT("C moved with it"), Out.Pos[2].X, 592.0 + 296.0, 1e-3);
        TestTrue(TEXT("A and the other region stay"), Out.Pos[0].IsZero() && Out.Pos[4].Equals(FVector2D(3000.0, 0.0)));
        TestEqual(TEXT("C's input, left of C but right of A, moves with C"), Out.Pos[5].X, 150.0 + 296.0, 1e-3);
    }

    // A chain hung off an event that stays, with three lone events of their own regions under it where the chain's
    // data goes: the chain keeps the event's white line and those events move down, in order, out of its way.
    {
        FProblem Ghosts;
        Ghosts.Shapes = {Event(200.0f), Exec(200.0f, 44.0f, 1), Pure(1), Pill(), Event(200.0f), Event(200.0f), Event(200.0f)};
        Ghosts.Links = {{0, 0, 1, 0, true}, {2, 0, 1, 0, false}, {3, 0, 2, 0, false}};
        Ghosts.Free = {false, true, true, true, false, false, false};
        Ghosts.Pos = {FVector2D(0.0, -144.0), FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector,
                      FVector2D(0.0, 0.0), FVector2D(0.0, 208.0), FVector2D(0.0, 416.0)};
        const FSolution Out = McpRuled::Solve(Ghosts, Params);
        TestEqual(TEXT("the chain keeps the event's white line"), Out.Pos[1].Y + 44.0, -144.0 + 44.0, 1e-3);
        TestTrue(TEXT("the event it hangs off stays"), Out.Pos[0].Equals(FVector2D(0.0, -144.0)));
        TestTrue(TEXT("the events in the way moved down"), Out.Pos[4].Y > 0.0);
        TestEqual(TEXT("keeping their spacing"), Out.Pos[5].Y - Out.Pos[4].Y, 208.0, 1e-3);
        TestEqual(TEXT("all three"), Out.Pos[6].Y - Out.Pos[5].Y, 208.0, 1e-3);
        TestTrue(TEXT("and their column"), Out.Pos[4].X == 0.0 && Out.Pos[5].X == 0.0 && Out.Pos[6].X == 0.0);
        bool bClear = true;
        for (int32 Mine = 1; Mine < 4; ++Mine)
        {
            const FBox2D Box(Out.Pos[Mine], Out.Pos[Mine] + FVector2D(Ghosts.Shapes[Mine].Width, Ghosts.Shapes[Mine].Height));
            const FBox2D Ghost(Out.Pos[4], Out.Pos[4] + FVector2D(Ghosts.Shapes[4].Width, Ghosts.Shapes[4].Height));
            bClear &= !Box.ExpandBy(Params.RegionGap - 1.0).Intersect(Ghost);
        }
        TestTrue(TEXT("the chain's data keeps RegionGap from them"), bClear);
    }

    // An insert (A -> new -> B -> C) in a region where A's Sequence also runs a second row (D -> E): B and C make room,
    // the second row stays where it is.
    {
        FProblem Side;
        Side.Shapes = {Exec(176.0f, 44.0f, 0, 2), Exec(200.0f, 44.0f), Exec(200.0f, 44.0f), Exec(200.0f, 44.0f), Exec(200.0f, 44.0f),
                       Exec(200.0f, 44.0f), Exec(200.0f, 44.0f)};
        // 0 Sequence, 1 A, 2 B, 3 C, 4 D, 5 E, 6 new.
        Side.Links = {{0, 0, 1, 0, true}, {0, 1, 4, 0, true}, {4, 0, 5, 0, true}, {1, 0, 6, 0, true}, {6, 0, 2, 0, true}, {2, 0, 3, 0, true}};
        Side.Free = {false, false, false, false, false, false, true};
        Side.Pos = {FVector2D(-300.0, 0.0), FVector2D(0.0, 0.0), FVector2D(296.0, 0.0), FVector2D(592.0, 0.0), FVector2D(0.0, 300.0),
                    FVector2D(500.0, 300.0), FVector2D::ZeroVector};
        const FSolution Out = McpRuled::Solve(Side, Params);
        TestTrue(TEXT("the new node right after A, on its line"), Out.Pos[6].Equals(FVector2D(296.0, 0.0), 1e-3));
        TestEqual(TEXT("B made room"), Out.Pos[2].X, 296.0 + 200.0 + Params.ExecGap, 1e-3);
        TestEqual(TEXT("C with it"), Out.Pos[3].X, 592.0 + 296.0, 1e-3);
        TestTrue(TEXT("the Sequence's second row did not move"), Out.Pos[4].Equals(FVector2D(0.0, 300.0)) && Out.Pos[5].Equals(FVector2D(500.0, 300.0)));
    }

    // An insert whose data, under the row, would land on a row of the same region just below: the new node keeps A's
    // line and that lower row moves down instead, keeping its column.
    {
        FProblem Dense;
        Dense.Shapes = {Exec(200.0f, 44.0f, 0, 2), Exec(200.0f, 44.0f), Exec(200.0f, 44.0f, 1), Pure(1), Pill(), Exec(200.0f, 44.0f)};
        // 0 A (two exec outputs), 1 B, 2 new, 3 and 4 its data, 5 D on A's second output.
        Dense.Links = {{0, 0, 2, 0, true}, {2, 0, 1, 0, true}, {3, 0, 2, 0, false}, {4, 0, 3, 0, false}, {0, 1, 5, 0, true}};
        Dense.Free = {false, false, true, true, true, false};
        Dense.Pos = {FVector2D(0.0, 0.0), FVector2D(500.0, 0.0), FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector,
                     FVector2D(100.0, 140.0)};
        const FSolution Out = McpRuled::Solve(Dense, Params);
        TestEqual(TEXT("the new node keeps A's line"), Out.Pos[2].Y, 0.0, 1e-3);
        TestEqual(TEXT("right after A"), Out.Pos[2].X, 296.0, 1e-3);
        TestTrue(TEXT("the lower row moved down"), Out.Pos[5].Y > 140.0);
        TestEqual(TEXT("keeping its column"), Out.Pos[5].X, 100.0, 1e-3);
        bool bClear = true;
        const FBox2D Lower(Out.Pos[5], Out.Pos[5] + FVector2D(200.0, Dense.Shapes[5].Height));
        for (int32 Mine = 2; Mine < 5; ++Mine)
        {
            const FBox2D Box(Out.Pos[Mine], Out.Pos[Mine] + FVector2D(Dense.Shapes[Mine].Width, Dense.Shapes[Mine].Height));
            bClear &= !Box.ExpandBy(Params.NodePad - 1.0).Intersect(Lower);
        }
        TestTrue(TEXT("nothing of the insert overlaps it"), bClear);

        McpRuled::FParams Arrange;
        Arrange.bAllowMovingExisting = false;
        const FSolution Fixed = McpRuled::Solve(Dense, Arrange);
        TestTrue(TEXT("arrange keeps all unlisted nodes fixed"),
                 Fixed.Pos[0] == Dense.Pos[0] && Fixed.Pos[1] == Dense.Pos[1] && Fixed.Pos[5] == Dense.Pos[5]);
    }

    // A connected output need not be level with its own node's input (e.g. Branch False).
    {
        FProblem Chain;
        Chain.Shapes = {Event(200.0f), Exec(200.0f, 44.0f, 0, 2), Exec(200.0f, 60.0f), Exec(200.0f, 44.0f)};
        Chain.Shapes[1].ExecOut[1] = 76.0f;
        Chain.Links = {{0, 0, 1, 0, true}, {1, 1, 2, 0, true}, {2, 0, 3, 0, true}};
        Chain.Free.Init(true, 4);
        Chain.Pos.Init(FVector2D::ZeroVector, 4);
        const FSolution Out = McpRuled::Solve(Chain, Params);
        TestEqual(TEXT("the receiver matches the connected False pin"), Out.Pos[2].Y + 60.0, Out.Pos[1].Y + 76.0, 1e-3);
        TestEqual(TEXT("the next edge inherits that row height"), Out.Pos[3].Y + 44.0, Out.Pos[2].Y + 60.0, 1e-3);
    }

    // Ten 300-wide nodes with rows at most 1500 wide: broken twice, each break two reroutes between the rows.
    {
        FProblem Long;
        for (int32 Node = 0; Node < 10; ++Node)
        {
            Long.Shapes.Add(Node == 0 ? Event(300.0f) : Exec(300.0f, 44.0f));
            if (Node > 0)
            {
                Long.Links.Add({Node - 1, 0, Node, 0, true});
            }
        }
        Long.Free.Init(true, 10);
        Long.Pos.Init(FVector2D::ZeroVector, 10);
        McpRuled::FParams Narrow;
        Narrow.MaxRowWidth = 1500.0f;
        const FSolution Out = McpRuled::Solve(Long, Narrow);
        TestEqual(TEXT("two breaks"), Out.Breaks.Num(), 2);
        TestEqual(TEXT("four nodes in the first row"), Out.Pos[3].Y, Out.Pos[0].Y, 1e-3);
        TestTrue(TEXT("the fifth starts the second row, under the first"), Out.Pos[4].Y > Out.Pos[3].Y + 67.0 && FMath::IsNearlyEqual(Out.Pos[4].X, Out.Pos[0].X));
        if (Out.Breaks.Num() > 0)
        {
            const McpRuled::FRowBreak& First = Out.Breaks[0];
            TestTrue(TEXT("reroute A right of the row it leaves"), First.A.X > Out.Pos[3].X + 300.0);
            TestTrue(TEXT("reroute B left of the row it enters"), First.B.X < Out.Pos[4].X);
            TestTrue(TEXT("both in the gap between the rows"), First.A.Y == First.B.Y && First.A.Y > Out.Pos[3].Y + 67.0 && First.A.Y < Out.Pos[4].Y);
        }
    }
    // A loop's terminal completion must not sit to the left of its already placed body.
    {
        FProblem Loop;
        FShape Terminal = Exec(180.0f, 44.0f, 1, 0);
        Loop.Shapes = {Event(180.0f), Exec(240.0f, 44.0f, 0, 2), Exec(260.0f, 44.0f),
                       Exec(260.0f, 44.0f), Terminal};
        Loop.Links = {{0, 0, 1, 0, true}, {1, 0, 2, 0, true}, {2, 0, 3, 0, true}, {1, 1, 4, 0, true}};
        Loop.Free.Init(true, 5);
        Loop.Pos.Init(FVector2D::ZeroVector, 5);
        const FSolution Out = McpRuled::Solve(Loop, Params);
        TestTrue(TEXT("terminal completion is right of the loop body, not back inside it"),
            Out.Pos[4].X >= Out.Pos[3].X + Loop.Shapes[3].Width + Params.ExecGap - 1.0f);
        TestTrue(TEXT("terminal is below its execution body, leaving a bottom completion lane"),
            Out.Pos[4].Y >= Out.Pos[3].Y + Loop.Shapes[3].Height + Params.RowGap - 1.0f);
        TestEqual(TEXT("completion has an explicit two-point route"), Out.Breaks.Num(), 1);

    }
    // A calculation reading a loop Index must not be tucked under the loop, to its left.
    {
        FProblem IndexFlow;
        FShape LoopShape = Exec(240.0f, 44.0f);
        LoopShape.DataOut.Add(96.0f);
        IndexFlow.Shapes = {Event(180.0f), LoopShape, Exec(180.0f, 44.0f, 1), Pure(1), Pure(1)};
        IndexFlow.Links = {{0, 0, 1, 0, true}, {1, 0, 2, 0, true}, {1, 0, 3, 0, false}, {3, 0, 4, 0, false}, {4, 0, 2, 0, false}};
        IndexFlow.Free.Init(true, 5);
        IndexFlow.Pos.Init(FVector2D::ZeroVector, 5);
        const FSolution Out = McpRuled::Solve(IndexFlow, Params);
        TestTrue(TEXT("Index calculation follows its exec producer left to right"),
            Out.Pos[3].X >= Out.Pos[1].X + LoopShape.Width + Params.DataGap - 1.0f);
    }
    // Partial arrangement keeps the loop fixed but must still respect the origin of its Index value.
    {
        FProblem FixedIndex;
        FShape LoopShape = Exec(240.0f, 44.0f);
        LoopShape.DataOut.Add(96.0f);
        FixedIndex.Shapes = {LoopShape, Exec(180.0f, 44.0f, 1), Pure(1), Pure(1)};
        FixedIndex.Links = {{0, 0, 1, 0, true}, {0, 0, 2, 0, false}, {2, 0, 3, 0, false}, {3, 0, 1, 0, false}};
        FixedIndex.Free = {false, true, true, true};
        FixedIndex.Pos.Init(FVector2D::ZeroVector, 4);
        FixedIndex.Pos[0] = FVector2D(1000.0, 0.0);
        McpRuled::FParams FixedParams;
        FixedParams.bAllowMovingExisting = false;
        const FSolution Out = McpRuled::Solve(FixedIndex, FixedParams);
        TestTrue(TEXT("unselected Index producer stays fixed"), Out.Pos[0] == FixedIndex.Pos[0]);
        TestTrue(TEXT("Index computation follows its fixed producer"),
            Out.Pos[2].X >= 1000.0 + LoopShape.Width + FixedParams.DataGap - 1.0f);
    }
    // An early exit shares a result with a loop in a later lower row (the distance-sort fixture).
    {
        FProblem Early;
        FShape Return = Exec(180.0f, 44.0f, 0, 0);
        Early.Shapes = {Event(180.0f), Exec(180.0f, 44.0f, 0, 2), Exec(240.0f, 44.0f, 0, 2),
                        Exec(240.0f, 44.0f), Exec(240.0f, 44.0f), Return};
        Early.Links = {{0,0,1,0,true},{1,0,2,0,true},{1,1,5,0,true},{2,0,3,0,true},{2,1,4,0,true},{4,0,5,0,true}};
        Early.Free.Init(true, 6);
        Early.Pos.Init(FVector2D::ZeroVector, 6);
        const FSolution Out = McpRuled::Solve(Early, Params);
        for (int32 Other = 0; Other < 5; ++Other)
        {
            TestTrue(TEXT("shared early-exit return clears every later body row"),
                Out.Pos[5].Y >= Out.Pos[Other].Y + Early.Shapes[Other].Height + Params.RowGap - 1.0f);
        }
    }
    return true;
}
#endif
