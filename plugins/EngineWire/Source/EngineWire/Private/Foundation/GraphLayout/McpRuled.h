#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;

// Ruled Blueprint layout: the house style a person arranges a graph in by hand, so nodes an agent makes without
// coordinates come out readable and every agent lays a graph out the same way.
//  - Each run of exec nodes is one row, left to right; the white exec pin centres of a row sit on one horizontal line
//    (the node tops differ by each node's own pin offset, not by a shared NodePosY).
//  - A node's data inputs are laid out right to left in columns, each producer one column left of its reader; a group
//    of inputs is spread evenly and centred on the reader's input pins (two inputs centred, three with the middle one
//    level with the second pin); a lone input is level with its pin.
//  - Data deeper than one column goes below the exec row, out of the white line's way; it may reach in under the
//    nodes before its reader, never into another node's data.
//  - A second exec output (Branch False, Completed, Is Not Valid) starts its own row right of its node, dropped below
//    what is there. A row longer than MaxRowWidth breaks before the node that would cross it, and the white wire runs
//    down to the next row through two reroutes in the gap between the rows.
//  - Each region (an event or function and everything wired to it) keeps RegionGap clear of every other region: the
//    logic of one event never runs in among another's.
//  - Nodes not listed stay where they are unless they are in the way: editing a graph never moves the new nodes off
//    their line to get round old ones. A new chain that goes on from a node that stays (or a value beside the node that
//    reads it) keeps its white line level; what lies below that line and is in the way moves down to make room (the
//    chain's own region below the highest node in the way by one amount, another region whole). Only when something at
//    the line's own level is in the way does the chain slide right, or as a last resort drop below it. One put between
//    two nodes that stay (A -> new -> B) moves B and what runs after it right by as much as it needs.
// Solve is pure geometry; LayoutNodes (McpRuledNodes.cpp) estimates node and pin geometry and applies it.
namespace McpRuled
{
/** A node's estimated size and pin centres, measured down from its top edge. */
struct FShape
{
    float Width = 0.0f;
    float Height = 0.0f;
    /** The exec input's centre; below zero when the node has none (a pure node or an event). */
    float ExecIn = -1.0f;
    TArray<float> ExecOut;
    TArray<float> DataIn;
    TArray<float> DataOut;
};

/** A wire: FromPin indexes the From node's ExecOut or DataOut, ToPin the To node's DataIn (unused for exec). */
struct FLink
{
    int32 From = INDEX_NONE;
    int32 FromPin = 0;
    int32 To = INDEX_NONE;
    int32 ToPin = 0;
    bool bExec = false;
};

/** A row break: the exec wire Links[Link] runs through reroutes centred at A (right of the row) then B (left of the next). */
struct FRowBreak
{
    int32 Link = INDEX_NONE;
    FVector2D A = FVector2D::ZeroVector;
    FVector2D B = FVector2D::ZeroVector;
};

struct FParams
{
    /** Clear space between two exec nodes of a row. */
    float ExecGap = 96.0f;
    /** Clear space between a data producer and its reader. */
    float DataGap = 48.0f;
    /** Least clear space between two stacked inputs of one reader. */
    float InputGap = 16.0f;
    /** Clear space between an exec row and the data laid out under it. */
    float LaneGap = 32.0f;
    /** Clear space a block dropped below others keeps from them. */
    float RowGap = 96.0f;
    /** Least clear space between any two nodes. */
    float NodePad = 24.0f;
    /** Clear space between two regions (events or functions not wired together). */
    float RegionGap = 128.0f;
    /** A row is broken before the node that would make it wider than this. */
    float MaxRowWidth = 6000.0f;
};

struct FProblem
{
    TArray<FShape> Shapes;
    TArray<FLink> Links;
    /** True for the nodes to place; the others keep Pos and anchor and block the placed ones. */
    TArray<bool> Free;
    /** Top-left corners; read for the nodes that are not free. */
    TArray<FVector2D> Pos;
    /** Other boxes to keep clear of (a comment bubble above a node). */
    TArray<FBox2D> Obstacles;
    /** For each obstacle, the node it belongs to (it moves and counts in that node's region), or INDEX_NONE. */
    TArray<int32> ObstacleOwner;
    /** Where a block wired to no placed node goes (dropped below what is there). */
    FVector2D Origin = FVector2D::ZeroVector;
};

struct FSolution
{
    /** Every node's top-left: the free ones placed, the others where they were unless room was made for an insert. */
    TArray<FVector2D> Pos;
    TArray<FRowBreak> Breaks;
};

FSolution Solve(const FProblem& Problem, const FParams& Params = FParams());

/** The pin centres and size LayoutNodes assumes for Node (Slate measures the real ones when the graph is drawn). */
FShape EstimateShape(const UEdGraphNode& Node);

/** Places Free in Graph by Solve; every other node stays where it is, unless an insert between two of them needs room. A block wired to nothing that stays starts at
 *  Origin, or left of and below the graph when Origin is null. Nodes in Ignore (about to be removed) are treated as
 *  absent. Row breaks add two reroute nodes each (appended to OutAdded). Returns the lowest bottom edge of the placed
 *  nodes, or Origin's Y when none was placed. */
float LayoutNodes(UEdGraph* Graph, const TArray<UEdGraphNode*>& Free, const FVector2D* Origin = nullptr,
                  TArray<UEdGraphNode*>* OutAdded = nullptr, const TSet<UEdGraphNode*>* Ignore = nullptr,
                  const FParams& Params = FParams());
} // namespace McpRuled
