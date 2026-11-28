#include "Foundation/GraphLayout/McpRuled.h"
#include "Foundation/GraphLayout/McpGraphLayoutDiagnostics.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
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

// Measure a preview node widget using the caller's pin mapping (PreviewPin -> logical key).
bool MeasurePreviewNode(UEdGraphNode* PreviewNode, const FPins& PreviewPins, FShape& Shape)
{
    if (!PreviewNode) { return false; }
    const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(PreviewNode);
    if (!Widget.IsValid()) { return false; }
    Widget->SlatePrepass(1.0f);
    const FVector2D Size = Widget->GetDesiredSize();
    if (!FMath::IsFinite(Size.X) || !FMath::IsFinite(Size.Y) || Size.X <= 0.0 || Size.Y <= 0.0 ||
        Size.X > 65536.0 || Size.Y > 65536.0) { return false; }

    TMap<const SWidget*, const UEdGraphPin*> Wanted;
    const auto Want = [&](UEdGraphPin* PreviewPin)
    {
        if (PreviewPin)
        {
            if (const TSharedPtr<SGraphPin> PinWidget = Widget->FindWidgetForPin(PreviewPin))
            {
                Wanted.Add(PinWidget.Get(), PreviewPin);
            }
        }
    };
    if (PreviewPins.ExecIn) { Want(PreviewPins.ExecIn); }
    for (UEdGraphPin* Pin : PreviewPins.ExecOut) { Want(Pin); }
    for (UEdGraphPin* Pin : PreviewPins.DataIn) { Want(Pin); }
    for (UEdGraphPin* Pin : PreviewPins.DataOut) { Want(Pin); }
    const int32 Expected = (PreviewPins.ExecIn ? 1 : 0) + PreviewPins.ExecOut.Num() + PreviewPins.DataIn.Num() + PreviewPins.DataOut.Num();
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
    Measured.ExecIn = PreviewPins.ExecIn ? Centres.FindChecked(PreviewPins.ExecIn) : -1.0f;
    for (UEdGraphPin* Pin : PreviewPins.ExecOut) { Measured.ExecOut.Add(Centres.FindChecked(Pin)); }
    for (UEdGraphPin* Pin : PreviewPins.DataIn) { Measured.DataIn.Add(Centres.FindChecked(Pin)); }
    for (UEdGraphPin* Pin : PreviewPins.DataOut) { Measured.DataOut.Add(Centres.FindChecked(Pin)); }
    Shape = MoveTemp(Measured);
    return true;
}

// Arrange a private native widget at normal edit detail in identity geometry; cached offsets belong to an
// earlier paint and possibly another zoom. Fall back completely if a visible pin cannot be measured.
bool MeasureShape(const UEdGraphNode& Node, const FPins& Pins, FShape& Shape, const FMeasureContext& Context)
{
    if (!Node.IsA<UK2Node>() || Node.IsA<UK2Node_Knot>()) { return false; }
    UEdGraphNode* const* Found = Context.Nodes.Find(&Node);
    UEdGraphNode* PreviewNode = Found ? *Found : nullptr;
    if (!PreviewNode) { return false; }
    FPins PreviewPins;
    const auto MapPin = [&](UEdGraphPin* Pin) { return Pin ? PreviewNode->FindPinById(Pin->PinId) : nullptr; };
    if (Pins.ExecIn)
    {
        PreviewPins.ExecIn = MapPin(Pins.ExecIn);
        if (!PreviewPins.ExecIn) { return false; }
    }
    for (UEdGraphPin* Pin : Pins.ExecOut)
    {
        UEdGraphPin* Mapped = MapPin(Pin);
        if (!Mapped) { return false; }
        PreviewPins.ExecOut.Add(Mapped);
    }
    for (UEdGraphPin* Pin : Pins.DataIn)
    {
        UEdGraphPin* Mapped = MapPin(Pin);
        if (!Mapped) { return false; }
        PreviewPins.DataIn.Add(Mapped);
    }
    for (UEdGraphPin* Pin : Pins.DataOut)
    {
        UEdGraphPin* Mapped = MapPin(Pin);
        if (!Mapped) { return false; }
        PreviewPins.DataOut.Add(Mapped);
    }
    return MeasurePreviewNode(PreviewNode, PreviewPins, Shape);
}

// Verify a function input parameter can be read by a local UK2Node_VariableGet and measure its exact pill shape.
bool MeasureLocalParamShape(const UEdGraph* Graph, const FMeasureContext& Context, const FName ParamName,
                            const FEdGraphPinType& ExpectedType, FShape& OutShape)
{
    const UBlueprint* Blueprint = Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
    UClass* OwningClass = Blueprint
        ? (Blueprint->SkeletonGeneratedClass ? *Blueprint->SkeletonGeneratedClass : *Blueprint->GeneratedClass)
        : nullptr;
    if (!OwningClass) { return false; }
    FMemberReference Ref;
    Ref.SetLocalMember(ParamName, Graph->GetName(), FGuid());
    const FProperty* Property = Ref.ResolveMember<FProperty>(OwningClass);
    if (!Property || !Property->HasAnyPropertyFlags(CPF_Parm) ||
        Property->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm | CPF_ReferenceParm) ||
        Property->HasMetaData(TEXT("BlueprintGetter")))
    {
        return false;
    }
    FEdGraphPinType ResolvedType;
    if (!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, ResolvedType) || ResolvedType != ExpectedType)
    {
        return false;
    }
    if (Context.Preview.IsValid() && Context.Preview->UbergraphPages.Num() > 0)
    {
        UEdGraph* PreviewGraph = Context.Preview->UbergraphPages[0];
        UK2Node_VariableGet* PreviewGetter = NewObject<UK2Node_VariableGet>(PreviewGraph);
        PreviewGetter->CreateNewGuid();
        PreviewGetter->VariableReference.SetLocalMember(ParamName, Graph->GetName(), FGuid());
        PreviewGraph->Nodes.Add(PreviewGetter);
        PreviewGetter->AllocateDefaultPins();
        UEdGraphPin* ValuePin = PreviewGetter->FindPin(ParamName, EGPD_Output);
        if (!ValuePin || ValuePin->PinType != ExpectedType)
        {
            return false;
        }
        FPins PreviewPins;
        CollectPins(*PreviewGetter, PreviewPins);
        if (PreviewPins.ExecIn || PreviewPins.ExecOut.Num() > 0 || PreviewPins.DataIn.Num() > 0 || PreviewPins.DataOut.Num() != 1)
        {
            return false;
        }
        if (MeasurePreviewNode(PreviewGetter, PreviewPins, OutShape))
        {
            return true;
        }
    }
    OutShape = FShape();
    OutShape.Width = FMath::Max(80.0f, TextWidth(ParamName.ToString()) + 56.0f);
    OutShape.Height = PillHeight;
    OutShape.ExecIn = -1.0f;
    OutShape.DataOut.Add(PillHeight * 0.5f);
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
    McpGraphLayoutDiag::MarkAutoLayoutKnot(Knot);
    return Knot;
}

bool IsEmptyDefaultEvent(const UEdGraphNode* Node)
{
    const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
    if (!Event || Node->IsA<UK2Node_CustomEvent>())
    {
        return false;
    }
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->LinkedTo.Num() > 0)
        {
            return false;
        }
    }
    return true;
}

