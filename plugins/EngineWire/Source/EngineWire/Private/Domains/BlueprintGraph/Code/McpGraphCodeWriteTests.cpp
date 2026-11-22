#include "Domains/BlueprintGraph/Code/McpGraphCodeWrite.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Foundation/GraphLayout/McpRuled.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"

namespace McpGraphCode
{
namespace WriteTestParts
{
UBlueprint* MakeActorBlueprint(const TCHAR* Name)
{
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), Name),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    return Blueprint;
}

UEdGraph* EventGraphOf(UBlueprint* Blueprint)
{
    return Blueprint && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
}

UK2Node_Event* FindEventByFunction(UEdGraph* Graph, const TCHAR* MemberName)
{
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
        {
            if (Event->EventReference.GetMemberName() == FName(MemberName))
            {
                return Event;
            }
        }
    }
    return nullptr;
}

UEdGraphNode* NextExecNode(UEdGraphNode* Node)
{
    UEdGraphPin* Out = Write::ExecPin(Node, false, FString());
    return Out && Out->LinkedTo.Num() > 0 ? Out->LinkedTo[0]->GetOwningNode() : nullptr;
}

bool WriteCode(UBlueprint* Blueprint, UEdGraph* Graph, const FString& Code, TMap<UEdGraphNode*, int32>& OutMade,
               TArray<TSharedPtr<FJsonValue>>& OutRows, int32& OutKept, FString& OutError)
{
    TArray<FSExpr> Forms;
    if (!ParseSExpr(Code, Forms, OutError) || Forms.Num() == 0)
    {
        return false;
    }
    Write::FGraphPlan Plan;
    Plan.Graph = Graph;
    for (int32 At = 0; At < Forms.Num(); ++At)
    {
        const FString Check = Write::CheckForm(Forms[At]);
        if (!Check.IsEmpty())
        {
            OutError = Check;
            return false;
        }
        Plan.Forms.Add(At);
    }
    if (!Write::PlanGraph(Forms, Plan, OutError))
    {
        return false;
    }
    const TSet<FString> Titles = Write::CodeTitles(Forms);
    const TArray<FMenuEntry> Candidates = MenuCandidates(Titles);
    if (!Write::BuildForms(Blueprint, Forms, Candidates, Titles, Plan, OutMade, OutRows, OutError))
    {
        return false;
    }
    OutKept = Plan.Kept;
    for (UEdGraphNode* Old : Plan.Old)
    {
        FBlueprintEditorUtils::RemoveNode(Blueprint, Old, /*bDontRecompile=*/true);
    }
    return true;
}

int32 CountOverlaps(UEdGraph* Graph)
{
    TArray<FBox2D> Boxes;
    int32 Overlaps = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || Node->IsA<UEdGraphNode_Comment>())
        {
            continue;
        }
        const McpRuled::FShape Shape = McpRuled::EstimateShape(*Node);
        const FBox2D Box(FVector2D(Node->NodePosX, Node->NodePosY),
                         FVector2D(Node->NodePosX + Shape.Width, Node->NodePosY + Shape.Height));
        for (const FBox2D& Other : Boxes)
        {
            if (Box.Min.X < Other.Max.X && Other.Min.X < Box.Max.X && Box.Min.Y < Other.Max.Y && Other.Min.Y < Box.Max.Y)
            {
                ++Overlaps;
            }
        }
        Boxes.Add(Box);
    }
    return Overlaps;
}

double ExecLineY(const UEdGraphNode& Node)
{
    const McpRuled::FShape Shape = McpRuled::EstimateShape(Node);
    const float PinOffset = Shape.ExecIn >= 0.0f ? Shape.ExecIn : (Shape.ExecOut.Num() > 0 ? Shape.ExecOut[0] : Shape.Height * 0.5f);
    return static_cast<double>(Node.NodePosY) + static_cast<double>(PinOffset);
}

