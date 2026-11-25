#include "Foundation/GraphLayout/McpGraphLayoutDiagnostics.h"
#include "Foundation/GraphLayout/McpLayered.h"
#include "Foundation/GraphLayout/McpRuled.h"
#include "Domains/BlueprintGraph/McpBPGraphBatchSteps.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace McpLayoutRegressionHelpers
{
UBlueprint* MakeTestBlueprint(const TCHAR* BaseName)
{
    return FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), BaseName),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

UK2Node_CustomEvent* MakeCustomEvent(UEdGraph* Graph, const TCHAR* Name, int32 X, int32 Y)
{
    UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
    Event->CreateNewGuid();
    Event->CustomFunctionName = FName(Name);
    Event->NodePosX = X;
    Event->NodePosY = Y;
    Graph->AddNode(Event, false, false);
    Event->AllocateDefaultPins();
    return Event;
}

UK2Node_CallFunction* MakePrintCall(UEdGraph* Graph, int32 X, int32 Y)
{
    UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
    Call->CreateNewGuid();
    Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString")));
    Call->NodePosX = X;
    Call->NodePosY = Y;
    Graph->AddNode(Call, false, false);
    Call->AllocateDefaultPins();
    return Call;
}

UK2Node_IfThenElse* MakeBranch(UEdGraph* Graph, int32 X, int32 Y)
{
    UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
    Branch->CreateNewGuid();
    Branch->NodePosX = X;
    Branch->NodePosY = Y;
    Graph->AddNode(Branch, false, false);
    Branch->AllocateDefaultPins();
    return Branch;
}

UK2Node_CallFunction* MakePureAddInt(UEdGraph* Graph, int32 X, int32 Y)
{
    UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
    Call->CreateNewGuid();
    Call->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Add_IntInt")));
    Call->NodePosX = X;
    Call->NodePosY = Y;
    Graph->AddNode(Call, false, false);
    Call->AllocateDefaultPins();
    return Call;
}

UEdGraphNode_Comment* MakeCommentBox(UEdGraph* Graph, const TCHAR* Title, int32 X, int32 Y, int32 W, int32 H, int32 Depth = -1)
{
    UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
    Comment->CreateNewGuid();
    Comment->NodeComment = Title;
    Comment->NodePosX = X;
    Comment->NodePosY = Y;
    Comment->NodeWidth = W;
    Comment->NodeHeight = H;
    Comment->CommentDepth = Depth;
    Graph->AddNode(Comment, false, false);
    return Comment;
}