struct FPartitionedBlock
{
    TArray<int32> NodeIndices;
    TArray<FVector2D> LocalPos;
    TArray<FRowBreak> LocalBreaks;
    TMap<UEdGraphNode_Comment*, FBox2D> CommentBoxes;
    FBox2D Bounds = FBox2D(ForceInit);
    double SortY = 0.0;
    double SortX = 0.0;
    FString TieBreak;
};

void SolveSubsetIntoBlock(const FProblem& FullProblem, const TArray<int32>& SubsetIndices, const FParams& Params,
                          FPartitionedBlock& OutBlock)
{
    OutBlock = FPartitionedBlock();
    if (SubsetIndices.Num() == 0)
    {
        return;
    }
    TMap<int32, int32> GlobalToLocal;
    FProblem SubProblem;
    SubProblem.Origin = FVector2D::ZeroVector;
    SubProblem.Shapes.Reserve(SubsetIndices.Num());
    SubProblem.Free.Reserve(SubsetIndices.Num());
    SubProblem.Pos.Reserve(SubsetIndices.Num());
    for (int32 LocalIdx = 0; LocalIdx < SubsetIndices.Num(); ++LocalIdx)
    {
        const int32 GlobalIdx = SubsetIndices[LocalIdx];
        GlobalToLocal.Add(GlobalIdx, LocalIdx);
        SubProblem.Shapes.Add(FullProblem.Shapes[GlobalIdx]);
        SubProblem.Free.Add(true);
        SubProblem.Pos.Add(FullProblem.Pos[GlobalIdx]);
    }
    TArray<int32> LocalToGlobalLink;
    for (int32 LinkIdx = 0; LinkIdx < FullProblem.Links.Num(); ++LinkIdx)
    {
        const FLink& Wire = FullProblem.Links[LinkIdx];
        const int32* LocalFrom = GlobalToLocal.Find(Wire.From);
        const int32* LocalTo = GlobalToLocal.Find(Wire.To);
        if (LocalFrom && LocalTo)
        {
            FLink SubWire = Wire;
            SubWire.From = *LocalFrom;
            SubWire.To = *LocalTo;
            SubProblem.Links.Add(SubWire);
            LocalToGlobalLink.Add(LinkIdx);
        }
    }
    FParams SubParams = Params;
    SubParams.bLocalizeSharedGetters = false;
    SubParams.bProtectExistingComments = false;
    SubParams.bCollapseAutoExecKnots = false;
    const FSolution SubSol = Solve(SubProblem, SubParams);

    FBox2D RawBounds(ForceInit);
    for (int32 LocalIdx = 0; LocalIdx < SubsetIndices.Num(); ++LocalIdx)
    {
        const FVector2D At(FMath::RoundToInt(SubSol.Pos[LocalIdx].X), FMath::RoundToInt(SubSol.Pos[LocalIdx].Y));
        const FShape& Shape = SubProblem.Shapes[LocalIdx];
        RawBounds += At;
        RawBounds += At + FVector2D(Shape.Width, Shape.Height);
    }
    for (const FRowBreak& Br : SubSol.Breaks)
    {
        for (const FVector2D& Pt : {Br.A, Br.B})
        {
            const FVector2D At(FMath::RoundToInt(Pt.X - 16.0), FMath::RoundToInt(Pt.Y - 12.0));
            RawBounds += At;
            RawBounds += At + FVector2D(32.0, 24.0);
        }
    }
    const FVector2D Shift = RawBounds.bIsValid ? -RawBounds.Min : FVector2D::ZeroVector;
    OutBlock.NodeIndices = SubsetIndices;
    OutBlock.LocalPos.Reserve(SubsetIndices.Num());
    for (int32 LocalIdx = 0; LocalIdx < SubsetIndices.Num(); ++LocalIdx)
    {
        const FVector2D At(FMath::RoundToInt(SubSol.Pos[LocalIdx].X + Shift.X),
                           FMath::RoundToInt(SubSol.Pos[LocalIdx].Y + Shift.Y));
        OutBlock.LocalPos.Add(At);
    }
    for (const FRowBreak& Br : SubSol.Breaks)
    {
        if (LocalToGlobalLink.IsValidIndex(Br.Link))
        {
            FRowBreak GlobalBr = Br;
            GlobalBr.Link = LocalToGlobalLink[Br.Link];
            GlobalBr.A += Shift;
            GlobalBr.B += Shift;
            OutBlock.LocalBreaks.Add(GlobalBr);
        }
    }
    OutBlock.Bounds = RawBounds.bIsValid ? RawBounds.ShiftBy(Shift) : FBox2D(FVector2D::ZeroVector, FVector2D::ZeroVector);
}

