// Shared pieces of the Geometry Collection commands: finding the asset, counting what it holds, and editing it as one
// undo step the way Fracture Mode's FGeometryCollectionEdit scope does.
#include "Domains/Animation/Physics/McpAnimGeometryCollection.h"

#include "Domains/Animation/McpAnimActionContext.h"

#if MCP_HAS_GC_FRACTURE
#include "Foundation/McpScopedEditorTransaction.h"
#include "Foundation/HandlerUtils/McpHandlerUtilsTransforms.h"
#include "Safety/McpSafeOperations.h"

#include "ComponentRecreateRenderStateContext.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "Interfaces/IPluginManager.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"
#endif

namespace McpAnimationHandlers {
namespace McpGeometryCollection {
bool RefuseUnsupported(FActionContext &Context)
{
    Context.Fail(TEXT("NOT_SUPPORTED"),
                 TEXT("This EngineWire build has no Geometry Collection commands: they are built for UE 5.4 with the "
                      "Fracture and PlanarCut plugins present. Nothing was changed."));
    return false;
}

#if MCP_HAS_GC_FRACTURE
bool RequireFracturePlugins(FActionContext &Context)
{
    for (const TCHAR *Name : {TEXT("Fracture"), TEXT("PlanarCut")})
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
        if (!Plugin.IsValid() || !Plugin->IsEnabled())
        {
            Context.Fail(TEXT("PLUGIN_DISABLED"),
                         FString::Printf(TEXT("The %s plugin is not enabled in this project; enable it (Edit > Plugins) "
                                              "and restart the editor. Nothing was changed."),
                                         Name));
            return false;
        }
    }
    return true;
}

UGeometryCollection *ResolveCollection(FActionContext &Context, const TSharedPtr<FJsonObject> &Payload)
{
    FString Path, ActorName;
    Payload->TryGetStringField(TEXT("geometryCollectionPath"), Path);
    Payload->TryGetStringField(TEXT("actorName"), ActorName);
    Path.TrimStartAndEndInline();
    ActorName.TrimStartAndEndInline();
    if (!Path.IsEmpty())
    {
        UGeometryCollection *Asset = LoadObject<UGeometryCollection>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (!Asset)
        {
            Context.Fail(TEXT("ASSET_NOT_FOUND"),
                         FString::Printf(TEXT("%s is not a Geometry Collection asset."), *Path));
        }
        return Asset;
    }
    if (ActorName.IsEmpty())
    {
        Context.Fail(TEXT("INVALID_ARGUMENT"), TEXT("geometryCollectionPath or actorName is required."));
        return nullptr;
    }
    AActor *Actor = Context.FindActorByName(ActorName);
    UGeometryCollectionComponent *Component = Actor ? Actor->FindComponentByClass<UGeometryCollectionComponent>() : nullptr;
    UGeometryCollection *Asset = Component ? const_cast<UGeometryCollection *>(Component->GetRestCollection()) : nullptr;
    if (!Asset)
    {
        Context.Fail(Actor ? TEXT("INVALID_ARGUMENT") : TEXT("ACTOR_NOT_FOUND"),
                     Actor ? FString::Printf(TEXT("'%s' has no Geometry Collection component with an asset."), *ActorName)
                           : FString::Printf(TEXT("No actor named '%s' in the level."), *ActorName));
    }
    return Asset;
}

TArray<UGeometryCollectionComponent *> ComponentsUsing(const UGeometryCollection *Asset, bool bEditorWorldOnly)
{
    TArray<UGeometryCollectionComponent *> Users;
    for (TObjectIterator<UGeometryCollectionComponent> It(RF_ClassDefaultObject | RF_ArchetypeObject, true,
                                                          EInternalObjectFlags::Garbage);
         It; ++It)
    {
        const UWorld *World = It->GetWorld();
        if (It->GetRestCollection() == Asset && (!bEditorWorldOnly || (World && World->WorldType == EWorldType::Editor)))
        {
            Users.Add(*It);
        }
    }
    return Users;
}

int32 CountPieces(const FGeometryCollection &Collection)
{
    int32 Pieces = 0;
    for (int32 Index = 0; Index < Collection.NumElements(FGeometryCollection::TransformGroup); ++Index)
    {
        if (Collection.Children[Index].Num() == 0 && Collection.TransformToGeometryIndex[Index] != INDEX_NONE)
        {
            ++Pieces;
        }
    }
    return Pieces;
}

TArray<int32> RootBones(const FGeometryCollection &Collection)
{
    TArray<int32> Roots;
    for (int32 Index = 0; Index < Collection.NumElements(FGeometryCollection::TransformGroup); ++Index)
    {
        if (Collection.Parent[Index] == FGeometryCollection::Invalid)
        {
            Roots.Add(Index);
        }
    }
    return Roots;
}

void DescribeCollectionInto(const UGeometryCollection *Asset, const TSharedPtr<FJsonObject> &Out)
{
    const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Shared = Asset->GetGeometryCollection();
    if (!Shared.IsValid())
    {
        Out->SetNumberField(TEXT("pieceCount"), 0);
        return;
    }
    const FGeometryCollection &Collection = *Shared;
    const int32 NumTransforms = Collection.NumElements(FGeometryCollection::TransformGroup);

    // Depth from the parent chain rather than the Level attribute, which only some edits keep current.
    TArray<int32> Depth;
    Depth.Init(0, NumTransforms);
    int32 MaxDepth = NumTransforms > 0 ? 0 : -1;
    int32 Clusters = 0;
    for (int32 Index = 0; Index < NumTransforms; ++Index)
    {
        int32 Steps = 0;
        for (int32 Up = Collection.Parent[Index]; Up != FGeometryCollection::Invalid && Steps <= NumTransforms;
             Up = Collection.Parent[Up])
        {
            ++Steps;
        }
        Depth[Index] = Steps;
        MaxDepth = FMath::Max(MaxDepth, Steps);
        Clusters += Collection.Children[Index].Num() > 0 ? 1 : 0;
    }
    TArray<int32> PerLevel;
    TArray<int32> PiecesPerLevel;
    PerLevel.Init(0, MaxDepth + 1);
    PiecesPerLevel.Init(0, MaxDepth + 1);
    for (int32 Index = 0; Index < NumTransforms; ++Index)
    {
        ++PerLevel[Depth[Index]];
        PiecesPerLevel[Depth[Index]] += Collection.Children[Index].Num() == 0 ? 1 : 0;
    }

    TArray<FTransform> Globals;
    GeometryCollectionAlgo::GlobalMatrices(Collection.Transform, Collection.Parent, Globals);
    FBox Bounds(ForceInit);
    for (int32 Geometry = 0; Geometry < Collection.NumElements(FGeometryCollection::GeometryGroup); ++Geometry)
    {
        const int32 Transform = Collection.TransformIndex[Geometry];
        if (Globals.IsValidIndex(Transform))
        {
            Bounds += Collection.BoundingBox[Geometry].TransformBy(Globals[Transform]);
        }
    }

    const auto Numbers = [](const TArray<int32> &Values) {
        TArray<TSharedPtr<FJsonValue>> Array;
        for (int32 Value : Values)
        {
            Array.Add(MakeShared<FJsonValueNumber>(Value));
        }
        return Array;
    };
    Out->SetNumberField(TEXT("pieceCount"), CountPieces(Collection));
    Out->SetNumberField(TEXT("clusterCount"), Clusters);
    Out->SetNumberField(TEXT("transformCount"), NumTransforms);
    Out->SetNumberField(TEXT("levelCount"), MaxDepth + 1);
    Out->SetArrayField(TEXT("transformsPerLevel"), Numbers(PerLevel));
    Out->SetArrayField(TEXT("piecesPerLevel"), Numbers(PiecesPerLevel));
    Out->SetNumberField(TEXT("triangleCount"), Collection.NumElements(FGeometryCollection::FacesGroup));
    Out->SetNumberField(TEXT("vertexCount"), Collection.NumElements(FGeometryCollection::VerticesGroup));
    if (Bounds.IsValid)
    {
        TSharedPtr<FJsonObject> Box = MakeShared<FJsonObject>();
        Box->SetObjectField(TEXT("min"), McpHandlerUtils::VectorToJson(Bounds.Min));
        Box->SetObjectField(TEXT("max"), McpHandlerUtils::VectorToJson(Bounds.Max));
        Box->SetObjectField(TEXT("size"), McpHandlerUtils::VectorToJson(Bounds.GetSize()));
        Out->SetObjectField(TEXT("bounds"), Box);
    }
}

bool EditCollection(FActionContext &Context, UGeometryCollection *Asset, const TCHAR *Title, bool bShapeChanges,
                    TFunctionRef<bool(FGeometryCollection &)> Mutate)
{
    const TArray<UGeometryCollectionComponent *> Users = ComponentsUsing(Asset);
    TArray<UObject *> Recorded = {Asset};
    for (UGeometryCollectionComponent *User : Users)
    {
        if (!User->HasAnyFlags(RF_Transactional))
        {
            User->SetFlags(RF_Transactional);
        }
        Recorded.Add(User);
    }
    const TArray<UGeometryCollectionComponent *> Components = ComponentsUsing(Asset, false);
    bool bChanged = false;
    {
        FMcpScopedEditorTransaction Transaction(FText::FromString(Title), EMcpMutationDurability::EditorStateOnly, Recorded);
        // Thumbnail and preview components can retain the asset without any actor in the editor world.
        // Retire their old proxies before changing transform counts, then recreate them after the asset rebuild.
        TArray<FComponentRecreateRenderStateContext> RenderStates;
        RenderStates.Reserve(Components.Num());
        for (UGeometryCollectionComponent *Component : Components)
        {
            RenderStates.Emplace(static_cast<UActorComponent *>(Component));
        }
        if (!RenderStates.IsEmpty())
        {
            FlushRenderingCommands();
        }
        {
            // Reset the transform caches and physics state for all matching components, including previews.
            TOptional<FGeometryCollectionEdit> Edit;
            if (!Components.IsEmpty())
            {
                Edit.Emplace(Components[0], GeometryCollection::EEditUpdate::RestPhysicsDynamic, !bShapeChanges, true);
            }
            const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Shared = Asset->GetGeometryCollection();
            bChanged = Shared.IsValid() && Mutate(*Shared);
            if (!Edit.IsSet())
            {
                if (bChanged && bShapeChanges)
                {
                    Asset->UpdateGeometryDependentProperties();
                }
                Asset->InvalidateCollection();
            }
        }
        Asset->RebuildRenderData();
        Transaction.DescribeInto(Context.Resp);
    }
    Asset->MarkPackageDirty();
    return bChanged;
}

void SaveInto(FActionContext &Context, UGeometryCollection *Asset)
{
    // Saving builds the simulation data, which caches the physical material's density into a tagged property between
    // the save's reference-harvesting pass and its write pass. On a new asset (cache still 0) the write pass then meets
    // a name the harvest never saw: an ensure, "CachedDensityFromPhysicsMaterialInGCm3 is not mapped when saving".
    // Caching it first makes both passes agree.
    Asset->CacheMaterialDensity();
    const bool bSaved = McpSafeOperations::McpSafeAssetSave(Asset);
    Context.Resp->SetBoolField(TEXT("saved"), bSaved);
    if (!bSaved)
    {
        Context.Resp->SetStringField(TEXT("warning"),
                                     TEXT("The Geometry Collection changed but could not be saved; save it before "
                                          "closing the editor."));
    }
}

bool ReadRange(const TSharedPtr<FJsonObject> &Payload, const TCHAR *Field, double &Value, double Min, double Max,
               FString &OutError)
{
    if (!Payload->HasField(Field))
    {
        return true;
    }
    double Number = 0.0;
    if (!Payload->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number) || Number < Min || Number > Max)
    {
        OutError = FString::Printf(TEXT("%s must be a number from %g to %g"), Field, Min, Max);
        return false;
    }
    Value = Number;
    return true;
}

