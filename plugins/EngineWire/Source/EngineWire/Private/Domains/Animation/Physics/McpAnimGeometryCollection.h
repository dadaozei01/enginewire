#pragma once

// Chaos destruction: a Geometry Collection made from static meshes, cut with Voronoi cells and grouped with auto
// cluster, the steps of Fracture Mode's New, Uniform (or Custom) Voronoi and Auto Cluster. The handlers are compiled
// only where MCP_HAS_GC_FRACTURE is 1 (UE 5.4, Fracture and PlanarCut present); elsewhere each refuses NOT_SUPPORTED.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UGeometryCollection;
class UGeometryCollectionComponent;
class FGeometryCollection;

namespace McpAnimationHandlers {
struct FActionContext;

namespace McpGeometryCollection {
#if MCP_HAS_GC_FRACTURE
// Fracture and auto cluster call into the Fracture and PlanarCut plugins, whose DLLs are delay-loaded on Windows: a
// call with either plugin off would not resolve. Refuses PLUGIN_DISABLED, naming the plugin, and returns false.
bool RequireFracturePlugins(FActionContext &Context);

// The asset named by geometryCollectionPath, or the rest collection of actorName's Geometry Collection component.
// Refuses INVALID_ARGUMENT, ASSET_NOT_FOUND or ACTOR_NOT_FOUND and returns null.
UGeometryCollection *ResolveCollection(FActionContext &Context, const TSharedPtr<FJsonObject> &Payload);

// Components in editor worlds that draw this asset (PIE copies are left alone).
TArray<UGeometryCollectionComponent *> ComponentsUsing(const UGeometryCollection *Asset, bool bEditorWorldOnly = true);

// Leaf transforms that carry geometry: the pieces a fracture produced, and what Fracture Mode counts as bones at the
// deepest level.
int32 CountPieces(const FGeometryCollection &Collection);

// The root transforms.
TArray<int32> RootBones(const FGeometryCollection &Collection);

// Pieces, clusters, levels with the transforms on each, bounds in the asset's space, triangles and vertices.
void DescribeCollectionInto(const UGeometryCollection *Asset, const TSharedPtr<FJsonObject> &Out);

// One change to the asset's collection as one undo step (titled Title): the asset and every component drawing it are
// recorded, components drop and rebuild physics state around the change, and render data is rebuilt after. With
// bShapeChanges, convex hulls, proximity and volumes are recomputed as Fracture Mode does after a cut. Mutate returns
// false to report that nothing changed. The undo block goes into the reply; saving is left to the caller, after the
// transaction has closed.
bool EditCollection(FActionContext &Context, UGeometryCollection *Asset, const TCHAR *Title, bool bShapeChanges,
                    TFunctionRef<bool(FGeometryCollection &)> Mutate);

// Saves the asset and records saved (and a warning when it could not be saved) in the reply.
void SaveInto(FActionContext &Context, UGeometryCollection *Asset);

// Optional number fields: an absent field leaves Value as it was; anything outside [Min, Max] (or, for ReadWhole, not
// a whole number) is refused with OutError naming the field.
bool ReadRange(const TSharedPtr<FJsonObject> &Payload, const TCHAR *Field, double &Value, double Min, double Max,
               FString &OutError);
bool ReadWhole(const TSharedPtr<FJsonObject> &Payload, const TCHAR *Field, int32 &Value, int32 Min, int32 Max,
               FString &OutError);

// Reads an optional integer array field into Out, refusing anything that is not a whole number in [0, Limit).
bool ReadIndexArray(const TSharedPtr<FJsonObject> &Payload, const TCHAR *Field, int32 Limit, TArray<int32> &Out,
                    FString &OutError);
#endif

// The NOT_SUPPORTED refusal every handler gives where the build left the destruction commands out.
bool RefuseUnsupported(FActionContext &Context);
} // namespace McpGeometryCollection
} // namespace McpAnimationHandlers