// Stage 2.3 (EW-001 / EW-007): full-graph ruled layout partitioned by comment boxes.
// Solves each comment group (and nested child comments) independently, excludes empty default events
// into a dedicated top row, stacks all top-level blocks vertically at LeftX with RegionGap clearance,
// and refits comment boxes from innermost to outermost with exact measured node bounds.
void SolvePartitionedByComments(const FProblem& FullProblem, const TArray<UEdGraphNode*>& Nodes,
                                const TArray<McpGraphLayoutDiag::FCommentGroup>& CommentGroups,
                                const FParams& Params, FSolution& OutSolution,
                                TMap<UEdGraphNode_Comment*, FBox2D>& OutCommentRects)
{
    OutSolution.Pos = FullProblem.Pos;
    OutSolution.Breaks.Reset();
    OutCommentRects.Reset();

    TArray<int32> EmptyEventIndices;
    TMap<UEdGraphNode_Comment*, TArray<int32>> DirectMembers;
    TArray<int32> UnboxedIndices;

    for (int32 NodeIdx = 0; NodeIdx < Nodes.Num(); ++NodeIdx)
    {
        UEdGraphNode* Node = Nodes[NodeIdx];
        if (!Node)
        {
            continue;
        }
        if (IsEmptyDefaultEvent(Node))
        {
            EmptyEventIndices.Add(NodeIdx);
            continue;
        }
        UEdGraphNode_Comment* Owner = McpGraphLayoutDiag::FindPrimaryOwnerComment(Node, CommentGroups);
        if (Owner)
        {
            DirectMembers.FindOrAdd(Owner).Add(NodeIdx);
        }
        else
        {
            UnboxedIndices.Add(NodeIdx);
        }
    }

    // Build parent-child comment nesting hierarchy (CommentGroups is sorted innermost / smallest-area first).
    TMap<UEdGraphNode_Comment*, UEdGraphNode_Comment*> ParentOf;
    TMap<UEdGraphNode_Comment*, TArray<UEdGraphNode_Comment*>> ChildrenOf;
    TMap<UEdGraphNode_Comment*, FBox2D> OrigBoundsOf;
    for (const McpGraphLayoutDiag::FCommentGroup& Group : CommentGroups)
    {
        if (Group.Comment)
        {
            OrigBoundsOf.Add(Group.Comment, Group.OriginalBounds);
        }
    }
    for (int32 I = 0; I < CommentGroups.Num(); ++I)
    {
        UEdGraphNode_Comment* Child = CommentGroups[I].Comment;
        if (!Child)
        {
            continue;
        }
        const FBox2D& ChildBox = CommentGroups[I].OriginalBounds;
        const double ChildArea = ChildBox.GetArea();
        for (int32 J = I + 1; J < CommentGroups.Num(); ++J)
        {
            UEdGraphNode_Comment* CandParent = CommentGroups[J].Comment;
            if (!CandParent)
            {
                continue;
            }
            const FBox2D& ParentBox = CommentGroups[J].OriginalBounds;
            if (ParentBox.GetArea() > ChildArea + 1.0 &&
                ParentBox.IsInsideOrOn(ChildBox.Min) && ParentBox.IsInsideOrOn(ChildBox.Max))
            {
                ParentOf.Add(Child, CandParent);
                ChildrenOf.FindOrAdd(CandParent).Add(Child);
                break;
            }
        }
    }

    // Solve comments bottom-up (innermost first).
    TMap<UEdGraphNode_Comment*, FPartitionedBlock> CommentBlocks;
    for (const McpGraphLayoutDiag::FCommentGroup& Group : CommentGroups)
    {
        UEdGraphNode_Comment* Comment = Group.Comment;
        if (!Comment)
        {
            continue;
        }
        FPartitionedBlock Combined;
        Combined.SortY = Group.OriginalBounds.Min.Y;
        Combined.SortX = Group.OriginalBounds.Min.X;
        Combined.TieBreak = Comment->NodeGuid.ToString();

        double LocalY = 0.0;
        FBox2D ContentBounds(ForceInit);
        if (const TArray<int32>* Direct = DirectMembers.Find(Comment))
        {
            if (Direct->Num() > 0)
            {
                FPartitionedBlock DirectBlock;
                SolveSubsetIntoBlock(FullProblem, *Direct, Params, DirectBlock);
                Combined.NodeIndices.Append(DirectBlock.NodeIndices);
                Combined.LocalPos.Append(DirectBlock.LocalPos);
                Combined.LocalBreaks.Append(DirectBlock.LocalBreaks);
                ContentBounds += DirectBlock.Bounds;
                LocalY = DirectBlock.Bounds.Max.Y + Params.RegionGap * 0.5;
            }
        }
        if (TArray<UEdGraphNode_Comment*>* Children = ChildrenOf.Find(Comment))
        {
            Children->Sort([&](const UEdGraphNode_Comment& A, const UEdGraphNode_Comment& B)
            {
                const FBox2D& BoxA = OrigBoundsOf[&A];
                const FBox2D& BoxB = OrigBoundsOf[&B];
                if (!FMath::IsNearlyEqual(BoxA.Min.Y, BoxB.Min.Y, 1.0))
                {
                    return BoxA.Min.Y < BoxB.Min.Y;
                }
                if (!FMath::IsNearlyEqual(BoxA.Min.X, BoxB.Min.X, 1.0))
                {
                    return BoxA.Min.X < BoxB.Min.X;
                }
                return A.NodeGuid.ToString() < B.NodeGuid.ToString();
            });
            for (UEdGraphNode_Comment* Child : *Children)
            {
                if (const FPartitionedBlock* ChildBlock = CommentBlocks.Find(Child))
                {
                    const FVector2D ChildOffset(0.0, FMath::RoundToInt(LocalY));
                    for (int32 Idx = 0; Idx < ChildBlock->NodeIndices.Num(); ++Idx)
                    {
                        Combined.NodeIndices.Add(ChildBlock->NodeIndices[Idx]);
                        Combined.LocalPos.Add(ChildBlock->LocalPos[Idx] + ChildOffset);
                    }
                    for (FRowBreak Br : ChildBlock->LocalBreaks)
                    {
                        Br.A += ChildOffset;
                        Br.B += ChildOffset;
                        Combined.LocalBreaks.Add(Br);
                    }
                    for (const TPair<UEdGraphNode_Comment*, FBox2D>& Nested : ChildBlock->CommentBoxes)
                    {
                        Combined.CommentBoxes.Add(Nested.Key, Nested.Value.ShiftBy(ChildOffset));
                    }
                    const FBox2D ShiftedChild = ChildBlock->Bounds.ShiftBy(ChildOffset);
                    ContentBounds += ShiftedChild;
                    LocalY = ShiftedChild.Max.Y + Params.RegionGap * 0.5;
                }
            }
        }

        constexpr double PadX = 32.0;
        constexpr double PadBottom = 32.0;
        const double TitleBarHeight = FMath::Max(40.0, static_cast<double>(Comment->FontSize) * 1.8 + 10.0);
        const double PadTop = TitleBarHeight + 16.0;

        if (ContentBounds.bIsValid)
        {
            const FBox2D Outer(ContentBounds.Min - FVector2D(PadX, PadTop),
                               ContentBounds.Max + FVector2D(PadX, PadBottom));
            const FVector2D Shift = -Outer.Min;
            for (FVector2D& Pt : Combined.LocalPos)
            {
                Pt += Shift;
            }
            for (FRowBreak& Br : Combined.LocalBreaks)
            {
                Br.A += Shift;
                Br.B += Shift;
            }
            for (TPair<UEdGraphNode_Comment*, FBox2D>& Nested : Combined.CommentBoxes)
            {
                Nested.Value = Nested.Value.ShiftBy(Shift);
            }
            const FBox2D NormalizedOuter = Outer.ShiftBy(Shift);
            Combined.CommentBoxes.Add(Comment, NormalizedOuter);
            Combined.Bounds = NormalizedOuter;
        }
        else
        {
            const FBox2D EmptyOuter(FVector2D::ZeroVector,
                                    FVector2D(FMath::Max(64, Comment->NodeWidth), FMath::Max(48, Comment->NodeHeight)));
            Combined.CommentBoxes.Add(Comment, EmptyOuter);
            Combined.Bounds = EmptyOuter;
        }
        CommentBlocks.Add(Comment, MoveTemp(Combined));
    }

    // Collect top-level blocks (top-level comments + unboxed block), sorted stably by original Y then X.
    TArray<FPartitionedBlock> TopBlocks;
    for (const McpGraphLayoutDiag::FCommentGroup& Group : CommentGroups)
    {
        if (Group.Comment && !ParentOf.Contains(Group.Comment))
        {
            if (FPartitionedBlock* Block = CommentBlocks.Find(Group.Comment))
            {
                TopBlocks.Add(MoveTemp(*Block));
            }
        }
    }
    if (UnboxedIndices.Num() > 0)
    {
        FPartitionedBlock UnboxedBlock;
        SolveSubsetIntoBlock(FullProblem, UnboxedIndices, Params, UnboxedBlock);
        FBox2D OrigUnboxed(ForceInit);
        for (const int32 Idx : UnboxedIndices)
        {
            OrigUnboxed += FullProblem.Pos[Idx];
        }
        UnboxedBlock.SortY = OrigUnboxed.bIsValid ? OrigUnboxed.Min.Y : 0.0;
        UnboxedBlock.SortX = OrigUnboxed.bIsValid ? OrigUnboxed.Min.X : 0.0;
        UnboxedBlock.TieBreak = TEXT("~unboxed");
        TopBlocks.Add(MoveTemp(UnboxedBlock));
    }
    TopBlocks.Sort([](const FPartitionedBlock& A, const FPartitionedBlock& B)
    {
        if (!FMath::IsNearlyEqual(A.SortY, B.SortY, 1.0))
        {
            return A.SortY < B.SortY;
        }
        if (!FMath::IsNearlyEqual(A.SortX, B.SortX, 1.0))
        {
            return A.SortX < B.SortX;
        }
        return A.TieBreak < B.TieBreak;
    });

    // Build inter-block directed graph so comment boxes flow left-to-right across columns instead of
    // piling into one vertical stack with backward cross-comment wires.
    const int32 NumBlocks = TopBlocks.Num();
    TMap<int32, int32> NodeToBlock;
    TArray<bool> BlockHasExec;
    BlockHasExec.Init(false, NumBlocks);
    for (int32 BlockIdx = 0; BlockIdx < NumBlocks; ++BlockIdx)
    {
        for (const int32 GlobalNode : TopBlocks[BlockIdx].NodeIndices)
        {
            NodeToBlock.Add(GlobalNode, BlockIdx);
            const FShape& Shape = FullProblem.Shapes[GlobalNode];
            if (Shape.ExecIn >= 0.0f || Shape.ExecOut.Num() > 0)
            {
                BlockHasExec[BlockIdx] = true;
            }
        }
    }

    TArray<TArray<int32>> BlockOut;
    TArray<TArray<int32>> BlockExecIn;
    TArray<TArray<int32>> BlockAllIn;
    BlockOut.SetNum(NumBlocks);
    BlockExecIn.SetNum(NumBlocks);
    BlockAllIn.SetNum(NumBlocks);
    for (const FLink& Wire : FullProblem.Links)
    {
        const int32* FromBlock = NodeToBlock.Find(Wire.From);
        const int32* ToBlock = NodeToBlock.Find(Wire.To);
        if (!FromBlock || !ToBlock || *FromBlock == *ToBlock)
        {
            continue;
        }
        // Use exec edges and outputs from pure-only blocks (e.g. shared math comment boxes) to order columns.
        if (Wire.bExec || !BlockHasExec[*FromBlock])
        {
            BlockOut[*FromBlock].AddUnique(*ToBlock);
            BlockAllIn[*ToBlock].AddUnique(*FromBlock);
            if (Wire.bExec)
            {
                BlockExecIn[*ToBlock].AddUnique(*FromBlock);
            }
        }
    }

    // Assign column indices via longest-path relaxation (guarded against inter-block cycles).
    TArray<int32> BlockCol;
    BlockCol.Init(0, NumBlocks);
    for (int32 Pass = 0; Pass < NumBlocks; ++Pass)
    {
        bool bChanged = false;
        for (int32 U = 0; U < NumBlocks; ++U)
        {
            for (const int32 V : BlockOut[U])
            {
                if (BlockCol[U] + 1 > BlockCol[V] && BlockCol[U] + 1 < NumBlocks)
                {
                    BlockCol[V] = BlockCol[U] + 1;
                    bChanged = true;
                }
            }
        }
        if (!bChanged)
        {
            break;
        }
    }

    // Pull pure-only helper blocks (no exec nodes, no incoming edges, has consumers) right into their
    // earliest consumer's column so shared pure math sits directly below the block(s) reading it instead
    // of stretching long colored data wires from Column 0 across unrelated event boxes.
    for (int32 BlockIdx = 0; BlockIdx < NumBlocks; ++BlockIdx)
    {
        if (!BlockHasExec[BlockIdx] && BlockAllIn[BlockIdx].Num() == 0 && BlockOut[BlockIdx].Num() > 0)
        {
            int32 MinConsumerCol = TNumericLimits<int32>::Max();
            for (const int32 Consumer : BlockOut[BlockIdx])
            {
                MinConsumerCol = FMath::Min(MinConsumerCol, BlockCol[Consumer]);
            }
            if (MinConsumerCol < TNumericLimits<int32>::Max())
            {
                BlockCol[BlockIdx] = MinConsumerCol;
            }
        }
    }

    int32 MaxCol = 0;
    for (const int32 Col : BlockCol)
    {
        MaxCol = FMath::Max(MaxCol, Col);
    }
    TArray<TArray<int32>> Columns;
    Columns.SetNum(MaxCol + 1);
    TArray<double> ColWidth;
    ColWidth.Init(0.0, MaxCol + 1);
    for (int32 BlockIdx = 0; BlockIdx < NumBlocks; ++BlockIdx)
    {
        const int32 Col = BlockCol[BlockIdx];
        Columns[Col].Add(BlockIdx);
        ColWidth[Col] = FMath::Max(ColWidth[Col], TopBlocks[BlockIdx].Bounds.GetSize().X);
    }

    const double LeftX = FMath::RoundToInt(FullProblem.Origin.X);
    const double StartY = FMath::RoundToInt(FullProblem.Origin.Y);
    double Col0CursorY = StartY;

    // Place empty default events stacked vertically at top-left so their screen-space disabled bubbles never overlap.
    if (EmptyEventIndices.Num() > 0)
    {
        EmptyEventIndices.Sort([&](int32 A, int32 B)
        {
            return Nodes[A]->NodeGuid.ToString() < Nodes[B]->NodeGuid.ToString();
        });
        constexpr double BubbleVerticalGap = 80.0;
        for (const int32 Idx : EmptyEventIndices)
        {
            OutSolution.Pos[Idx] = FVector2D(LeftX, FMath::RoundToInt(Col0CursorY));
            Col0CursorY += FullProblem.Shapes[Idx].Height + BubbleVerticalGap;
            ColWidth[0] = FMath::Max(ColWidth[0], static_cast<double>(FullProblem.Shapes[Idx].Width));
        }
        Col0CursorY += Params.RegionGap * 0.5;
    }

    const double ColGapX = Params.RegionGap * 1.5;
    TArray<double> ColX;
    ColX.SetNum(MaxCol + 1);
    ColX[0] = LeftX;
    for (int32 Col = 1; Col <= MaxCol; ++Col)
    {
        ColX[Col] = FMath::RoundToInt(ColX[Col - 1] + ColWidth[Col - 1] + ColGapX);
    }

    TArray<double> BlockPlacedY;
    BlockPlacedY.Init(StartY, NumBlocks);
    TArray<double> BlockPlacedCenterY;
    BlockPlacedCenterY.Init(StartY, NumBlocks);
    TArray<bool> BlockPlaced;
    BlockPlaced.Init(false, NumBlocks);

    for (int32 Col = 0; Col <= MaxCol; ++Col)
    {
        TArray<int32>& ColBlocks = Columns[Col];
        TArray<double> IdealY;
        IdealY.Init(StartY, NumBlocks);
        for (const int32 BlockIdx : ColBlocks)
        {
            const TArray<int32>& Feeders = BlockExecIn[BlockIdx].Num() > 0 ? BlockExecIn[BlockIdx] : BlockAllIn[BlockIdx];
            double SumY = 0.0;
            int32 PlacedFeeders = 0;
            for (const int32 Pred : Feeders)
            {
                if (BlockPlaced[Pred])
                {
                    SumY += BlockPlacedCenterY[Pred];
                    ++PlacedFeeders;
                }
            }
            if (PlacedFeeders > 0)
            {
                const double AvgCenter = SumY / static_cast<double>(PlacedFeeders);
                IdealY[BlockIdx] = FMath::Max(StartY, AvgCenter - TopBlocks[BlockIdx].Bounds.GetSize().Y * 0.5);
            }
            else
            {
                IdealY[BlockIdx] = TopBlocks[BlockIdx].SortY;
            }
        }
        if (Col > 0)
        {
            ColBlocks.Sort([&](int32 A, int32 B)
            {
                // Keep exec blocks above pure-only helper blocks placed in the same column.
                if (BlockHasExec[A] != BlockHasExec[B])
                {
                    return BlockHasExec[A];
                }
                if (!FMath::IsNearlyEqual(IdealY[A], IdealY[B], 1.0))
                {
                    return IdealY[A] < IdealY[B];
                }
                if (!FMath::IsNearlyEqual(TopBlocks[A].SortY, TopBlocks[B].SortY, 1.0))
                {
                    return TopBlocks[A].SortY < TopBlocks[B].SortY;
                }
                return TopBlocks[A].TieBreak < TopBlocks[B].TieBreak;
            });
        }

        double CursorY = (Col == 0) ? Col0CursorY : StartY;
        for (const int32 BlockIdx : ColBlocks)
        {
            const FPartitionedBlock& Block = TopBlocks[BlockIdx];
            const double TargetY = (Col > 0 && (BlockExecIn[BlockIdx].Num() > 0 || BlockAllIn[BlockIdx].Num() > 0))
                ? FMath::Max(CursorY, IdealY[BlockIdx])
                : CursorY;
            const FVector2D Offset(ColX[Col], FMath::RoundToInt(TargetY));
            for (int32 Idx = 0; Idx < Block.NodeIndices.Num(); ++Idx)
            {
                OutSolution.Pos[Block.NodeIndices[Idx]] = Block.LocalPos[Idx] + Offset;
            }
            for (FRowBreak Br : Block.LocalBreaks)
            {
                Br.A += Offset;
                Br.B += Offset;
                OutSolution.Breaks.Add(Br);
            }
            for (const TPair<UEdGraphNode_Comment*, FBox2D>& Entry : Block.CommentBoxes)
            {
                OutCommentRects.Add(Entry.Key, Entry.Value.ShiftBy(Offset));
            }
            const double BlockH = Block.Bounds.GetSize().Y;
            BlockPlacedY[BlockIdx] = Offset.Y;
            BlockPlacedCenterY[BlockIdx] = Offset.Y + BlockH * 0.5;
            BlockPlaced[BlockIdx] = true;
            CursorY = Offset.Y + BlockH + Params.RegionGap;
        }
    }
}
} // namespace RuledNodes