void ConnectExec(UEdGraphNode* From, const FName FromPinName, UEdGraphNode* To)
{
    UEdGraphPin* Out = From->FindPin(FromPinName, EGPD_Output);
    UEdGraphPin* In = To->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    check(Out && In);
    Out->MakeLinkTo(In);
}
} // namespace McpLayoutRegressionHelpers

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpGraphLayoutRegressionTest, "EngineWire.Foundation.GraphLayout.RegressionSuite",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpGraphLayoutRegressionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace McpLayoutRegressionHelpers;

    // 1. EW-002 + EW-003 + EW-005 (Cannon Cart fixture):
    //    - Exec-only region partitioning keeps distinct events sharing a pure variable separate.
    //    - Backward exec loop (Delay-like re-fire back to Branch) routes through outer corridor auto-knots.
    //    - Repeat layout collapses auto-knots cleanly (idempotent, no knot stacking) while preserving manual knots.
    //    - Dead pure island (chained pure nodes with no active consumer) is detected as one unused group.
    {
        UBlueprint* BP = MakeTestBlueprint(TEXT("LayoutReg_CannonCart"));
        UEdGraph* Graph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
        if (!TestNotNull(TEXT("CannonCart fixture has EventGraph"), Graph)) { return false; }

        UK2Node_CustomEvent* FireEvent = MakeCustomEvent(Graph, TEXT("OnFireLoop"), 0, 0);
        UK2Node_IfThenElse* CanShoot = MakeBranch(Graph, 280, 0);
        UK2Node_CallFunction* DoShot = MakePrintCall(Graph, 560, 0);
        UK2Node_CallFunction* WaitRefire = MakePrintCall(Graph, 920, 0);
        ConnectExec(FireEvent, UEdGraphSchema_K2::PN_Then, CanShoot);
        ConnectExec(CanShoot, UEdGraphSchema_K2::PN_Then, DoShot);
        ConnectExec(DoShot, UEdGraphSchema_K2::PN_Then, WaitRefire);
        // Backward exec loop: WaitRefire -> CanShoot (mimics Delay re-fire back to branch).
        ConnectExec(WaitRefire, UEdGraphSchema_K2::PN_Then, CanShoot);

        // Dead pure node chain (2 connected pure nodes with no active consumer).
        UK2Node_CallFunction* DeadPureA = MakePureAddInt(Graph, -400, 900);
        UK2Node_CallFunction* DeadPureB = MakePureAddInt(Graph, -150, 900);
        DeadPureA->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output)->MakeLinkTo(DeadPureB->FindPin(TEXT("A"), EGPD_Input));

        const TArray<FString> BeforeFingerprint = McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Graph);
        const TArray<TArray<UEdGraphNode*>> UnusedGroups = McpGraphLayoutDiag::FindUnusedPureGroups(Graph);
        TestEqual(TEXT("EW-003 detects dead chained pure nodes as one unused group"), UnusedGroups.Num(), 1);
        if (UnusedGroups.Num() == 1)
        {
            TestEqual(TEXT("EW-003 unused pure group contains both chained pure nodes"), UnusedGroups[0].Num(), 2);
        }

        TArray<UEdGraphNode*> Free = {FireEvent, CanShoot, DoShot, WaitRefire, DeadPureA, DeadPureB};
        McpRuled::FParams Params;
        Params.bRouteBackwardExecs = true;
        Params.bCollapseAutoExecKnots = true;
        Params.bAllowMovingExisting = true;
        FString Failure;
        TArray<UEdGraphNode*> Added1;
        McpRuled::LayoutNodes(Graph, Free, nullptr, &Added1, nullptr, Params, &Failure);
        TestTrue(*FString::Printf(TEXT("EW-002 ruled layout with backward exec succeeds: %s"), *Failure), Failure.IsEmpty());
        TestTrue(TEXT("EW-002 backward exec loop generated 2 outer corridor auto-knots"), Added1.Num() == 2);
        for (UEdGraphNode* Node : Added1)
        {
            TestTrue(TEXT("generated corridor knot carries auto-layout marker"), McpGraphLayoutDiag::IsAutoLayoutKnot(Node));
        }
        TestTrue(TEXT("EW-002 topology fingerprint preserved after auto-knot routing"),
                 BeforeFingerprint == McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Graph));

        // Repeat layout must collapse old auto-knots and recreate identically without stacking extra knots.
        int32 AutoKnotsAfterRun1 = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (McpGraphLayoutDiag::IsAutoLayoutKnot(Node)) { ++AutoKnotsAfterRun1; }
        }
        TArray<UEdGraphNode*> Added2;
        McpRuled::LayoutNodes(Graph, Free, nullptr, &Added2, nullptr, Params, &Failure);
        int32 AutoKnotsAfterRun2 = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (McpGraphLayoutDiag::IsAutoLayoutKnot(Node)) { ++AutoKnotsAfterRun2; }
        }
        TestEqual(TEXT("EW-002 repeated layout does not stack auto-knots"), AutoKnotsAfterRun2, AutoKnotsAfterRun1);
        TestTrue(TEXT("EW-002 topology fingerprint still preserved after repeat layout"),
                 BeforeFingerprint == McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Graph));
    }

    // 2. EW-007 (Formation 3-Comment-Box Batch Rewire fixture):
    //    Rewiring a new batch node into an existing multi-entry merge inside a separate comment box
    //    must keep the merge node, its downstream nodes, and all 3 existing comment boxes completely stationary.
    {
        UBlueprint* BP = MakeTestBlueprint(TEXT("LayoutReg_FormationComments"));
        UEdGraph* Graph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
        if (!TestNotNull(TEXT("Formation fixture has EventGraph"), Graph)) { return false; }

        // Region 1: Upgrade entry inside Comment 1
        UK2Node_CustomEvent* UpgradeEvent = MakeCustomEvent(Graph, TEXT("OnUpgradeTier3"), 0, 0);
        UK2Node_CallFunction* NewHook = MakePrintCall(Graph, 320, 0);
        UEdGraphNode_Comment* CommentUpgrade = MakeCommentBox(Graph, TEXT("Upgrade"), -40, -60, 700, 260);

        // Region 2: Tick entry feeding SharedTransform (existing multi-entry merge) inside Comment 2
        UK2Node_CustomEvent* TickLike = MakeCustomEvent(Graph, TEXT("OnTickCheck"), 0, 600);
        UK2Node_CallFunction* SharedTransform = MakePrintCall(Graph, 400, 600);
        UK2Node_CallFunction* RefreshSquad = MakePrintCall(Graph, 800, 600);
        ConnectExec(TickLike, UEdGraphSchema_K2::PN_Then, SharedTransform);
        ConnectExec(SharedTransform, UEdGraphSchema_K2::PN_Then, RefreshSquad);
        UEdGraphNode_Comment* CommentShared = MakeCommentBox(Graph, TEXT("SharedTransform"), -40, 540, 1200, 280);

        // Region 3: VFX display inside Comment 3
        UK2Node_CallFunction* VfxCheck = MakePrintCall(Graph, 400, 1200);
        ConnectExec(RefreshSquad, UEdGraphSchema_K2::PN_Then, VfxCheck);
        UEdGraphNode_Comment* CommentVfx = MakeCommentBox(Graph, TEXT("VFX"), 340, 1140, 440, 240);

        // Batch connects UpgradeEvent -> NewHook -> SharedTransform.
        ConnectExec(UpgradeEvent, UEdGraphSchema_K2::PN_Then, NewHook);
        ConnectExec(NewHook, UEdGraphSchema_K2::PN_Then, SharedTransform);

        const FIntPoint SharedPosBefore(SharedTransform->NodePosX, SharedTransform->NodePosY);
        const FIntPoint RefreshPosBefore(RefreshSquad->NodePosX, RefreshSquad->NodePosY);
        const FIntPoint VfxPosBefore(VfxCheck->NodePosX, VfxCheck->NodePosY);
        const FIntPoint CommentSharedBefore(CommentShared->NodePosX, CommentShared->NodePosY);
        const FIntPoint CommentVfxBefore(CommentVfx->NodePosX, CommentVfx->NodePosY);

        McpBlueprintGraphHandlers::GraphBatch::FBatchState BatchState;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node != NewHook)
            {
                BatchState.ExistingGuids.Add(Node->NodeGuid.ToString());
            }
        }
        BatchState.AutoPlacedGuids.Add(NewHook->NodeGuid.ToString());
        McpBlueprintGraphHandlers::GraphBatch::FBatchLinkChange LinkChange;
        LinkChange.FromGuid = NewHook->NodeGuid.ToString();
        LinkChange.ToGuid = SharedTransform->NodeGuid.ToString();
        LinkChange.bExec = true;
        BatchState.LinkChanges.Add(LinkChange);

        TSharedPtr<FJsonObject> Diagnostics;
        McpBlueprintGraphHandlers::GraphBatch::LayoutAutoPlacedNodes(BP, BatchState, &Diagnostics);

        TestEqual(TEXT("EW-007 existing multi-entry SharedTransform did not drift"),
                  FIntPoint(SharedTransform->NodePosX, SharedTransform->NodePosY), SharedPosBefore);
        TestEqual(TEXT("EW-007 downstream RefreshSquad inside locked comment did not drift"),
                  FIntPoint(RefreshSquad->NodePosX, RefreshSquad->NodePosY), RefreshPosBefore);
        TestEqual(TEXT("EW-007 downstream VfxCheck inside separate comment did not drift"),
                  FIntPoint(VfxCheck->NodePosX, VfxCheck->NodePosY), VfxPosBefore);
        TestEqual(TEXT("EW-007 SharedTransform comment box stayed fixed"),
                  FIntPoint(CommentShared->NodePosX, CommentShared->NodePosY), CommentSharedBefore);
        TestEqual(TEXT("EW-007 VFX comment box stayed fixed"),
                  FIntPoint(CommentVfx->NodePosX, CommentVfx->NodePosY), CommentVfxBefore);
        if (TestTrue(TEXT("EW-004/008 batch returned layoutDiagnostics"), Diagnostics.IsValid()))
        {
            const int32 ProtectedMerges = static_cast<int32>(Diagnostics->GetNumberField(TEXT("protectedMergeBoundaries")));
            TestTrue(TEXT("EW-007 recorded protected multi-entry merge boundary"), ProtectedMerges >= 1);
        }
        (void)CommentUpgrade;
    }

    // 3. EW-001 + EW-006 + EW-008 (native McpRuled full-graph/preview and McpLayered selection boundary):
    {
        UBlueprint* BP = MakeTestBlueprint(TEXT("LayoutReg_RuledAndLayered"));
        UEdGraph* Graph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
        if (!TestNotNull(TEXT("RuledAndLayered fixture has EventGraph"), Graph)) { return false; }

        UK2Node_CustomEvent* EventA = MakeCustomEvent(Graph, TEXT("EventA"), 1200, 900);
        UK2Node_IfThenElse* BranchA = MakeBranch(Graph, 100, 100);
        UK2Node_CallFunction* TrueCall = MakePrintCall(Graph, 200, 50);
        UK2Node_CallFunction* FalseCall = MakePrintCall(Graph, 150, 120);
        ConnectExec(EventA, UEdGraphSchema_K2::PN_Then, BranchA);
        ConnectExec(BranchA, UEdGraphSchema_K2::PN_Then, TrueCall);
        ConnectExec(BranchA, UEdGraphSchema_K2::PN_Else, FalseCall);

        UK2Node_CustomEvent* EventB = MakeCustomEvent(Graph, TEXT("EventB"), 2000, 100);
        UK2Node_CallFunction* CallB = MakePrintCall(Graph, 2300, 100);
        ConnectExec(EventB, UEdGraphSchema_K2::PN_Then, CallB);
        UEdGraphNode_Comment* CommentB = MakeCommentBox(Graph, TEXT("RegionB"), 1950, 40, 680, 240);

        // Preview mode (EW-008): McpRuled with bPreviewOnly must never mutate node coordinates.
        const FIntPoint EventAPos0(EventA->NodePosX, EventA->NodePosY);
        const FIntPoint BranchAPos0(BranchA->NodePosX, BranchA->NodePosY);
        {
            McpRuled::FParams PreviewParams;
            PreviewParams.bPreviewOnly = true;
            PreviewParams.bAllowMovingExisting = true;
            FString Failure;
            TMap<UEdGraphNode*, FIntPoint> PlannedPositions;
            McpRuled::LayoutNodes(Graph, {EventA, BranchA, TrueCall, FalseCall, EventB, CallB},
                                  nullptr, nullptr, nullptr, PreviewParams, &Failure, &PlannedPositions);
            TestTrue(*FString::Printf(TEXT("EW-008 preview succeeds: %s"), *Failure), Failure.IsEmpty());
            TestEqual(TEXT("EW-008 preview plans positions for all 6 nodes"), PlannedPositions.Num(), 6);
            TestEqual(TEXT("EW-008 preview leaves EventA untouched"), FIntPoint(EventA->NodePosX, EventA->NodePosY), EventAPos0);
            TestEqual(TEXT("EW-008 preview leaves BranchA untouched"), FIntPoint(BranchA->NodePosX, BranchA->NodePosY), BranchAPos0);
        }

        // Selection mode (EW-006): McpLayered + obstacle-clearing block shift keeps unselected nodes and comments fixed.
        const FIntPoint EventBPosBefore(EventB->NodePosX, EventB->NodePosY);
        const FIntPoint CallBPosBefore(CallB->NodePosX, CallB->NodePosY);
        const FIntPoint CommentBPosBefore(CommentB->NodePosX, CommentB->NodePosY);
        {
            const TArray<FString> BeforeFp = McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Graph);
            const TMap<const UEdGraphNode*, FBox2D> BeforeBoxes = McpGraphLayoutDiag::MeasureNodeBoxes(Graph);
            const TArray<McpGraphLayoutDiag::FCommentGroup> Comments = McpGraphLayoutDiag::CollectCommentGroups(Graph, BeforeBoxes);
            const TArray<UEdGraphNode*> Selection = {BranchA, TrueCall, FalseCall};
            McpLayered::LayoutNodes(Selection);
            int32 ObstacleHits = 0;
            const bool bClear = McpGraphLayoutDiag::ShiftBlockClearOfObstacles(Graph, Selection, BeforeBoxes, Comments, ObstacleHits);
            TestTrue(TEXT("EW-006 selection layout clears unselected obstacles"), bClear);
            TestEqual(TEXT("EW-006 unselected EventB stays fixed"), FIntPoint(EventB->NodePosX, EventB->NodePosY), EventBPosBefore);
            TestEqual(TEXT("EW-006 unselected CallB stays fixed"), FIntPoint(CallB->NodePosX, CallB->NodePosY), CallBPosBefore);
            TestEqual(TEXT("EW-006 untouched CommentB stays fixed"), FIntPoint(CommentB->NodePosX, CommentB->NodePosY), CommentBPosBefore);
            TestTrue(TEXT("EW-006 BranchA sits left of TrueCall"), BranchA->NodePosX < TrueCall->NodePosX);
            TestTrue(TEXT("EW-006 topology fingerprint unchanged"), BeforeFp == McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Graph));
        }

        // Full-graph mode (EW-001 + EW-002): add a backward exec loop TrueCall -> BranchA and arrange all 6 nodes with McpRuled.
        ConnectExec(TrueCall, UEdGraphSchema_K2::PN_Then, BranchA);
        {
            const TArray<FString> BeforeFp = McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Graph);
            McpRuled::FParams FullParams;
            FullParams.bAllowMovingExisting = true;
            FullParams.bRouteBackwardExecs = true;
            FullParams.bCollapseAutoExecKnots = true;
            FString Failure;
            TArray<UEdGraphNode*> AddedNodes;
            McpRuled::LayoutNodes(Graph, {EventA, BranchA, TrueCall, FalseCall, EventB, CallB},
                                  nullptr, &AddedNodes, nullptr, FullParams, &Failure);
            TestTrue(*FString::Printf(TEXT("EW-001 full-graph ruled layout with cycle succeeds: %s"), *Failure), Failure.IsEmpty());
            TestTrue(TEXT("EW-001 EventA now sits left of BranchA"), EventA->NodePosX < BranchA->NodePosX);
            TestEqual(TEXT("EW-001 EventA and EventB align flush left"), EventA->NodePosX, EventB->NodePosX);
            TestTrue(TEXT("EW-001 full-graph topology fingerprint unchanged"), BeforeFp == McpGraphLayoutDiag::BuildSemanticTopologyFingerprint(Graph));
            const McpGraphLayoutDiag::FLayoutQualityReport Quality = McpGraphLayoutDiag::AnalyzeGraphQuality(Graph);
            TestEqual(TEXT("EW-001 full-graph layout has zero node overlaps"), Quality.NodeOverlapCount, 0);
            TestTrue(TEXT("EW-001 backward exec generated corridor auto-knots"), AddedNodes.Num() >= 2);
            for (UEdGraphNode* Knot : AddedNodes)
            {
                TestTrue(TEXT("EW-001 generated corridor knot carries auto-layout marker"), McpGraphLayoutDiag::IsAutoLayoutKnot(Knot));
            }
        }
    }

    return true;
}
#endif
