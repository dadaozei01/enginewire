#include "Foundation/GraphLayout/McpRuled.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "NodeFactory.h"
#include "SGraphNode.h"
#include "SGraphPin.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/StrongObjectPtr.h"

namespace McpRuled
{
namespace RuledNodes
{
// Slate metrics of a default Blueprint node, from hand-arranged graphs (BP_Manager): a title bar, then one pin row per
// pin down each side. Approximate by design; Slate measures the real ones when the graph is drawn.
constexpr float TitleHeight = 32.0f;
constexpr float SubtitleHeight = 16.0f;
constexpr float FirstRow = 12.0f;
constexpr float RowPitch = 26.0f;
// An unlinked input that shows a value box (number, bool, text) is a little taller.
constexpr float EditorExtra = 8.0f;
constexpr float BottomPad = 23.0f;
constexpr float AdvancedArrow = 20.0f;
// A Set node draws a short "SET" header.
constexpr float SetTitleHeight = 16.0f;
// Compact nodes (operators) and pills (variable reads, self): no title bar.
constexpr float CompactFirst = 17.0f;
constexpr float CompactPitch = 28.0f;
constexpr float PillHeight = 32.0f;

struct FPins
{
    UEdGraphPin* ExecIn = nullptr;
    TArray<UEdGraphPin*> ExecOut;
    TArray<UEdGraphPin*> DataIn;
    TArray<UEdGraphPin*> DataOut;
};

float TextWidth(const FString& Text)
{
    float Width = 0.0f;
    for (const TCHAR Char : Text)
    {
        Width += Char < 0x2E80 ? 7.0f : 13.0f;
    }
    return Width;
}

bool IsShown(const UEdGraphNode& Node, const UEdGraphPin* Pin)
{
    return Pin && !Pin->bHidden && !(Pin->bAdvancedView && Node.AdvancedPinDisplay == ENodeAdvancedPins::Hidden && Pin->LinkedTo.Num() == 0);
}

bool IsExecPin(const UEdGraphPin* Pin)
{
    return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
}

// Width of the value box an unlinked input shows, 0 when it shows none.
float EditorWidth(const UEdGraphPin* Pin)
{
    if (Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() > 0 || Pin->PinType.IsContainer() || Pin->PinType.bIsReference)
    {
        return 0.0f;
    }
    const FName Category = Pin->PinType.PinCategory;
    if (Category == UEdGraphSchema_K2::PC_Boolean)
    {
        return 24.0f;
    }
    if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64 || Category == UEdGraphSchema_K2::PC_Real ||
        Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double)
    {
        return 64.0f;
    }
    if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
    {
        return Pin->PinType.PinSubCategoryObject.IsValid() ? 120.0f : 64.0f;
    }
    if (Category == UEdGraphSchema_K2::PC_Name || Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Text)
    {
        return 96.0f;
    }
    if (Category == UEdGraphSchema_K2::PC_Struct && Pin->PinType.PinSubCategoryObject.IsValid())
    {
        const FName Struct = Pin->PinType.PinSubCategoryObject->GetFName();
        if (Struct == NAME_Vector || Struct == NAME_Rotator || Struct == TEXT("Vector3f"))
        {
            return 180.0f;
        }
        if (Struct == NAME_LinearColor || Struct == NAME_Vector2D)
        {
            return 120.0f;
        }
    }
    return 0.0f;
}

FString PinLabel(const UEdGraphNode& Node, const UEdGraphPin* Pin)
{
    if (IsExecPin(Pin) && (Pin->PinName == UEdGraphSchema_K2::PN_Execute || Pin->PinName == UEdGraphSchema_K2::PN_Then))
    {
        return FString();
    }
    return Node.GetPinDisplayName(Pin).ToString();
}

void CollectPins(const UEdGraphNode& Node, FPins& Out)
{
    for (UEdGraphPin* Pin : Node.Pins)
    {
        if (!IsShown(Node, Pin))
        {
            continue;
        }
        const bool bExec = IsExecPin(Pin);
        if (Pin->Direction == EGPD_Input)
        {
            if (bExec)
            {
                Out.ExecIn = Out.ExecIn ? Out.ExecIn : Pin;
            }
            else
            {
                Out.DataIn.Add(Pin);
            }
        }
        else
        {
            (bExec ? Out.ExecOut : Out.DataOut).Add(Pin);
        }
    }
}

// One private graph per layout invocation. Native pin constructors may normalize defaults; their writes
// stay in this context. Reuse it across all nodes instead of allocating an entire graph per measurement.
struct FMeasureContext
{
    TStrongObjectPtr<UBlueprint> Preview;
    TMap<const UEdGraphNode*, UEdGraphNode*> Nodes;