FString ShortTag(const UEdGraphNode* Node)
{
    return TEXT("n") + Node->NodeGuid.ToString().Left(6).ToLower();
}
} // namespace WriteTestParts
} // namespace McpGraphCode

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpGraphCodeWriteLayoutTest, "EngineWire.BlueprintGraph.Code.WriteLayout",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpGraphCodeWriteLayoutTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace McpGraphCode;
    using namespace McpGraphCode::WriteTestParts;

    // 1. Default 3-event Actor EventGraph (BeginPlay + unused ghost ActorBeginOverlap + Tick):
    //    writing BeginPlay and Tick together must keep all three existing events in place, expand both calls
    //    horizontally level with their own event entries, and never drop them below the graph.
    {
        UBlueprint* BP = MakeActorBlueprint(TEXT("CodeWrite_DefaultThreeEvents"));
        UEdGraph* Graph = EventGraphOf(BP);
        if (!TestNotNull(TEXT("actor blueprint has an EventGraph"), Graph)) { return false; }

        UK2Node_Event* BeginPlay = FindEventByFunction(Graph, TEXT("ReceiveBeginPlay"));
        UK2Node_Event* Overlap = FindEventByFunction(Graph, TEXT("ReceiveActorBeginOverlap"));
        UK2Node_Event* Tick = FindEventByFunction(Graph, TEXT("ReceiveTick"));
        if (!TestTrue(TEXT("default actor EventGraph has BeginPlay, Overlap, and Tick"), BeginPlay && Overlap && Tick))
        {
            return false;
        }
        const FIntPoint BeginPos(BeginPlay->NodePosX, BeginPlay->NodePosY);
        const FIntPoint OverlapPos(Overlap->NodePosX, Overlap->NodePosY);
        const FIntPoint TickPos(Tick->NodePosX, Tick->NodePosY);

        TMap<UEdGraphNode*, int32> Made;
        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 Kept = 0;
        FString Error;
        const FString Code = TEXT(
            "(event EventBeginPlay\n"
            "  (Development|PrintString \"begin\"))\n"
            "(event EventTick (DeltaSeconds)\n"
            "  (Development|PrintString \"tick\"))\n");
        if (!TestTrue(*FString::Printf(TEXT("default 3-event write succeeds (%s)"), *Error),
                      WriteCode(BP, Graph, Code, Made, Rows, Kept, Error)))
        {
            return false;
        }
        TestEqual(TEXT("BeginPlay kept its original position"), FIntPoint(BeginPlay->NodePosX, BeginPlay->NodePosY), BeginPos);
        TestEqual(TEXT("unused ActorBeginOverlap kept its original position"), FIntPoint(Overlap->NodePosX, Overlap->NodePosY), OverlapPos);
        TestEqual(TEXT("Tick kept its original position"), FIntPoint(Tick->NodePosX, Tick->NodePosY), TickPos);

        UEdGraphNode* BeginCall = NextExecNode(BeginPlay);
        UEdGraphNode* TickCall = NextExecNode(Tick);
        if (TestTrue(TEXT("both events wired to their calls"), BeginCall && TickCall))
        {
            TestTrue(TEXT("BeginPlay call sits right of BeginPlay"), BeginCall->NodePosX > BeginPlay->NodePosX);
            TestTrue(TEXT("BeginPlay white exec line is horizontal"), FMath::IsNearlyEqual(ExecLineY(*BeginCall), ExecLineY(*BeginPlay), 1.5));
            TestTrue(TEXT("Tick call sits right of Tick"), TickCall->NodePosX > Tick->NodePosX);
            TestTrue(TEXT("Tick white exec line is horizontal"), FMath::IsNearlyEqual(ExecLineY(*TickCall), ExecLineY(*Tick), 1.5));
        }
        TestEqual(TEXT("no overlapping nodes in default 3-event graph"), CountOverlaps(Graph), 0);
    }

    // 2. Existing event + newly created custom event in one write: unplaced custom event nodes must not block
    //    Pass 0, existing events stay in place, and the new custom event starts cleanly below existing events.
    {
        UBlueprint* BP = MakeActorBlueprint(TEXT("CodeWrite_ExistingPlusNewEvent"));
        UEdGraph* Graph = EventGraphOf(BP);
        UK2Node_Event* BeginPlay = FindEventByFunction(Graph, TEXT("ReceiveBeginPlay"));
        UK2Node_Event* Overlap = FindEventByFunction(Graph, TEXT("ReceiveActorBeginOverlap"));
        UK2Node_Event* Tick = FindEventByFunction(Graph, TEXT("ReceiveTick"));
        const FIntPoint BeginPos(BeginPlay->NodePosX, BeginPlay->NodePosY);
        const FIntPoint OverlapPos(Overlap->NodePosX, Overlap->NodePosY);
        const FIntPoint TickPos(Tick->NodePosX, Tick->NodePosY);

        TMap<UEdGraphNode*, int32> Made;
        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 Kept = 0;
        FString Error;
        const FString Code = TEXT(
            "(event EventBeginPlay\n"
            "  (Development|PrintString \"existing\"))\n"
            "(event Custom|AddedEvent\n"
            "  (Development|PrintString \"added\"))\n");
        if (!TestTrue(*FString::Printf(TEXT("existing + new event write succeeds (%s)"), *Error),
                      WriteCode(BP, Graph, Code, Made, Rows, Kept, Error)))
        {
            return false;
        }
        TestEqual(TEXT("BeginPlay stays put"), FIntPoint(BeginPlay->NodePosX, BeginPlay->NodePosY), BeginPos);
        TestEqual(TEXT("Overlap stays put"), FIntPoint(Overlap->NodePosX, Overlap->NodePosY), OverlapPos);
        TestEqual(TEXT("Tick stays put"), FIntPoint(Tick->NodePosX, Tick->NodePosY), TickPos);

        UEdGraphNode* BeginCall = NextExecNode(BeginPlay);
        if (TestNotNull(TEXT("BeginPlay call created"), BeginCall))
        {
            TestTrue(TEXT("BeginPlay call stays horizontal on BeginPlay's line"),
                     BeginCall->NodePosX > BeginPlay->NodePosX && FMath::IsNearlyEqual(ExecLineY(*BeginCall), ExecLineY(*BeginPlay), 1.5));
        }

        UEdGraphNode* CustomEntry = nullptr;
        for (const TPair<UEdGraphNode*, int32>& Each : Made)
        {
            if (Each.Key->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TEXT("AddedEvent")))
            {
                CustomEntry = Each.Key;
                break;
            }
        }
        if (TestNotNull(TEXT("new custom event created"), CustomEntry))
        {
            TestTrue(TEXT("new custom event is placed below existing events"), CustomEntry->NodePosY > Tick->NodePosY);
            UEdGraphNode* CustomCall = NextExecNode(CustomEntry);
            if (TestNotNull(TEXT("new custom event call created"), CustomCall))
            {
                TestTrue(TEXT("new custom event call is right and level"),
                         CustomCall->NodePosX > CustomEntry->NodePosX && FMath::IsNearlyEqual(ExecLineY(*CustomCall), ExecLineY(*CustomEntry), 1.5));
            }
        }
        TestEqual(TEXT("no overlapping nodes in existing + new event graph"), CountOverlaps(Graph), 0);
    }

    // 3. Multiple newly created custom events in one write: later unplaced events must not poison FreeSpot
    //    or block earlier new events at (0, 0).
    {
        UBlueprint* BP = MakeActorBlueprint(TEXT("CodeWrite_MultipleNewEvents"));
        UEdGraph* Graph = EventGraphOf(BP);
        UK2Node_Event* Tick = FindEventByFunction(Graph, TEXT("ReceiveTick"));

        TMap<UEdGraphNode*, int32> Made;
        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 Kept = 0;
        FString Error;
        const FString Code = TEXT(
            "(event Custom|FirstAdded\n"
            "  (Development|PrintString \"one\"))\n"
            "(event Custom|SecondAdded\n"
            "  (Development|PrintString \"two\"))\n");
        if (!TestTrue(*FString::Printf(TEXT("multiple new events write succeeds (%s)"), *Error),
                      WriteCode(BP, Graph, Code, Made, Rows, Kept, Error)))
        {
            return false;
        }
        UEdGraphNode* FirstEntry = nullptr;
        UEdGraphNode* SecondEntry = nullptr;
        for (const TPair<UEdGraphNode*, int32>& Each : Made)
        {
            const FString Title = Each.Key->GetNodeTitle(ENodeTitleType::ListView).ToString();
            if (Title.Contains(TEXT("FirstAdded"))) { FirstEntry = Each.Key; }
            else if (Title.Contains(TEXT("SecondAdded"))) { SecondEntry = Each.Key; }
        }
        if (TestTrue(TEXT("both new custom events created"), FirstEntry && SecondEntry))
        {
            TestTrue(TEXT("first custom event below Tick"), FirstEntry->NodePosY > Tick->NodePosY);
            TestTrue(TEXT("second custom event below first"), SecondEntry->NodePosY > FirstEntry->NodePosY);
            UEdGraphNode* FirstCall = NextExecNode(FirstEntry);
            UEdGraphNode* SecondCall = NextExecNode(SecondEntry);
            if (TestTrue(TEXT("both custom events wired to their calls"), FirstCall && SecondCall))
            {
                TestTrue(TEXT("first custom event row is level"),
                         FirstCall->NodePosX > FirstEntry->NodePosX && FMath::IsNearlyEqual(ExecLineY(*FirstCall), ExecLineY(*FirstEntry), 1.5));
                TestTrue(TEXT("second custom event row is level"),
                         SecondCall->NodePosX > SecondEntry->NodePosX && FMath::IsNearlyEqual(ExecLineY(*SecondCall), ExecLineY(*SecondEntry), 1.5));
            }
        }
        TestEqual(TEXT("no overlapping nodes in multiple new events graph"), CountOverlaps(Graph), 0);
    }

    // 4. Inserting into an existing kept execution chain (A -> B becomes A -> MID -> B using ; id tags):
    //    bAllowMovingExisting remains enabled for edits with kept chain nodes so B shifts right to make room.
    {
        UBlueprint* BP = MakeActorBlueprint(TEXT("CodeWrite_InsertIntoKeptChain"));
        UEdGraph* Graph = EventGraphOf(BP);
        UK2Node_Event* BeginPlay = FindEventByFunction(Graph, TEXT("ReceiveBeginPlay"));

        TMap<UEdGraphNode*, int32> InitialMade;
        TArray<TSharedPtr<FJsonValue>> InitialRows;
        int32 InitialKept = 0;
        FString Error;
        const FString InitialCode = TEXT(
            "(event EventBeginPlay\n"
            "  (Development|PrintString \"A\")\n"
            "  (Development|PrintString \"B\"))\n");
        if (!TestTrue(*FString::Printf(TEXT("initial kept-chain write succeeds (%s)"), *Error),
                      WriteCode(BP, Graph, InitialCode, InitialMade, InitialRows, InitialKept, Error)))
        {
            return false;
        }
        UEdGraphNode* NodeA = NextExecNode(BeginPlay);
        UEdGraphNode* NodeB = NodeA ? NextExecNode(NodeA) : nullptr;
        if (!TestTrue(TEXT("initial chain has A -> B"), NodeA && NodeB)) { return false; }

        const FIntPoint PosA(NodeA->NodePosX, NodeA->NodePosY);
        const FIntPoint PosB(NodeB->NodePosX, NodeB->NodePosY);
        const FString EditCode = FString::Printf(
            TEXT("(event EventBeginPlay\n"
                 "  (Development|PrintString \"A\")  ; %s\n"
                 "  (Development|PrintString \"MID\")\n"
                 "  (Development|PrintString \"B\"))  ; %s\n"),
            *ShortTag(NodeA), *ShortTag(NodeB));

        TMap<UEdGraphNode*, int32> EditMade;
        TArray<TSharedPtr<FJsonValue>> EditRows;
        int32 EditKept = 0;
        if (!TestTrue(*FString::Printf(TEXT("kept-chain insert succeeds (%s)"), *Error),
                      WriteCode(BP, Graph, EditCode, EditMade, EditRows, EditKept, Error)))
        {
            return false;
        }
        TestEqual(TEXT("A and B were kept"), EditKept, 2);
        TestEqual(TEXT("A stayed at its original position"), FIntPoint(NodeA->NodePosX, NodeA->NodePosY), PosA);
        UEdGraphNode* NodeMid = NextExecNode(NodeA);
        if (TestTrue(TEXT("MID inserted between A and B"), NodeMid && NodeMid != NodeB && NextExecNode(NodeMid) == NodeB))
        {
            TestTrue(TEXT("MID sits between A and B horizontally"), NodeMid->NodePosX > NodeA->NodePosX && NodeMid->NodePosX < NodeB->NodePosX);
            TestTrue(TEXT("MID stays level with A"), FMath::IsNearlyEqual(ExecLineY(*NodeMid), ExecLineY(*NodeA), 1.5));
            TestTrue(TEXT("B shifted right to make room for MID"), NodeB->NodePosX > PosB.X);
        }
        TestEqual(TEXT("no overlapping nodes after kept-chain insert"), CountOverlaps(Graph), 0);
    }

    // 5. Auto-inserted schema conversion nodes (int -> double) in FTranspiler::Connect must be captured in Made,
    //    counted in FormRows, laid out off (0, 0), and leave zero overlaps with FunctionEntry.
    {
        UBlueprint* BP = MakeActorBlueprint(TEXT("CodeWrite_AutoConversionNode"));
        UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
            BP, TEXT("TestConvert"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, true, static_cast<UClass*>(nullptr));

        UK2Node_FunctionEntry* Entry = nullptr;
        for (UEdGraphNode* Node : FuncGraph->Nodes)
        {
            if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node))
            {
                Entry = AsEntry;
                break;
            }
        }
        if (!TestNotNull(TEXT("function graph has entry node"), Entry)) { return false; }

        FEdGraphPinType IntType;
        IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
        Entry->CreateUserDefinedPin(TEXT("InInt"), IntType, EGPD_Output);

        FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*FuncGraph);
        UK2Node_FunctionResult* ResultNode = ResultCreator.CreateNode(false);
        ResultNode->NodePosX = 480;
        ResultNode->NodePosY = 0;
        ResultCreator.Finalize();
        FEdGraphPinType DoubleType;
        DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
        DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
        ResultNode->CreateUserDefinedPin(TEXT("OutDouble"), DoubleType, EGPD_Input);
        FKismetEditorUtilities::CompileBlueprint(BP);

        TMap<UEdGraphNode*, int32> Made;
        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 Kept = 0;
        FString Error;
        const FString Code = TEXT(
            "(fn TestConvert (InInt)\n"
            "  (return (Math|Float|float*float InInt 0.5)))\n");
        if (!TestTrue(*FString::Printf(TEXT("conversion function write succeeds (%s)"), *Error),
                      WriteCode(BP, FuncGraph, Code, Made, Rows, Kept, Error)))
        {
            return false;
        }

        UK2Node_CallFunction* ConvNode = nullptr;
        for (UEdGraphNode* Node : FuncGraph->Nodes)
        {
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
            {
                if (Call->GetFunctionName() == FName(TEXT("Conv_IntToDouble")) ||
                    Call->GetFunctionName() == FName(TEXT("Conv_IntToFloat")))
                {
                    ConvNode = Call;
                    break;
                }
            }
        }
        if (TestNotNull(TEXT("schema auto-inserted an int-to-float/double conversion node"), ConvNode))
        {
            TestTrue(TEXT("auto-inserted conversion node was recorded in Made"), Made.Contains(ConvNode));
            TestTrue(TEXT("conversion node was laid out off (0, 0) where FunctionEntry sits"),
                     ConvNode->NodePosX != Entry->NodePosX || ConvNode->NodePosY != Entry->NodePosY);
        }
        if (TestEqual(TEXT("one form row returned"), Rows.Num(), 1))
        {
            const int32 ReportedNodes = static_cast<int32>(Rows[0]->AsObject()->GetNumberField(TEXT("nodes")));
            TestEqual(TEXT("form row node count includes auto-inserted conversion node"), ReportedNodes, Made.Num());
        }
        TestEqual(TEXT("no overlapping nodes in conversion function graph"), CountOverlaps(FuncGraph), 0);
    }

    return true;
}
#endif
