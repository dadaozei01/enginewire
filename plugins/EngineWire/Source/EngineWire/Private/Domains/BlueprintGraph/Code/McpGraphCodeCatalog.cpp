#include "Domains/BlueprintGraph/Code/McpGraphCode.h"

#include "Domains/BlueprintGraph/McpBPGraphPrivate.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/UnrealType.h"

namespace McpGraphCode
{
namespace CatalogParts
{
// A spawner still naming a member of a class a recompile replaced: the filter ensures on one (an editor stall of many
// seconds) and the node it makes would call the old class.
bool IsStale(FBlueprintActionInfo& Info)
{
    const UFunction* Function = Info.GetAssociatedFunction();
    const FProperty* Property = Function ? nullptr : Info.GetAssociatedProperty();
    const UClass* Owner = Function ? Function->GetOwnerClass() : Property ? Property->GetOwnerClass() : nullptr;
    return Owner && (Owner->HasAnyClassFlags(CLASS_NewerVersionExists) || Owner->GetOutermost() == GetTransientPackage());
}

// Each translated segment of a common category and its English source, built once from the engine's own text table
// (EditorCategoryUtils.cpp): GetCommonCategory keeps only the displayed string, so its source is already 变量. Flow
// Control's 流程控制 -> Flow Control, 变量 -> Variables.
const TMap<FString, FString>& CommonSegments()
{
    static const TMap<FString, FString> Segments = []
    {
        const TPair<const TCHAR*, const TCHAR*> Keys[] = {
            {TEXT("AICategory"), TEXT("AI")}, {TEXT("AnimationCategory"), TEXT("Animation")}, {TEXT("AudioCategory"), TEXT("Audio")},
            {TEXT("DevelopmentCategory"), TEXT("Development")}, {TEXT("EffectsCategory"), TEXT("Effects")}, {TEXT("GameplayCategory"), TEXT("Game")},
            {TEXT("InputCategory"), TEXT("Input")}, {TEXT("MathCategory"), TEXT("Math")}, {TEXT("NetworkingCategory"), TEXT("Networking")},
            {TEXT("PawnCategory"), TEXT("Pawn")}, {TEXT("RenderingCategory"), TEXT("Rendering")}, {TEXT("UtilitiesCategory"), TEXT("Utilities")},
            {TEXT("DelegatesCategory"), TEXT("Event Dispatchers")}, {TEXT("VariablesCategory"), TEXT("Variables")}, {TEXT("ClassCategory"), TEXT("Class")},
            {TEXT("UserInterfaceCategory"), TEXT("User Interface")}, {TEXT("AnimNotifyCategory"), TEXT("Add AnimNotify Event")},
            {TEXT("BranchPointCategory"), TEXT("Add Montage Branching Point Event")}, {TEXT("FlowControlCategory"), TEXT("Flow Control")},
            {TEXT("TransformationCategory"), TEXT("Transformation")}, {TEXT("StringCategory"), TEXT("String")}, {TEXT("TextCategory"), TEXT("Text")},
            {TEXT("NameCategory"), TEXT("Name")}, {TEXT("EnumCategory"), TEXT("Enum")}, {TEXT("StructCategory"), TEXT("Struct")},
            {TEXT("MacroCategory"), TEXT("Macro")}};
        TMap<FString, FString> Out;
        for (const TPair<const TCHAR*, const TCHAR*>& Key : Keys)
        {
            const FString Source = Key.Value;
            FText Shown;
#if ENGINE_MAJOR_VERSION > 5 || ENGINE_MINOR_VERSION >= 5
            const bool bFound = FText::FindTextInLiveTable_Advanced(TEXT("EditorCategoryUtils"), Key.Key, Shown, &Source);
#else
            const bool bFound = FText::FindText(TEXT("EditorCategoryUtils"), Key.Key, Shown, &Source);
#endif
            if (bFound && !Shown.ToString().Equals(Source))
            {
                Out.Add(Shown.ToString().TrimStartAndEnd(), Source);
            }
        }
        return Out;
    }();
    return Segments;
}
} // namespace CatalogParts

FString CategoryText(const FText& Category)
{
    TArray<FString> Segments;
    SourceText(Category).ParseIntoArray(Segments, TEXT("|"), false);
    for (FString& Segment : Segments)
    {
        if (const FString* English = CatalogParts::CommonSegments().Find(Segment.TrimStartAndEnd()))
        {
            Segment = *English;
        }
    }
    return FString::Join(Segments, TEXT("|"));
}

FString MemberKey(const UStruct* Owner, FName Name)
{
    const UClass* Class = Cast<UClass>(Owner);
    Class = Class ? Class->GetAuthoritativeClass() : nullptr;
    return Owner ? (Class ? Class->GetPathName() : Owner->GetPathName()) + TEXT(":") + Name.ToString() : FString();
}

FString MenuTitleKey(const FString& MenuId)
{
    int32 Bar = INDEX_NONE;
    return (MenuId.FindLastChar(TEXT('|'), Bar) ? MenuId.Mid(Bar + 1) : MenuId).Replace(TEXT(" "), TEXT("")).ToLower();
}

TArray<FMenuEntry> MenuCandidates(const TSet<FString>& Titles)
{
    TArray<FMenuEntry> Candidates;
    if (Titles.Num() == 0)
    {
        return Candidates;
    }
    for (auto It = FBlueprintActionDatabase::Get().GetAllActions().CreateConstIterator(); It; ++It)
    {
        UObject* Owner = It->Key.ResolveObjectPtr();
        for (const UBlueprintNodeSpawner* Spawner : It->Value)
        {
            if (!Owner || !Spawner)
            {
                continue;
            }
            const FString Title = SourceText(Spawner->PrimeDefaultUiSpec().MenuName).Replace(TEXT(" "), TEXT(""));
            if (Title.IsEmpty() || Titles.Contains(Title.ToLower()))
            {
                Candidates.Add(FMenuEntry{Owner, Spawner, FString()});
            }
        }
    }
    return Candidates;
}

TArray<FMenuEntry> MenuEntries(UBlueprint* Blueprint, UEdGraph* Graph, const TArray<FMenuEntry>& Candidates, const TSet<FString>& Titles)
{
    FBlueprintActionFilter Filter;
    Filter.Context.Graphs.Add(Graph);
    Filter.Context.Blueprints.Add(Blueprint);
    TArray<FMenuEntry> Entries;
    for (const FMenuEntry& Candidate : Candidates)
    {
        FBlueprintActionInfo Info(Candidate.Owner, Candidate.Spawner);
        if (CatalogParts::IsStale(Info) || Filter.IsFiltered(Info))
        {
            continue;
        }
        const FBlueprintActionUiSpec Spec = Candidate.Spawner->GetUiSpec(Filter.Context, Info.GetBindings());
        const FString Title = SourceText(Spec.MenuName).Replace(TEXT(" "), TEXT(""));
        if (Titles.Contains(Title.ToLower()))
        {
            const UFunction* Function = Info.GetAssociatedFunction();
            const FProperty* Property = Function ? nullptr : Info.GetAssociatedProperty();
            const FString Member = Function ? MemberKey(Function->GetOwnerClass(), Function->GetFName())
                                 : Property ? MemberKey(Property->GetOwnerStruct(), Property->GetFName())
                                            : FString();
            Entries.Add(FMenuEntry{Candidate.Owner, Candidate.Spawner, CategoryText(Spec.Category).Replace(TEXT(" "), TEXT("")) + TEXT("|") + Title, Member});
        }
    }
    // Byte order of id, then of the owner's path: two reads or writes of one graph pick the same spawner.
    Entries.Sort([](const FMenuEntry& A, const FMenuEntry& B)
    {
        const int32 Order = A.Id.Compare(B.Id, ESearchCase::CaseSensitive);
        return Order != 0 ? Order < 0 : A.Owner->GetPathName().Compare(B.Owner->GetPathName(), ESearchCase::CaseSensitive) < 0;
    });
    return Entries;
}

// Epic's create_node_from_name: the spawner whose menu entry in this graph is MenuId (exact case first, as
// canonicalize then matches any case), invoked at At. Several spawners showing one id: the first by owner path.
UEdGraphNode* CreateByMenuId(UBlueprint* Blueprint, UEdGraph* Graph, const FString& MenuId, const FVector2D& At, FString& OutError)
{
    const TSet<FString> Titles = {MenuTitleKey(MenuId)};
    const TArray<FMenuEntry> Entries = MenuEntries(Blueprint, Graph, MenuCandidates(Titles), Titles);
    const FMenuEntry* Found = Entries.FindByPredicate([&MenuId](const FMenuEntry& Entry) { return Entry.Id.Equals(MenuId, ESearchCase::CaseSensitive); });
    Found = Found ? Found : Entries.FindByPredicate([&MenuId](const FMenuEntry& Entry) { return Entry.Id.Equals(MenuId, ESearchCase::IgnoreCase); });
    if (!Found)
    {
        TArray<FString> Offered;
        for (const FMenuEntry& Entry : Entries)
        {
            if (Offered.Num() < 8)
            {
                Offered.AddUnique(Entry.Id);
            }
        }
        OutError = Offered.Num() > 0
            ? FString::Printf(TEXT("The menu of graph %s has no '%s'; it offers that title as: %s."), *Graph->GetName(), *MenuId, *FString::Join(Offered, TEXT(", ")))
            : FString::Printf(TEXT("The menu of graph %s has no node titled '%s' (a menu id is Category|Title with spaces removed, as read_graph_as_code prints it)."),
                              *Graph->GetName(), *MenuId);
        return nullptr;
    }
    const FBlueprintActionInfo Info(Found->Owner, Found->Spawner);
    UEdGraphNode* Node = Found->Spawner->Invoke(Graph, Info.GetBindings(), At);
    if (!Node)
    {
        OutError = FString::Printf(TEXT("The menu entry '%s' made no node in graph %s."), *MenuId, *Graph->GetName());
    }
    return Node;
}

bool TryCreateMenuNode(McpBlueprintGraphHandlers::FActionContext& Context, float X, float Y)
{
    FString MenuId;
    if (!Context.Payload->TryGetStringField(TEXT("menuId"), MenuId) || MenuId.IsEmpty())
    {
        return false;
    }
    // A named custom event is no single menu entry ("Add Custom Event..."): AddEvent|Custom|Name makes one, as Epic's
    // create_node does.
    const FString CustomEvent = TEXT("AddEvent|Custom|");
    if (MenuId.StartsWith(CustomEvent, ESearchCase::CaseSensitive))
    {
        Context.Payload->SetStringField(TEXT("customEventName"), MenuId.Mid(CustomEvent.Len()));
        return McpBlueprintGraphHandlers::TryCreateCustomEventNode(Context, TEXT("CustomEvent"), X, Y);
    }
    FString Error;
    UEdGraphNode* Node = CreateByMenuId(Context.Blueprint, Context.TargetGraph, MenuId, FVector2D(X, Y), Error);
    if (!Node)
    {
        Context.SendError(Error, TEXT("MENU_ID_NOT_FOUND"));
        return true;
    }
    Context.FinishCreatedNode(Node, X, Y);
    return true;
}
} // namespace McpGraphCode