    explicit FMeasureContext(const UEdGraph* SourceGraph)
    {
        if (!IsInGameThread() || !FSlateApplication::IsInitialized() || !SourceGraph) { return; }
        const UBlueprint* SourceBlueprint = FBlueprintEditorUtils::FindBlueprintForGraph(SourceGraph);
        if (!SourceBlueprint) { return; }
        Preview.Reset(NewObject<UBlueprint>(GetTransientPackage()));
        Preview->ParentClass = SourceBlueprint->ParentClass;
        Preview->GeneratedClass = SourceBlueprint->GeneratedClass;
        Preview->SkeletonGeneratedClass = SourceBlueprint->SkeletonGeneratedClass;
        Preview->BlueprintType = SourceBlueprint->BlueprintType;
        Preview->NewVariables = SourceBlueprint->NewVariables;
        UEdGraph* PreviewGraph = DuplicateObject<UEdGraph>(SourceGraph, Preview.Get());
        Preview->UbergraphPages.Add(PreviewGraph);
        for (int32 Index = 0; Index < SourceGraph->Nodes.Num() && Index < PreviewGraph->Nodes.Num(); ++Index)
        {
            if (SourceGraph->Nodes[Index] && PreviewGraph->Nodes[Index])
            {
                Nodes.Add(SourceGraph->Nodes[Index], PreviewGraph->Nodes[Index]);
            }
        }
    }
};

// Arrange a private native widget at normal edit detail in identity geometry; cached offsets belong to an
// earlier paint and possibly another zoom. Fall back completely if a visible pin cannot be measured.
bool MeasureShape(const UEdGraphNode& Node, const FPins& Pins, FShape& Shape, const FMeasureContext& Context)
{
    if (!Node.IsA<UK2Node>() || Node.IsA<UK2Node_Knot>()) { return false; }
    UEdGraphNode* const* Found = Context.Nodes.Find(&Node);
    UEdGraphNode* PreviewNode = Found ? *Found : nullptr;
    if (!PreviewNode) { return false; }
    const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(PreviewNode);
    if (!Widget.IsValid()) { return false; }
    Widget->SlatePrepass(1.0f);
    const FVector2D Size = Widget->GetDesiredSize();
    if (!FMath::IsFinite(Size.X) || !FMath::IsFinite(Size.Y) || Size.X <= 0.0 || Size.Y <= 0.0 ||
        Size.X > 65536.0 || Size.Y > 65536.0) { return false; }

    TMap<const SWidget*, const UEdGraphPin*> Wanted;
    const auto Want = [&](UEdGraphPin* Pin)
    {
        UEdGraphPin* PreviewPin = PreviewNode->FindPinById(Pin->PinId);
        if (PreviewPin)
        {
            if (const TSharedPtr<SGraphPin> PinWidget = Widget->FindWidgetForPin(PreviewPin))
            {
                Wanted.Add(PinWidget.Get(), Pin);
            }
        }
    };
    if (Pins.ExecIn) { Want(Pins.ExecIn); }
    for (UEdGraphPin* Pin : Pins.ExecOut) { Want(Pin); }
    for (UEdGraphPin* Pin : Pins.DataIn) { Want(Pin); }
    for (UEdGraphPin* Pin : Pins.DataOut) { Want(Pin); }
    const int32 Expected = (Pins.ExecIn ? 1 : 0) + Pins.ExecOut.Num() + Pins.DataIn.Num() + Pins.DataOut.Num();
    if (Wanted.Num() != Expected) { return false; }
    TMap<const UEdGraphPin*, float> Centres;
    TFunction<void(const TSharedRef<SWidget>&, const FGeometry&, int32)> Visit;
    Visit = [&](const TSharedRef<SWidget>& Child, const FGeometry& Geometry, int32 Depth)
    {
        if (Depth > 128) { return; }
        if (const UEdGraphPin* const* Pin = Wanted.Find(&Child.Get()))
        {
            // SGraphPin::Tick uses the centre of the allotted pin row for its connection offset.
            const FVector2D Centre = Geometry.LocalToAbsolute(FVector2D(0.0, Geometry.GetLocalSize().Y * 0.5f));
            if (FMath::IsFinite(Centre.Y) && Centre.Y >= 0.0 && Centre.Y <= Size.Y)
            {
                Centres.Add(*Pin, static_cast<float>(Centre.Y));
            }
            return;
        }
        FArrangedChildren Children(EVisibility::Visible);
        Child->ArrangeChildren(Geometry, Children);
        for (int32 Index = 0; Index < Children.Num(); ++Index)
        {
            Visit(Children[Index].Widget, Children[Index].Geometry, Depth + 1);
        }
    };
    Visit(Widget.ToSharedRef(), FGeometry::MakeRoot(Size, FSlateLayoutTransform()), 0);
    if (Centres.Num() != Expected) { return false; }
    FShape Measured;
    Measured.Width = static_cast<float>(Size.X);
    Measured.Height = static_cast<float>(Size.Y);
    Measured.ExecIn = Pins.ExecIn ? Centres.FindChecked(Pins.ExecIn) : -1.0f;
    for (UEdGraphPin* Pin : Pins.ExecOut) { Measured.ExecOut.Add(Centres.FindChecked(Pin)); }
    for (UEdGraphPin* Pin : Pins.DataIn) { Measured.DataIn.Add(Centres.FindChecked(Pin)); }
    for (UEdGraphPin* Pin : Pins.DataOut) { Measured.DataOut.Add(Centres.FindChecked(Pin)); }
    Shape = MoveTemp(Measured);
    return true;
}

FShape ShapeAndPins(const UEdGraphNode& Node, FPins& Pins, const FMeasureContext& Context)
{
    CollectPins(Node, Pins);
    FShape Shape;
    if (MeasureShape(Node, Pins, Shape, Context)) { return Shape; }
    TArray<const UEdGraphPin*> Left;
    TArray<const UEdGraphPin*> Right;
    for (const UEdGraphPin* Pin : Node.Pins)
    {
        if (IsShown(Node, Pin))
        {
            (Pin->Direction == EGPD_Input ? Left : Right).Add(Pin);
        }
    }
    TMap<const UEdGraphPin*, float> Centre;
    const UK2Node* K2 = Cast<UK2Node>(&Node);
    const bool bKnot = Node.IsA<UK2Node_Knot>();
    const bool bPill = Node.IsA<UK2Node_VariableGet>() || Node.IsA<UK2Node_Self>();
    const bool bCompact = !bKnot && !bPill && K2 && K2->ShouldDrawCompact();
    if (bKnot)
    {
        Shape.Width = 32.0f;
        Shape.Height = 24.0f;
        for (const UEdGraphPin* Pin : Node.Pins)
        {
            Centre.Add(Pin, 12.0f);
        }
    }
    else if (bPill || bCompact)
    {
        const float First = bPill ? PillHeight * 0.5f : CompactFirst;
        const float Pitch = bPill ? RowPitch : CompactPitch;
        float Y = First;
        float Editors = 0.0f;
        for (const UEdGraphPin* Pin : Left)
        {
            const float Editor = EditorWidth(Pin);
            Centre.Add(Pin, Y);
            Y += Pitch + (Editor > 0.0f ? EditorExtra : 0.0f);
            Editors = FMath::Max(Editors, Editor);
        }
        const float LastIn = Left.Num() > 0 ? Centre[Left.Last()] : First;
        Shape.Height = FMath::Max(bPill ? PillHeight : CompactFirst * 2.0f, LastIn + First);
        for (int32 Index = 0; Index < Right.Num(); ++Index)
        {
            Centre.Add(Right[Index], Shape.Height * 0.5f + (Index - (Right.Num() - 1) * 0.5f) * Pitch);
        }
        // A variable read shows only the variable's name.
        const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(&Node);
        const FString Title = Variable ? Variable->GetVarName().ToString() : Node.GetNodeTitle(ENodeTitleType::ListView).ToString();
        Shape.Width = bPill ? FMath::Max(80.0f, TextWidth(Title) + 56.0f) : 112.0f + (Editors > 0.0f ? 48.0f : 0.0f);
    }
    else
    {
        // The full title's extra lines are its subtitles ("Target is Actor", "Custom Event"), each one a row taller.
        const FString Title = Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
        TArray<FString> Lines;
        Title.ParseIntoArrayLines(Lines);
        float TitleWidth = 0.0f;
        for (const FString& Line : Lines)
        {
            TitleWidth = FMath::Max(TitleWidth, TextWidth(Line));
        }
        const float Header = Node.IsA<UK2Node_VariableSet>() ? SetTitleHeight : TitleHeight + FMath::Max(0, Lines.Num() - 1) * SubtitleHeight;
        float LeftWidth = 0.0f;
        float RightWidth = 0.0f;
        float Y = Header + FirstRow;
        float LastLeft = 0.0f;
        for (const UEdGraphPin* Pin : Left)
        {
            const float Editor = EditorWidth(Pin);
            Centre.Add(Pin, Y);
            LastLeft = Y;
            Y += RowPitch + (Editor > 0.0f ? EditorExtra : 0.0f);
            LeftWidth = FMath::Max(LeftWidth, TextWidth(PinLabel(Node, Pin)) + Editor);
        }
        Y = Header + FirstRow;
        float LastRight = 0.0f;
        for (const UEdGraphPin* Pin : Right)
        {
            // An event's delegate pin is the small square in its title bar, not a row.
            if (Node.IsA<UK2Node_Event>() && Pin->PinName == UK2Node_Event::DelegateOutputName)
            {
                Centre.Add(Pin, TitleHeight * 0.5f);
                continue;
            }
            Centre.Add(Pin, Y);
            LastRight = Y;
            Y += RowPitch;
            RightWidth = FMath::Max(RightWidth, TextWidth(PinLabel(Node, Pin)));
        }
        Shape.Height = FMath::Max(LastLeft, LastRight) > 0.0f ? FMath::Max(LastLeft, LastRight) + BottomPad : Header + 16.0f;
        Shape.Height += Node.AdvancedPinDisplay != ENodeAdvancedPins::NoPins ? AdvancedArrow : 0.0f;
        Shape.Width = FMath::Max3(112.0f, TitleWidth + 72.0f, LeftWidth + RightWidth + 80.0f);
    }
    const auto At = [&Centre, &Shape](const UEdGraphPin* Pin) { return Centre.Contains(Pin) ? Centre[Pin] : Shape.Height * 0.5f; };
    Shape.ExecIn = Pins.ExecIn ? At(Pins.ExecIn) : -1.0f;
    for (const UEdGraphPin* Pin : Pins.ExecOut)
    {
        Shape.ExecOut.Add(At(Pin));
    }
    for (const UEdGraphPin* Pin : Pins.DataIn)
    {
        Shape.DataIn.Add(At(Pin));
    }
    for (const UEdGraphPin* Pin : Pins.DataOut)
    {
        Shape.DataOut.Add(At(Pin));
    }
    return Shape;
}

UK2Node_Knot* MakeKnot(UEdGraph& Graph, const FVector2D& Centre)
{
    FGraphNodeCreator<UK2Node_Knot> Creator(Graph);
    UK2Node_Knot* Knot = Creator.CreateNode(false);
    Knot->NodePosX = FMath::RoundToInt(Centre.X - 16.0f);
    Knot->NodePosY = FMath::RoundToInt(Centre.Y - 12.0f);
    Creator.Finalize();
    return Knot;
}
} // namespace RuledNodes