FShape EstimateShape(const UEdGraphNode& Node)
{
    RuledNodes::FPins Pins;
    const RuledNodes::FMeasureContext Context(Node.GetGraph());
    return RuledNodes::ShapeAndPins(Node, Pins, Context);
}

void MeasureGraphShapes(const UEdGraph* Graph, TMap<const UEdGraphNode*, FShape>& OutShapes)
{
    OutShapes.Reset();
    if (!Graph)
    {
        return;
    }
    const RuledNodes::FMeasureContext Context(Graph);
    OutShapes.Reserve(Graph->Nodes.Num());
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>())
        {
            continue;
        }
        RuledNodes::FPins Pins;
        OutShapes.Add(Node, RuledNodes::ShapeAndPins(*Node, Pins, Context));
    }
}

float LayoutNodes(UEdGraph* Graph, const TArray<UEdGraphNode*>& Free, const FVector2D* Origin, TArray<UEdGraphNode*>* OutAdded,
                  const TSet<UEdGraphNode*>* Ignore, const FParams& Params, FString* OutFailure,
                  TMap<UEdGraphNode*, FIntPoint>* OutPreviewPositions,
                  TArray<UEdGraphNode*>* OutUpdatedComments)
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
    TSet<UEdGraphNode*> FreeSet(Free);
    if (Params.bCollapseAutoExecKnots && !Params.bPreviewOnly)
    {
        McpGraphLayoutDiag::CollapseAutoExecKnots(Graph, &FreeSet);
        McpGraphLayoutDiag::CollapseAutoDataKnots(Graph, &FreeSet);
        // Prune any collapsed auto-knot pointers from FreeSet.
        TSet<UEdGraphNode*> GraphNodeSet(Graph->Nodes);
        for (auto It = FreeSet.CreateIterator(); It; ++It)
        {
            if (!GraphNodeSet.Contains(*It))
            {
                It.RemoveCurrent();
            }
        }
        if (FreeSet.Num() == 0)
        {
            return NoneBottom;
        }
    }
    const FMeasureContext Context(Graph);
    FProblem Problem;
    TArray<UEdGraphNode*> Nodes;
    TArray<FPins> Pins;
    TMap<const UEdGraphNode*, int32> Index;
    TMap<const UEdGraphNode*, FVector2D> OriginalPositions;
    TMap<const UEdGraphNode*, FBox2D> InitialNodeBoxes;
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
        OriginalPositions.Add(Node, At);
        InitialNodeBoxes.Add(Node, FBox2D(At, At + FVector2D(Shape.Width, Shape.Height)));
        if (!bFree)
        {
            Fixed += At;
            Fixed += At + FVector2D(Shape.Width, Shape.Height);
            // A comment bubble (a disabled ghost event's note among them) is drawn above the node: keep clear of it too.
            if (!Node->NodeComment.IsEmpty() && !McpGraphLayoutDiag::IsAutoLayoutKnot(Node))
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
    const TArray<McpGraphLayoutDiag::FCommentGroup> CommentGroups =
        McpGraphLayoutDiag::CollectCommentGroups(Graph, InitialNodeBoxes);
    if (Params.bProtectExistingComments)
    {
        // Untouched comment boxes (0 free members) act as locked boundaries so batch/local layouts
        // never intrude into or detach unrelated comment regions (EW-007).
        for (const McpGraphLayoutDiag::FCommentGroup& Group : CommentGroups)
        {
            if (!Group.Comment || !Group.OriginalBounds.bIsValid)
            {
                continue;
            }
            bool bHasFreeMember = false;
            for (UEdGraphNode* Member : Group.Members)
            {
                if (FreeSet.Contains(Member))
                {
                    bHasFreeMember = true;
                    break;
                }
            }
            if (!bHasFreeMember)
            {
                Problem.Obstacles.Add(Group.OriginalBounds);
                Problem.ObstacleOwner.Add(INDEX_NONE);
            }
        }
    }
    TArray<TPair<UEdGraphPin*, UEdGraphPin*>> Wires;
    for (int32 From = 0; From < Nodes.Num(); ++From)
    {
        const auto AddOutputs = [&](const TArray<UEdGraphPin*>& Outputs, bool bExec)
        {
            for (int32 PinAt = 0; PinAt < Outputs.Num(); ++PinAt)
            {
                const bool bDelegate = !bExec &&
                    (Outputs[PinAt]->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate ||
                     Outputs[PinAt]->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate);
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
                    Problem.Links.Add({From, PinAt, *To, ToPin, bExec, bDelegate});
                    Wires.Emplace(Outputs[PinAt], Linked);
                }
            }
        };
        AddOutputs(Pins[From].ExecOut, true);
        AddOutputs(Pins[From].DataOut, false);
    }
    Problem.Origin = Origin ? *Origin : (Fixed.bIsValid ? FVector2D(Fixed.Min.X, Fixed.Max.Y + 200.0f) : FVector2D::ZeroVector);
    const bool bAllNodesFree = !Fixed.bIsValid;
    const bool bUseCommentPartition = Params.bAllowMovingExisting && bAllNodesFree && CommentGroups.Num() > 0;
    FSolution Solution;
    TMap<UEdGraphNode_Comment*, FBox2D> PartitionedCommentRects;
    if (bUseCommentPartition)
    {
        SolvePartitionedByComments(Problem, Nodes, CommentGroups, Params, Solution, PartitionedCommentRects);
    }
    else
    {
        Solution = Solve(Problem, Params);
    }
    enum class ECopyKind : uint8
    {
        DuplicateNode,
        FunctionParam
    };
    struct FCopyTargetPlan
    {
        bool bValid = false;
        ECopyKind Kind = ECopyKind::DuplicateNode;
        int32 Source = INDEX_NONE;
        FName ParamName = NAME_None;
        int32 ToPin = INDEX_NONE;
        FShape Shape;
    };
    struct FGetterCopy
    {
        ECopyKind Kind = ECopyKind::DuplicateNode;
        int32 Source = INDEX_NONE;
        int32 SourcePin = 0;
        FName ParamName = NAME_None;
        int32 Copy = INDEX_NONE;
        ECopyKind TargetKind = ECopyKind::DuplicateNode;
        int32 TargetSource = INDEX_NONE;
        FName TargetParamName = NAME_None;
        int32 TargetCopy = INDEX_NONE;
        int32 TargetWireLink = INDEX_NONE;
        FVector2D Reader = FVector2D::ZeroVector;
        TArray<int32> Links;
    };
    TArray<FGetterCopy> GetterCopies;
    // Plan copies in the geometry model first. Do not change the graph until the full layout validates.
    // Only pure, zero-side-effect reads qualify: self-context member variables, Self, read-only local
    // variables in the same scope, read-only function input parameters, and zero-argument native const
    // BlueprintPure member getters (e.g. Spline -> GetSplineLength, GetActorLocation). Never copy
    // calculations with arguments, static calls, exec outputs, mutated locals/params, or by-ref inputs.
    const int32 OriginalNodes = Nodes.Num();
    if (Params.bLocalizeSharedGetters && !bUseCommentPartition)
    {
        TSet<FName> MutatedLocals;
        for (UEdGraphNode* GraphNode : Graph->Nodes)
        {
            if (const UK2Node_VariableSet* Setter = Cast<UK2Node_VariableSet>(GraphNode))
            {
                if (Setter->VariableReference.IsLocalScope())
                {
                    MutatedLocals.Add(Setter->GetVarName());
                }
            }
        }
        const UEdGraph* TopGraph = FBlueprintEditorUtils::GetTopLevelGraph(Graph);
        const FString TopGraphName = TopGraph ? TopGraph->GetName() : FString();
        const auto IsSimplePureGetterNode = [&](const int32 NodeIndex, bool& bOutIsReadOnlyLocal) -> bool
        {
            bOutIsReadOnlyLocal = false;
            UEdGraphNode* Candidate = Nodes[NodeIndex];
            const bool bSelfNode = Candidate->GetClass() == UK2Node_Self::StaticClass();
            const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Candidate);
            const bool bGetterNode = Getter && Getter->GetClass() == UK2Node_VariableGet::StaticClass();
            if ((!bSelfNode && !bGetterNode) || Pins[NodeIndex].ExecIn || Pins[NodeIndex].ExecOut.Num() > 0 ||
                Pins[NodeIndex].DataOut.Num() != 1 || Pins[NodeIndex].DataOut[0]->ParentPin || Pins[NodeIndex].DataOut[0]->SubPins.Num() > 0)
            {
                return false;
            }
            if (bGetterNode)
            {
                const bool bSelfMember = Getter->VariableReference.IsSelfContext() && !Getter->VariableReference.IsLocalScope();
                const bool bReadOnlyLocal = Getter->VariableReference.IsLocalScope() &&
                    !TopGraphName.IsEmpty() && Getter->VariableReference.GetMemberScopeName() == TopGraphName &&
                    !MutatedLocals.Contains(Getter->GetVarName());
                if (!bSelfMember && !bReadOnlyLocal) { return false; }
                const FProperty* Property = Getter->GetPropertyForVariable();
                if (!Property || Property->HasMetaData(TEXT("BlueprintGetter"))) { return false; }
                for (UEdGraphPin* Input : Getter->Pins)
                {
                    if (Input->Direction == EGPD_Input && (Input->LinkedTo.Num() > 0 || Input->DefaultObject != nullptr))
                    {
                        return false;
                    }
                }
                bOutIsReadOnlyLocal = bReadOnlyLocal;
            }
            return true;
        };
        const auto PlanReaders = [&](const ECopyKind Kind, const int32 Source, const int32 SourcePin,
                                     const FName ParamName, const TArray<int32>& Readers, const int32 Keep,
                                     const FShape& CopyShape, const FCopyTargetPlan& TargetPlan)
        {
            for (const int32 Link : Readers)
            {
                const int32 ToNode = Problem.Links[Link].To;
                if (Link == Keep || !Problem.Free[ToNode]) { continue; }
                const FVector2D Reader = Solution.Pos[ToNode];
                const FVector2D Span = Reader - Solution.Pos[Source];
                if (Span.Size() < Params.MaxSharedGetSpan && FMath::Abs(Span.Y) < Params.RowGap * 2.5f) { continue; }
                int32 Group = INDEX_NONE;
                for (int32 Candidate = 0; Candidate < GetterCopies.Num(); ++Candidate)
                {
                    const FGetterCopy& Copy = GetterCopies[Candidate];
                    if (Copy.Kind == Kind && Copy.Source == Source && Copy.SourcePin == SourcePin &&
                        FMath::Abs(Copy.Reader.X - Reader.X) <= Params.DataGap * 6.0f &&
                        FMath::Abs(Copy.Reader.Y - Reader.Y) <= Params.InputGap * 12.0f)
                    { Group = Candidate; break; }
                }
                if (Group == INDEX_NONE)
                {
                    Group = GetterCopies.AddDefaulted();
                    FGetterCopy& Copy = GetterCopies[Group];
                    Copy.Kind = Kind;
                    Copy.Source = Source;
                    Copy.SourcePin = SourcePin;
                    Copy.ParamName = ParamName;
                    Copy.Copy = Nodes.Num();
                    Copy.Reader = Reader;
                    // Source pointers are placeholders until validation succeeds; no source node is moved here.
                    UEdGraphNode* SourceNode = Nodes[Source];
                    Nodes.Add(SourceNode);
                    Pins.AddDefaulted();
                    Problem.Shapes.Add(CopyShape);
                    Problem.Free.Add(true);
                    const FVector2D SourcePos = Problem.Pos[Source];
                    Problem.Pos.Add(SourcePos);
                    if (TargetPlan.bValid)
                    {
                        Copy.TargetKind = TargetPlan.Kind;
                        Copy.TargetSource = TargetPlan.Source;
                        Copy.TargetParamName = TargetPlan.ParamName;
                        Copy.TargetCopy = Nodes.Num();
                        UEdGraphNode* TargetSourceNode = Nodes[TargetPlan.Source];
                        Nodes.Add(TargetSourceNode);
                        Pins.AddDefaulted();
                        Problem.Shapes.Add(TargetPlan.Shape);
                        Problem.Free.Add(true);
                        const FVector2D TargetPos = Problem.Pos[TargetPlan.Source];
                        Problem.Pos.Add(TargetPos);
                        Copy.TargetWireLink = Problem.Links.Add({Copy.TargetCopy, 0, Copy.Copy, TargetPlan.ToPin, false});
                        Wires.Emplace(nullptr, nullptr);
                    }
                }
                GetterCopies[Group].Links.Add(Link);
                // Re-index Problem.Links after potential reallocation above.
                Problem.Links[Link].From = GetterCopies[Group].Copy;
                Problem.Links[Link].FromPin = 0;
            }
        };

        const FCopyTargetPlan NoTarget;
        for (int32 Source = 0; Source < OriginalNodes; ++Source)
        {
            UEdGraphNode* SourceNode = Nodes[Source];
            if (SourceNode->GetClass() == UK2Node_FunctionEntry::StaticClass())
            {
                if (TopGraph != Graph) { continue; }
                for (int32 PinAt = 0; PinAt < Pins[Source].DataOut.Num(); ++PinAt)
                {
                    UEdGraphPin* ParamPin = Pins[Source].DataOut[PinAt];
                    if (!ParamPin || ParamPin->ParentPin || ParamPin->SubPins.Num() > 0 ||
                        ParamPin->PinType.bIsReference || MutatedLocals.Contains(ParamPin->PinName))
                    {
                        continue;
                    }
                    TArray<int32> Readers;
                    bool bHasByRefReader = false;
                    bool bAnyDistant = false;
                    for (int32 Link = 0; Link < Problem.Links.Num(); ++Link)
                    {
                        const FLink& Wire = Problem.Links[Link];
                        if (Wire.bExec || Wire.From != Source || Wire.FromPin != PinAt) { continue; }
                        if (Wires[Link].Value && Wires[Link].Value->PinType.bIsReference) { bHasByRefReader = true; break; }
                        Readers.Add(Link);
                        if (Problem.Free[Wire.To])
                        {
                            const FVector2D Span = Solution.Pos[Wire.To] - Solution.Pos[Source];
                            bAnyDistant |= Span.Size() >= Params.MaxSharedGetSpan || FMath::Abs(Span.Y) >= Params.RowGap * 2.5f;
                        }
                    }
                    if (bHasByRefReader || !bAnyDistant || Readers.Num() == 0) { continue; }
                    FShape ParamShape;
                    if (!MeasureLocalParamShape(Graph, Context, ParamPin->PinName, ParamPin->PinType, ParamShape)) { continue; }
                    PlanReaders(ECopyKind::FunctionParam, Source, PinAt, ParamPin->PinName, Readers, INDEX_NONE, ParamShape, NoTarget);
                }
                continue;
            }

            bool bCheckByRefReaders = false;
            FCopyTargetPlan TargetPlan;
            if (IsSimplePureGetterNode(Source, bCheckByRefReaders))
            {
                // Standalone pure member/local getter or Self node.
            }
            else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(SourceNode))
            {
                // Lightweight zero-argument native const BlueprintPure member getter (e.g. Spline -> GetSplineLength).
                if (Call->GetClass() != UK2Node_CallFunction::StaticClass() || !Call->IsNodePure() ||
                    Pins[Source].ExecIn || Pins[Source].ExecOut.Num() > 0 ||
                    Pins[Source].DataOut.Num() != 1 || Pins[Source].DataOut[0]->ParentPin || Pins[Source].DataOut[0]->SubPins.Num() > 0)
                {
                    continue;
                }
                const UFunction* Function = Call->GetTargetFunction();
                if (!Function || Call->IsLatentFunction() ||
                    !Function->HasAllFunctionFlags(FUNC_Native | FUNC_Const) ||
                    Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Event | FUNC_BlueprintEvent | FUNC_Net))
                {
                    continue;
                }
                UEdGraphPin* SelfPin = nullptr;
                bool bHasExtraInput = false;
                for (UEdGraphPin* Input : Call->Pins)
                {
                    if (Input->Direction != EGPD_Input) { continue; }
                    if (Input->PinName == UEdGraphSchema_K2::PN_Self) { SelfPin = Input; continue; }
                    if (IsShown(*Call, Input) || Input->LinkedTo.Num() > 0) { bHasExtraInput = true; break; }
                }
                if (bHasExtraInput) { continue; }
                if (SelfPin && SelfPin->LinkedTo.Num() > 0)
                {
                    if (SelfPin->LinkedTo.Num() != 1) { continue; }
                    const int32 TargetToPin = Pins[Source].DataIn.IndexOfByKey(SelfPin);
                    if (TargetToPin == INDEX_NONE) { continue; }
                    UEdGraphPin* TargetOutPin = SelfPin->LinkedTo[0];
                    const int32* TargetIdx = TargetOutPin ? Index.Find(TargetOutPin->GetOwningNode()) : nullptr;
                    if (!TargetIdx || *TargetIdx >= OriginalNodes) { continue; }
                    bool bTargetLocal = false;
                    if (IsSimplePureGetterNode(*TargetIdx, bTargetLocal))
                    {
                        TargetPlan.bValid = true;
                        TargetPlan.Kind = ECopyKind::DuplicateNode;
                        TargetPlan.Source = *TargetIdx;
                        TargetPlan.ToPin = TargetToPin;
                        TargetPlan.Shape = Problem.Shapes[*TargetIdx];
                    }
                    else if (TopGraph == Graph && Nodes[*TargetIdx]->GetClass() == UK2Node_FunctionEntry::StaticClass() &&
                             !TargetOutPin->ParentPin && TargetOutPin->SubPins.Num() == 0 &&
                             !TargetOutPin->PinType.bIsReference && !MutatedLocals.Contains(TargetOutPin->PinName))
                    {
                        FShape ParamShape;
                        if (!MeasureLocalParamShape(Graph, Context, TargetOutPin->PinName, TargetOutPin->PinType, ParamShape)) { continue; }
                        TargetPlan.bValid = true;
                        TargetPlan.Kind = ECopyKind::FunctionParam;
                        TargetPlan.Source = *TargetIdx;
                        TargetPlan.ParamName = TargetOutPin->PinName;
                        TargetPlan.ToPin = TargetToPin;
                        TargetPlan.Shape = MoveTemp(ParamShape);
                    }
                    else
                    {
                        continue;
                    }
                }
                else if (SelfPin && SelfPin->DefaultObject != nullptr)
                {
                    continue;
                }
            }
            else
            {
                continue;
            }

            TArray<int32> Readers;
            int32 Keep = INDEX_NONE;
            double Nearest = TNumericLimits<double>::Max();
            bool bHasByRefReader = false;
            for (int32 Link = 0; Link < Problem.Links.Num(); ++Link)
            {
                const FLink& Wire = Problem.Links[Link];
                if (Wire.bExec || Wire.From != Source) { continue; }
                if (bCheckByRefReaders && Wires[Link].Value && Wires[Link].Value->PinType.bIsReference) { bHasByRefReader = true; break; }
                Readers.Add(Link);
                const double Distance = (Solution.Pos[Wire.To] - Solution.Pos[Source]).SizeSquared();
                if (Distance < Nearest) { Nearest = Distance; Keep = Link; }
            }
            if (bHasByRefReader || Readers.Num() < 2) { continue; }
            const FShape SourceShape = Problem.Shapes[Source];
            PlanReaders(ECopyKind::DuplicateNode, Source, 0, NAME_None, Readers, Keep, SourceShape, TargetPlan);
        }
        if (GetterCopies.Num() > 0) { Solution = Solve(Problem, Params); }
    }
    const auto RouteKey = [&](const FRowBreak& Break, bool bLast, const FVector2D& Centre)
    {
        const int32 RX = FMath::RoundToInt(Centre.X - 16.0);
        const int32 RY = FMath::RoundToInt(Centre.Y - 12.0);
        if (bLast)
        {
            return FString::Printf(TEXT("end:%p:%d:%d"), Wires[Break.Link].Value, RX, RY);
        }
        const bool bExec = Wires[Break.Link].Key &&
            Wires[Break.Link].Key->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
        if (bExec)
        {
            return FString::Printf(TEXT("exec:%p:%p:%d:%d"),
                Wires[Break.Link].Key->GetOwningNode(), Wires[Break.Link].Value, RX, RY);
        }
        return FString::Printf(TEXT("data:%p:%d:%d"), Wires[Break.Link].Key, RX, RY);
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
                        *OutFailure = FString::Printf(
                            TEXT("A planned reroute at (%.0f,%.0f..%.0f,%.0f) overlaps (%.0f,%.0f..%.0f,%.0f)."),
                            KnotBox.Min.X, KnotBox.Min.Y, KnotBox.Max.X, KnotBox.Max.Y,
                            Box.Min.X, Box.Min.Y, Box.Max.X, Box.Max.Y);
                        return NoneBottom;
                    }
                }
                Boxes.Add(KnotBox);
            }
        }
    }
    if (Params.bPreviewOnly)
    {
        float PreviewBottom = -TNumericLimits<float>::Max();
        for (int32 Node = 0; Node < OriginalNodes; ++Node)
        {
            const int32 NewX = FMath::RoundToInt(Solution.Pos[Node].X);
            const int32 NewY = FMath::RoundToInt(Solution.Pos[Node].Y);
            if (Problem.Free[Node])
            {
                if (OutPreviewPositions)
                {
                    OutPreviewPositions->Add(Nodes[Node], FIntPoint(NewX, NewY));
                }
                PreviewBottom = FMath::Max(PreviewBottom, static_cast<float>(Solution.Pos[Node].Y + Problem.Shapes[Node].Height));
            }
        }
        return PreviewBottom > -TNumericLimits<float>::Max() ? PreviewBottom : NoneBottom;
    }
    // Commit the validated localized copies and rewire only their planned consumers. DuplicateObject
    // preserves the exact FMemberReference and pin types/defaults. Clear cloned link arrays directly:
    // BreakAllNodeLinks on a fresh duplicate could otherwise touch the original neighbours.
    if (GetterCopies.Num() > 0) { Graph->Modify(); }
    const auto MaterializeCopy = [&](const ECopyKind Kind, const int32 SourceIndex, const FName ParamName) -> UK2Node*
    {
        UK2Node* SourceNode = CastChecked<UK2Node>(Nodes[SourceIndex]);
        SourceNode->Modify();
        if (Kind == ECopyKind::DuplicateNode)
        {
            UK2Node* Dup = DuplicateObject<UK2Node>(SourceNode, Graph, MakeUniqueObjectName(Graph, SourceNode->GetClass(), SourceNode->GetFName()));
            Dup->SetFlags(RF_Transactional);
            Dup->CreateNewGuid();
            for (UEdGraphPin* Pin : Dup->Pins) { Pin->LinkedTo.Reset(); Pin->PinId = FGuid::NewGuid(); }
            Graph->AddNode(Dup, false, false);
            return Dup;
        }
        UK2Node_VariableGet* ParamGetter = NewObject<UK2Node_VariableGet>(Graph, MakeUniqueObjectName(Graph, UK2Node_VariableGet::StaticClass(), TEXT("K2Node_VariableGet")));
        ParamGetter->SetFlags(RF_Transactional);
        ParamGetter->CreateNewGuid();
        ParamGetter->VariableReference.SetLocalMember(ParamName, Graph->GetName(), FGuid());
        Graph->AddNode(ParamGetter, false, false);
        ParamGetter->AllocateDefaultPins();
        return ParamGetter;
    };
    for (const FGetterCopy& Plan : GetterCopies)
    {
        UK2Node* Copy = MaterializeCopy(Plan.Kind, Plan.Source, Plan.ParamName);
        Nodes[Plan.Copy] = Copy;
        Pins[Plan.Copy] = FPins();
        CollectPins(*Copy, Pins[Plan.Copy]);
        if (Plan.TargetCopy != INDEX_NONE)
        {
            UK2Node* TargetCopy = MaterializeCopy(Plan.TargetKind, Plan.TargetSource, Plan.TargetParamName);
            Nodes[Plan.TargetCopy] = TargetCopy;
            Pins[Plan.TargetCopy] = FPins();
            CollectPins(*TargetCopy, Pins[Plan.TargetCopy]);
            UEdGraphPin* TargetOut = Pins[Plan.TargetCopy].DataOut.Num() == 1 ? Pins[Plan.TargetCopy].DataOut[0] : nullptr;
            UEdGraphPin* CopySelf = Copy->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
            check(TargetOut && CopySelf);
            TargetOut->MakeLinkTo(CopySelf);
            if (Plan.TargetWireLink != INDEX_NONE)
            {
                Wires[Plan.TargetWireLink] = TPair<UEdGraphPin*, UEdGraphPin*>(TargetOut, CopySelf);
            }
            if (OutAdded) { OutAdded->Add(TargetCopy); }
        }
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
            const int32 NewX = FMath::RoundToInt(Solution.Pos[Node].X);
            const int32 NewY = FMath::RoundToInt(Solution.Pos[Node].Y);
            if (Nodes[Node]->NodePosX != NewX || Nodes[Node]->NodePosY != NewY)
            {
                Nodes[Node]->Modify();
                Nodes[Node]->NodePosX = NewX;
                Nodes[Node]->NodePosY = NewY;
            }
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
        if (bNewStart || bNewEnd) { Schema->TryCreateConnection(A->GetOutputPin(), B->GetInputPin()); }
        if (bNewEnd) { Schema->TryCreateConnection(B->GetOutputPin(), Target); }
        if (OutAdded)
        {
            if (bNewStart) { OutAdded->Add(A); }
            if (bNewEnd) { OutAdded->Add(B); }
        }
    }
    if (bUseCommentPartition)
    {
        for (const TPair<UEdGraphNode_Comment*, FBox2D>& Entry : PartitionedCommentRects)
        {
            UEdGraphNode_Comment* Comment = Entry.Key;
            if (!Comment || !Entry.Value.bIsValid)
            {
                continue;
            }
            const int32 NewX = FMath::RoundToInt(Entry.Value.Min.X);
            const int32 NewY = FMath::RoundToInt(Entry.Value.Min.Y);
            const int32 NewW = FMath::Max(64, FMath::RoundToInt(Entry.Value.Max.X - Entry.Value.Min.X));
            const int32 NewH = FMath::Max(48, FMath::RoundToInt(Entry.Value.Max.Y - Entry.Value.Min.Y));
            if (Comment->NodePosX != NewX || Comment->NodePosY != NewY ||
                Comment->NodeWidth != NewW || Comment->NodeHeight != NewH)
            {
                Comment->Modify();
                Comment->NodePosX = NewX;
                Comment->NodePosY = NewY;
                Comment->NodeWidth = NewW;
                Comment->NodeHeight = NewH;
                if (OutUpdatedComments)
                {
                    OutUpdatedComments->Add(Comment);
                }
            }
        }
    }
    else if (CommentGroups.Num() > 0)
    {
        TMap<const UEdGraphNode*, FBox2D> FinalBoxes;
        for (int32 Node = 0; Node < Nodes.Num(); ++Node)
        {
            if (Nodes[Node])
            {
                const FVector2D At(Nodes[Node]->NodePosX, Nodes[Node]->NodePosY);
                FinalBoxes.Add(Nodes[Node], FBox2D(At, At + FVector2D(Problem.Shapes[Node].Width, Problem.Shapes[Node].Height)));
            }
        }
        TArray<UEdGraphNode_Comment*> UpdatedComments;
        McpGraphLayoutDiag::SyncAndRefitComments(CommentGroups, OriginalPositions, FinalBoxes,
                                                 /*bAllowShrinkOnFullReflow=*/false, &UpdatedComments);
        if (OutUpdatedComments)
        {
            for (UEdGraphNode_Comment* Comment : UpdatedComments)
            {
                OutUpdatedComments->Add(Comment);
            }
        }
    }
    return Bottom > -TNumericLimits<float>::Max() ? Bottom : NoneBottom;
}
} // namespace McpRuled
