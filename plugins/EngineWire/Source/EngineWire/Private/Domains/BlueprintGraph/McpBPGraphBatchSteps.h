#pragma once

#include "Core/Requests/McpResponseCaptureRegistry.h"
#include "Domains/BlueprintGraph/McpBPGraphPrivate.h"

// One build_graph step: resolve its "$alias" references, run it through the
// ordinary single-step handler, and record the outcome. Split from the batch
// loop in McpBPGraphBatch.cpp.
namespace McpBlueprintGraphHandlers::GraphBatch
{
constexpr int32 MaxBatchSteps = 200;

struct FBatchLinkChange
{
    FString FromGuid;
    FString ToGuid;
    bool bExec = false;
    bool bSecondaryExecOut = false;
    TArray<FString> ReplacedGuids;
};

struct FBatchState
{
    /** `id` of an earlier step -> the guid of the node it created. */
    TMap<FString, FString> Aliases;
    /** Left edge of the grid auto-placed nodes fill, right of existing nodes. */
    float OriginX = 0.0f;
    int32 AutoPlaced = 0;
    /** Guids of the nodes placed on that grid, for SettleAutoPlacedNodes. */
    TArray<FString> AutoPlacedGuids;
    /** Guids of newly created nodes whose caller explicitly gave posX/posY (kept fixed). */
    TSet<FString> ExplicitlyPlacedGuids;
    /** Topology changes made by connect_pins steps (new links + replaced links), for local re-layout. */
    TArray<FBatchLinkChange> LinkChanges;
    /** Guids of every node the Blueprint held when the batch began: a create step that hands one of them back (an
     *  event the graph already has) did not make it, so the layout leaves it where it is. */
    TSet<FString> ExistingGuids;
};

/**
 * Runs operations[Index] and fills Entry with its outcome. Returns an empty
 * string on success, otherwise the reason, with OutErrorCode set.
 */
FString RunBatchStep(const FActionContext& Context, FBatchState& State,
                     const TSharedPtr<FJsonValue>& StepValue, int32 Index,
                     const TSharedPtr<FJsonObject>& Entry, const TSharedPtr<FJsonObject>& NodeIds,
                     FString& OutErrorCode);

/** Runs one step's handler in-process (McpBPGraphBatchPlacement.cpp) and hands
 *  back the reply it parked instead of sent; OutPins describes the pins of a node the step created. */
FMcpCapturedResponse RunStep(const FActionContext& Parent, const TSharedPtr<FJsonObject>& Payload,
                             const FString& Edit, const FString& StepId, FString& OutPins);

/** RunStep for a step that may create a node: auto-placed when it names no position, and moved to a free
 *  slot, with a placementWarning naming where it went, when the position it named overlaps a node. */
FMcpCapturedResponse RunPlacedStep(const FActionContext& Parent, FBatchState& State,
                                   const TSharedPtr<FJsonObject>& Payload, const FString& Edit,
                                   const FString& StepId, FString& OutPins);

/** The payload one step is sent (McpBPGraphBatchCheck.cpp): the batch's shared fields under the step's own, with
 *  from/to "$id.Pin" shorthand expanded. */
TSharedPtr<FJsonObject> BuildStepPayload(const TSharedPtr<FJsonObject>& Batch, const TSharedPtr<FJsonObject>& Step,
                                         const FString& Edit);

/** Replaces each "$id" node reference in Payload with what State knows for it; false, with OutError listing the
 *  ids defined so far, for one no earlier step defined. */
bool ResolveStepAliases(const FBatchState& State, const TSharedPtr<FJsonObject>& Payload, FString& OutError);

/** Runs PrecheckSteps and sends its refusal (marked dryRun when bDryRun); true when the batch may go on. */
bool PassesPrecheck(const FActionContext& Context, const TArray<TSharedPtr<FJsonValue>>& Steps, bool bDryRun);

/** dryRun, after the pre-check passed: resolves every "$id" against the steps before it and looks up every node named
 *  by guid, runs nothing, and replies with every problem found (or the steps checked and the nodes it would create). */
bool AnswerDryRun(const FActionContext& Context, const TArray<TSharedPtr<FJsonValue>>& Steps);

/** Synchronous, non-destructive edits only; the member steps are among them. */
bool IsBatchableEdit(const FString& Edit);

/** Why a step whose edit IsBatchableEdit refuses cannot run, listing the edits that can. */
FString DescribeUnbatchableEdit(const FString& Edit);

/** Every function, variable, dispatcher and async factory the steps name must resolve before
 *  any step runs; add_variable, add_function, add_event and add_event_dispatcher steps declare
 *  theirs for later steps. Empty on success, else the reason with OutIndex and OutCode set. */
FString PrecheckSteps(const FActionContext& Context, const TArray<TSharedPtr<FJsonValue>>& Steps,
                      int32& OutIndex, FString& OutCode);

/** One step of that pre-check: a set_node_property Expression the node cannot parse is described (one at step 7
 *  stopped the batch with 7 nodes made); an add_variable of a bool is noted in DeclaredBools for the steps after
 *  it. Empty for every other step. */
FString DescribeExpressionStep(const UBlueprint* Blueprint, const FJsonObject& Step, TSet<FName>& DeclaredBools);

/** add_variable, add_function, add_event and add_event_dispatcher run their ordinary member
 *  handler (which compiles, so later steps can use what it made). False for any other edit. */
bool RunBlueprintMemberStep(const FActionContext& Parent, const FString& Edit, const FString& StepId,
                            const TSharedPtr<FJsonObject>& Payload);

/** build_graph's body after the subAction check: checks, runs every step, and replies.
 *  bCompile false leaves the compile and the save to the caller (McpBlueprintBehaviour). */
bool RunGraphBatch(FActionContext& Context, int32 MaxSteps, bool bCompile);

/** A node of Blueprint by guid text, in any of its graphs. */
UEdGraphNode* FindBatchNode(UBlueprint* Blueprint, const FString& Guid);

/** Once every step ran, moves each auto-placed node beside a node it is wired to (arrange_nodes too).
 *  Returns the nodes that found no wired neighbour to settle by, or no free slot, and did not move. */
TArray<UEdGraphNode*> SettleAutoPlacedNodes(UBlueprint* Blueprint, const FBatchState& State);

/** Once every step ran, lays the auto-placed nodes out in the house style (McpRuled), graph by graph: exec rows with
 *  their exec pins level, inputs centred one column left of their readers, deeper data under the row. Nodes given a
 *  position, and every node the graph held before, stay where they are. Optionally fills OutDiagnostics with
 *  post-layout wire/overlap/comment quality metrics. */
void LayoutAutoPlacedNodes(UBlueprint* Blueprint, const FBatchState& State,
                           TSharedPtr<FJsonObject>* OutDiagnostics = nullptr);
}