bool ReadWhole(const TSharedPtr<FJsonObject> &Payload, const TCHAR *Field, int32 &Value, int32 Min, int32 Max,
               FString &OutError)
{
    double Number = Value;
    if (!ReadRange(Payload, Field, Number, Min, Max, OutError))
    {
        return false;
    }
    if (Number != FMath::FloorToDouble(Number))
    {
        OutError = FString::Printf(TEXT("%s must be a whole number"), Field);
        return false;
    }
    Value = static_cast<int32>(Number);
    return true;
}

bool ReadIndexArray(const TSharedPtr<FJsonObject> &Payload, const TCHAR *Field, int32 Limit, TArray<int32> &Out,
                    FString &OutError)
{
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    if (!Payload->TryGetArrayField(Field, Values) || !Values)
    {
        return true;
    }
    for (const TSharedPtr<FJsonValue> &Value : *Values)
    {
        double Number = -1.0;
        if (!Value.IsValid() || !Value->TryGetNumber(Number) || Number != FMath::FloorToDouble(Number) || Number < 0 ||
            Number >= Limit)
        {
            OutError = FString::Printf(TEXT("%s takes transform indices from 0 to %d"), Field, Limit - 1);
            return false;
        }
        Out.AddUnique(static_cast<int32>(Number));
    }
    return true;
}
#endif
} // namespace McpGeometryCollection
} // namespace McpAnimationHandlers