FShape EstimateShape(const UEdGraphNode& Node)
{
    RuledNodes::FPins Pins;
    const RuledNodes::FMeasureContext Context(Node.GetGraph());
    return RuledNodes::ShapeAndPins(Node, Pins, Context);
}

float LayoutNodes(UEdGraph* Graph, const TArray<UEdGraphNode*>& Free, const FVector2D* Origin, TArray<UEdGraphNode*>* OutAdded,
                  const TSet<UEdGraphNode*>* Ignore, const FParams& Params, FString* OutFailure)
{
    using namespace RuledNodes;
    const float NoneBottom = Origin ? static_cast<float>(Origin->Y) : 0.0f;
    FString InternalFailure;
    if (!OutFailure) { OutFailure = &InternalFailure; }
    OutFailure->Reset();
    if (!Graph || Free.Num() == 0)
    {
        return NoneBottom;
    }
    const TSet<UEdGraphNode*> FreeSet(Free);
    const FMeasureContext Context(Graph);
    FProblem Problem;
    TArray<UEdGraphNode*> Nodes;
    TArray<FPins> Pins;
    TMap<const UEdGraphNode*, int32> Index;
    FBox2D Fixed(ForceInit);
    bool bAnyFree = false;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>() || (Ignore && Ignore->Contains(Node)))
        {
            continue;
        }
        Index.Add(Node, Nodes.Add(Node));
        FShape Shape = ShapeAndPins(*Node, Pins.AddDefaulted_GetRef(), Context);
        const bool bFree = FreeSet.Contains(Node);
        bAnyFree |= bFree;
        const FVector2D At(Node->NodePosX, Node->NodePosY);
        if (!bFree)
        {
            Fixed += At;
            Fixed += At + FVector2D(Shape.Width, Shape.Height);
            // A comment bubble (a disabled ghost event's note among them) is drawn above the node: keep clear of it too.
            if (!Node->NodeComment.IsEmpty())
            {
                TArray<FString> Lines;
                Node->NodeComment.ParseIntoArrayLines(Lines);
                float Wide = 0.0f;
                for (const FString& Line : Lines)
                {
                    Wide = FMath::Max(Wide, TextWidth(Line));
                }
                const float Tall = 18.0f * FMath::Max(1, Lines.Num()) + 24.0f;
                Problem.Obstacles.Add(FBox2D(At - FVector2D(0.0f, Tall), At + FVector2D(FMath::Max(Shape.Width, Wide + 48.0f), 0.0f)));
                Problem.ObstacleOwner.Add(Nodes.Num() - 1);
            }
        }
        Problem.Shapes.Add(MoveTemp(Shape));
        Problem.Free.Add(bFree);
        Problem.Pos.Add(At);
    }
    if (!bAnyFree)
    {
        return NoneBottom;
    }
    TArray<TPair<UEdGraphPin*, UEdGraphPin*>> Wires;
    for (int32 From = 0; From < Nodes.Num(); ++From)
    {
        const auto AddOutputs = [&](const TArray<UEdGraphPin*>& Outputs, bool bExec)
        {
            for (int32 PinAt = 0; PinAt < Outputs.Num(); ++PinAt)
            {
                for (UEdGraphPin* Linked : Outputs[PinAt]->LinkedTo)
                {
                    const int32* To = Linked ? Index.Find(Linked->GetOwningNode()) : nullptr;
                    // Links between two nodes that stay count too: they tell which nodes make one region.
                    if (!To)
                    {
                        continue;
                    }
                    const int32 ToPin = bExec ? 0 : Pins[*To].DataIn.IndexOfByKey(Linked);
                    if (ToPin == INDEX_NONE)
                    {
                        continue;
                    }
                    Problem.Links.Add({From, PinAt, *To, ToPin, bExec});
                    Wires.Emplace(Outputs[PinAt], Linked);
                }
            }
        };
        AddOutputs(Pins[From].ExecOut, true);
        AddOutputs(Pins[From].DataOut, false);
    }
    Problem.Origin = Origin ? *Origin : (Fixed.bIsValid ? FVector2D(Fixed.Min.X, Fixed.Max.Y + 200.0f) : FVector2D::ZeroVector);
    FSolution Solution = Solve(Problem, Params);
    struct FGetterCopy
    {
        int32 Source = INDEX_NONE;
        int32 Copy = INDEX_NONE;
        FVector2D Reader = FVector2D::ZeroVector;
        TArray<int32> Links;
    };
    TArray<FGetterCopy> GetterCopies;
    // Plan copies in the geometry model first. Do not change the graph until the full layout validates.
    // Only pure, self-context member reads qualify: never copy an exec output, a calculation, a local
    // variable with a different scope, or a getter bound to another target object.
    const int32 OriginalNodes = Nodes.Num();
    if (Params.bLocalizeSharedGetters)
    {
        for (int32 Source = 0; Source < OriginalNodes; ++Source)
        {
            const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Nodes[Source]);
            if (!Getter || Getter->GetClass() != UK2Node_VariableGet::StaticClass() || !Getter->VariableReference.IsSelfContext() || Getter->VariableReference.IsLocalScope() ||
                Pins[Source].ExecIn || Pins[Source].ExecOut.Num() > 0) { continue; }
            const FProperty* Property = Getter->GetPropertyForVariable();
            if (!Property || Property->HasMetaData(TEXT("BlueprintGetter"))) { continue; }
            bool bHasTarget = false;
            for (UEdGraphPin* Input : Getter->Pins)
            {
                if (Input->Direction == EGPD_Input)
                {
                    bHasTarget |= Input->LinkedTo.Num() > 0 || Input->DefaultObject != nullptr;
                }
            }
            if (bHasTarget) { continue; }
            TArray<int32> Readers;
            int32 Keep = INDEX_NONE;
            double Nearest = TNumericLimits<double>::Max();
            for (int32 Link = 0; Link < Problem.Links.Num(); ++Link)
            {
                const FLink& Wire = Problem.Links[Link];
                if (Wire.bExec || Wire.From != Source) { continue; }
                Readers.Add(Link);
                const double Distance = (Solution.Pos[Wire.To] - Solution.Pos[Source]).SizeSquared();
                if (Distance < Nearest) { Nearest = Distance; Keep = Link; }
            }
            if (Readers.Num() < 2) { continue; }
            for (const int32 Link : Readers)
            {
                FLink& Wire = Problem.Links[Link];
                if (Link == Keep || !Problem.Free[Wire.To]) { continue; }
                const FVector2D Reader = Solution.Pos[Wire.To];
                const FVector2D Span = Reader - Solution.Pos[Source];
                if (FMath::Abs(Span.X) < Params.MaxSharedGetSpan && FMath::Abs(Span.Y) < Params.RowGap * 3.0f) { continue; }
                int32 Group = INDEX_NONE;
                for (int32 Candidate = 0; Candidate < GetterCopies.Num(); ++Candidate)
                {
                    const FGetterCopy& Copy = GetterCopies[Candidate];
                    if (Copy.Source == Source && FMath::Abs(Copy.Reader.X - Reader.X) <= Params.DataGap * 6.0f &&
                        FMath::Abs(Copy.Reader.Y - Reader.Y) <= Params.InputGap * 12.0f)
                    { Group = Candidate; break; }
                }
                if (Group == INDEX_NONE)
                {
                    Group = GetterCopies.AddDefaulted();
                    FGetterCopy& Copy = GetterCopies[Group];
                    Copy.Source = Source;
                    Copy.Copy = Nodes.Num();
                    Copy.Reader = Reader;
                    // Source pointers are placeholders until validation succeeds; no source node is moved here.
                    UEdGraphNode* SourceNode = Nodes[Source];
                    Nodes.Add(SourceNode);
                    FPins SourcePins = Pins[Source];
                    Pins.Add(MoveTemp(SourcePins));
                    const FShape Shape = Problem.Shapes[Source];
                    Problem.Shapes.Add(Shape);
                    Problem.Free.Add(true);
                    const FVector2D SourcePos = Problem.Pos[Source];
                    Problem.Pos.Add(SourcePos);
                }
                GetterCopies[Group].Links.Add(Link);
                Wire.From = GetterCopies[Group].Copy;
            }
        }
        if (GetterCopies.Num() > 0) { Solution = Solve(Problem, Params); }
    }
    const auto RouteKey = [&](const FRowBreak& Break, bool bLast, const FVector2D& Centre)
    {
        return FString::Printf(TEXT("%p:%p:%d:%d"), bLast ? nullptr : Wires[Break.Link].Key->GetOwningNode(),
            Wires[Break.Link].Value, FMath::RoundToInt(Centre.X - 16.0), FMath::RoundToInt(Centre.Y - 12.0));
    };
    // Validate for every caller, including automatic batch/code placement whose API omits OutFailure.
    {
        OutFailure->Reset();
        TArray<FBox2D> Boxes;
        for (int32 Node = 0; Node < Nodes.Num(); ++Node)
        {
            const FVector2D At(FMath::RoundToInt(Solution.Pos[Node].X), FMath::RoundToInt(Solution.Pos[Node].Y));
            const FShape& Shape = Problem.Shapes[Node];
            Boxes.Emplace(At, At + FVector2D(Shape.Width, Shape.Height));
            if (!Params.bAllowMovingExisting && !Problem.Free[Node] && At != Problem.Pos[Node])
            {
                *OutFailure = TEXT("Layout tried to move an unlisted node.");
                return NoneBottom;
            }
        }
        const auto Overlaps = [](const FBox2D& A, const FBox2D& B)
        {
            return A.Min.X < B.Max.X && B.Min.X < A.Max.X && A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y;
        };
        for (int32 Node = 0; Node < Nodes.Num(); ++Node)
        {
            for (int32 Other = 0; Other < Node; ++Other)
            {
                if ((Problem.Free[Node] || Problem.Free[Other]) && Overlaps(Boxes[Node], Boxes[Other]))
                {
                    *OutFailure = FString::Printf(TEXT("Layout overlaps nodes %s and %s."),
                        *Nodes[Node]->NodeGuid.ToString(), *Nodes[Other]->NodeGuid.ToString());
                    return NoneBottom;
                }
            }
        }
        TSet<FString> RoutedPoints;
        for (const FRowBreak& Break : Solution.Breaks)
        {
            int32 PointIndex = 0;
            for (const FVector2D& Centre : {Break.A, Break.B})
            {
                // Completion wires sharing an identical last knot and target share that knot. The
                // source-side knots stay distinct. Use rounded geometry, exactly as MakeKnot does.
                const bool bLast = PointIndex++ == 1;
                const FString Key = RouteKey(Break, bLast, Centre);
                if (RoutedPoints.Contains(Key)) { continue; }
                RoutedPoints.Add(Key);
                // MakeKnot rounds the top-left, using the same 32 by 24 footprint as ShapeAndPins.
                const FVector2D At(FMath::RoundToInt(Centre.X - 16.0), FMath::RoundToInt(Centre.Y - 12.0));
                const FBox2D KnotBox(At, At + FVector2D(32.0, 24.0));
                for (const FBox2D& Box : Boxes)
                {
                    if (Overlaps(KnotBox, Box))
                    {
                        *OutFailure = TEXT("A planned reroute overlaps a node or another reroute.");
                        return NoneBottom;
                    }
                }
                Boxes.Add(KnotBox);
            }
        }
    }
    // Commit the validated same-variable copies and rewire only their planned consumers. DuplicateObject
    // preserves the exact FMemberReference and pin types/defaults. Clear cloned link arrays directly:
    // BreakAllNodeLinks on a fresh duplicate could otherwise touch the original neighbours.
    if (GetterCopies.Num() > 0) { Graph->Modify(); }
    for (const FGetterCopy& Plan : GetterCopies)
    {
        UK2Node_VariableGet* Source = CastChecked<UK2Node_VariableGet>(Nodes[Plan.Source]);
        Source->Modify();
        UK2Node_VariableGet* Copy = DuplicateObject<UK2Node_VariableGet>(Source, Graph, MakeUniqueObjectName(Graph, Source->GetClass(), Source->GetFName()));
        Copy->SetFlags(RF_Transactional);
        Copy->CreateNewGuid();
        for (UEdGraphPin* Pin : Copy->Pins) { Pin->LinkedTo.Reset(); Pin->PinId = FGuid::NewGuid(); }
        Graph->AddNode(Copy, false, false);
        Nodes[Plan.Copy] = Copy;
        Pins[Plan.Copy] = FPins();
        CollectPins(*Copy, Pins[Plan.Copy]);
        for (const int32 Link : Plan.Links)
        {
            UEdGraphPin* Original = Wires[Link].Key;
            UEdGraphPin* Target = Wires[Link].Value;
            UEdGraphPin* Local = Copy->FindPin(Original->PinName, EGPD_Output);
            check(Local && Local->PinType == Original->PinType);
            Target->GetOwningNode()->Modify();
            Original->BreakLinkTo(Target);
            Local->MakeLinkTo(Target);
            Wires[Link].Key = Local;
        }
        if (OutAdded) { OutAdded->Add(Copy); }
    }
    float Bottom = -TNumericLimits<float>::Max();
    for (int32 Node = 0; Node < Nodes.Num(); ++Node)
    {
        if (Problem.Free[Node])
        {
            Nodes[Node]->NodePosX = FMath::RoundToInt(Solution.Pos[Node].X);
            Nodes[Node]->NodePosY = FMath::RoundToInt(Solution.Pos[Node].Y);
            Bottom = FMath::Max(Bottom, static_cast<float>(Solution.Pos[Node].Y + Problem.Shapes[Node].Height));
        }
        else if (!Solution.Pos[Node].Equals(Problem.Pos[Node], 0.5))
        {
            // Room made for an insert: the part of the region right of it moved over.
            Nodes[Node]->Modify();
            Nodes[Node]->NodePosX = FMath::RoundToInt(Solution.Pos[Node].X);
            Nodes[Node]->NodePosY = FMath::RoundToInt(Solution.Pos[Node].Y);
        }
    }
    // A broken row's white wire runs down to the next row through two reroutes in the gap between them.
    const UEdGraphSchema* Schema = Graph->GetSchema();
    TMap<FString, UK2Node_Knot*> RoutedPoints;
    for (const FRowBreak& Break : Solution.Breaks)
    {
        UEdGraphPin* Source = Wires[Break.Link].Key;
        UEdGraphPin* Target = Wires[Break.Link].Value;
        if (!Schema || !Source || !Target)
        {
            continue;
        }
        const FString StartKey = RouteKey(Break, false, Break.A);
        const FString EndKey = RouteKey(Break, true, Break.B);
        UK2Node_Knot* A = RoutedPoints.FindRef(StartKey);
        UK2Node_Knot* B = RoutedPoints.FindRef(EndKey);
        const bool bNewStart = A == nullptr;
        const bool bNewEnd = B == nullptr;
        if (bNewStart) { A = MakeKnot(*Graph, Break.A); RoutedPoints.Add(StartKey, A); }
        if (bNewEnd) { B = MakeKnot(*Graph, Break.B); RoutedPoints.Add(EndKey, B); }
        Source->BreakLinkTo(Target);
        Schema->TryCreateConnection(Source, A->GetInputPin());
        if (bNewStart) { Schema->TryCreateConnection(A->GetOutputPin(), B->GetInputPin()); }
        if (bNewEnd) { Schema->TryCreateConnection(B->GetOutputPin(), Target); }
        if (OutAdded)
        {
            if (bNewStart) { OutAdded->Add(A); }
            if (bNewEnd) { OutAdded->Add(B); }
        }
    }
    return Bottom > -TNumericLimits<float>::Max() ? Bottom : NoneBottom;
}
} // namespace McpRuled
