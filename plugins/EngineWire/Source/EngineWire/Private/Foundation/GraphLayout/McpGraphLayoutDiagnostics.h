#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UEdGraph;
class UEdGraphNode;
class UEdGraphNode_Comment;
class UK2Node_Knot;

namespace McpGraphLayoutDiag
{
inline constexpr const TCHAR* AutoKnotMarker = TEXT("__ew_auto_knot__");

struct FCommentGroup
{
    UEdGraphNode_Comment* Comment = nullptr;
    FBox2D OriginalBounds = FBox2D(ForceInit);
    TArray<UEdGraphNode*> Members;
    int32 Depth = 0;
    bool bWasEmpty = false;
};

struct FEdgeIssue
{
    FString Kind;
    FString EdgeType;
    FString FromGuid;
    FString FromTitle;
    FString FromPin;
    FString ToGuid;
    FString ToTitle;
    FString ToPin;
    double DeltaX = 0.0;
    double DeltaY = 0.0;
    double Length = 0.0;
    int32 CrossedNodeCount = 0;
};

struct FLayoutQualityReport
{
    int32 NodeOverlapCount = 0;
    int32 WireNodeCrossingCount = 0;
    int32 DataWireNodeCrossingCount = 0;
    int32 CommentIntrusionCount = 0;
    int32 BackwardExecEdgeCount = 0;
    int32 LongCrossRegionExecCount = 0;
    int32 LongDelegateEdgeCount = 0;
    int32 LongDataEdgeCount = 0;
    int32 DanglingAutoKnotCount = 0;
    int32 BoundaryEdgeCount = 0;
    double MaxDataEdgeLength = 0.0;
    double MaxDisplacement = 0.0;
    TArray<FEdgeIssue> Issues;
    TArray<FEdgeIssue> BoundaryEdges;
    TArray<TArray<FString>> UnusedPureGroups;

    TSharedRef<FJsonObject> ToJson() const;
};

bool IsAutoLayoutKnot(const UEdGraphNode* Node);
void MarkAutoLayoutKnot(UK2Node_Knot* Knot);

/** Collapses EngineWire-owned PC_Exec auto-knots (never manual knots, never data/wildcard knots). */
int32 CollapseAutoExecKnots(UEdGraph* Graph, const TSet<UEdGraphNode*>* ScopeNodes = nullptr,
                            TArray<FGuid>* OutRemovedGuids = nullptr);

/** Collapses EngineWire-owned non-exec (data) auto-knots (never manual knots). */
int32 CollapseAutoDataKnots(UEdGraph* Graph, const TSet<UEdGraphNode*>* ScopeNodes = nullptr,
                            TArray<FGuid>* OutRemovedGuids = nullptr);

/** Sorted multiset of folded non-knot pin connections for exact topology invariance checks. */
TArray<FString> BuildSemanticTopologyFingerprint(const UEdGraph* Graph);

/** Connected components of pure nodes that have no directed data path to any active exec/entry/result node. */
TArray<TArray<UEdGraphNode*>> FindUnusedPureGroups(const UEdGraph* Graph,
                                                   const TSet<UEdGraphNode*>* Ignore = nullptr);

/** Estimates/measures current 2D bounding boxes for all non-comment nodes in Graph. */
TMap<const UEdGraphNode*, FBox2D> MeasureNodeBoxes(const UEdGraph* Graph,
                                                   const TSet<UEdGraphNode*>* Ignore = nullptr);

/** Collects comment boxes and their member nodes, sorted innermost/smallest first. */
TArray<FCommentGroup> CollectCommentGroups(const UEdGraph* Graph,
                                           const TMap<const UEdGraphNode*, FBox2D>& NodeBoxes);

/** Returns the innermost (or smallest-area) comment owning Node, or nullptr. */
UEdGraphNode_Comment* FindPrimaryOwnerComment(const UEdGraphNode* Node,
                                              const TArray<FCommentGroup>& Groups);

/** Translates or refits comment boxes whose members moved; untouched comments remain fixed. */
int32 SyncAndRefitComments(const TArray<FCommentGroup>& Groups,
                           const TMap<const UEdGraphNode*, FVector2D>& OldPositions,
                           const TMap<const UEdGraphNode*, FBox2D>& NewBoxes,
                           bool bAllowShrinkOnFullReflow,
                           TArray<UEdGraphNode_Comment*>* OutUpdated = nullptr);

/**
 * EW-006: translates a solved selection block by the smallest clear offset (down, right, then up)
 * so it does not overlap unselected nodes or comments. Returns false if no clear offset is found.
 */
bool ShiftBlockClearOfObstacles(UEdGraph* Graph, const TArray<UEdGraphNode*>& Listed,
                                const TMap<const UEdGraphNode*, FBox2D>& BeforeBoxes,
                                const TArray<FCommentGroup>& Comments, int32& OutObstacleHits);

/** Analyzes node overlaps, wire crossings, backward execs, long delegate spans, comment intrusions, and dead pure islands. */
FLayoutQualityReport AnalyzeGraphQuality(const UEdGraph* Graph,
                                         const TSet<UEdGraphNode*>* ScopeNodes = nullptr,
                                         const TMap<const UEdGraphNode*, FBox2D>* CachedBoxes = nullptr,
                                         const TArray<FCommentGroup>* OriginalComments = nullptr,
                                         const TMap<const UEdGraphNode*, FVector2D>* OldPositions = nullptr);
} // namespace McpGraphLayoutDiag
