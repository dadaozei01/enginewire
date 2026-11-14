// Composite Niagara setups for crowd and battle effects (UE 5.8):
//
// - setup_niagara_neighbor_grid builds the usual GPU neighbour search on one emitter in one call: a NeighborGrid3D
//   user parameter, a "populate" simulation stage that adds every particle to its grid cell and a "query" stage that
//   walks the 27 surrounding cells and writes each particle's neighbour count and a separation vector. It runs the
//   existing commands (set_niagara_data_interface, add_simulation_stage, create/apply_niagara_scratch_module,
//   bind_parameter_to_source, set_parameter_value) as captured steps, so each one keeps its own checks.
// - create_niagara_data_channel makes a Niagara Data Channel asset with its variables, the channel game code
//   (Blueprint "Write To Niagara Data Channel") writes and emitters read with a DataChannelRead data interface.
#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"

#if MCP_HAS_NIAGARA_INPUT_OVERRIDES && ENGINE_MINOR_VERSION >= 8

#include "Core/Requests/McpResponseCaptureRegistry.h"
#include "Domains/NiagaraAuthoring/McpNiagaraCreatePackage.h"
#include "NiagaraDataChannel.h"
#include "NiagaraDataChannelAsset.h"
#include "NiagaraDataChannelVariable.h"
#include "Safety/McpSafeOperations.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"

namespace McpNiagaraAuthoringHandlers
{
namespace
{
// Runs one Niagara authoring command in-process with its reply captured, and records it in Steps.
bool RunNiagaraStep(FActionContext& Context, const FString& Label, const TSharedPtr<FJsonObject>& Payload,
    TArray<TSharedPtr<FJsonValue>>& Steps, TSharedPtr<FJsonObject>* OutResult = nullptr)
{
    const FString StepId = FString::Printf(TEXT("%s#%s"), *Context.RequestId, *Label);
    FMcpResponseCaptureRegistry::Get().Begin(StepId, false);
    FMcpNiagaraHandlerAccess::Niagara(Context.Subsystem, StepId, Payload, Context.RequestingSocket);
    const FMcpCapturedResponse Reply = FMcpResponseCaptureRegistry::Get().End(StepId);
    bool bOk = Reply.bCaptured && Reply.bSuccess;
    // A handler that reports success but a failed compile has not done its job.
    bool bCompileOk = true;
    if (bOk && Reply.Result.IsValid() && Reply.Result->TryGetBoolField(TEXT("compileOk"), bCompileOk) && !bCompileOk)
    {
        bOk = false;
    }
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("step"), Label);
    Row->SetStringField(TEXT("subAction"), GetJsonStringField(Payload, TEXT("subAction")));
    Row->SetBoolField(TEXT("success"), bOk);
    if (!bOk)
    {
        Row->SetStringField(TEXT("message"), Reply.bCaptured ? Reply.Message : TEXT("the step sent no reply"));
        if (!Reply.ErrorCode.IsEmpty()) Row->SetStringField(TEXT("errorCode"), Reply.ErrorCode);
        const TArray<TSharedPtr<FJsonValue>>* Scripts = nullptr;
        if (Reply.Result.IsValid() && Reply.Result->TryGetArrayField(TEXT("compiledScripts"), Scripts))
        {
            Row->SetArrayField(TEXT("compiledScripts"), *Scripts);
        }
    }
    Steps.Add(MakeShared<FJsonValueObject>(Row));
    if (OutResult)
    {
        *OutResult = Reply.Result;
    }
    return bOk;
}

TSharedPtr<FJsonObject> ParseJsonObject(const FString& Text)
{
    TSharedPtr<FJsonObject> Object;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    FJsonSerializer::Deserialize(Reader, Object);
    return Object;
}

TSharedPtr<FJsonObject> StepPayload(const FActionContext& Context, const TCHAR* SubAction)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("subAction"), SubAction);
    Payload->SetStringField(TEXT("systemPath"), Context.SystemPath);
    Payload->SetStringField(TEXT("emitterName"), Context.EmitterName);
    Payload->SetBoolField(TEXT("save"), false);
    return Payload;
}

bool ReadIntVector(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field, FIntVector Default, int32 Min, int32 Max, FIntVector& Out)
{
    Out = Default;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Payload->HasField(Field)) return true;
    if (!Payload->TryGetArrayField(Field, Values) || Values->Num() != 3) return false;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        double Number = 0;
        if (!(*Values)[Axis]->TryGetNumber(Number) || Number < Min || Number > Max || Number != FMath::FloorToDouble(Number)) return false;
        Out[Axis] = static_cast<int32>(Number);
    }
    return true;
}

bool ReadVector(const TSharedPtr<FJsonObject>& Payload, const TCHAR* Field, FVector Default, FVector& Out)
{
    Out = Default;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Payload->HasField(Field)) return true;
    if (!Payload->TryGetArrayField(Field, Values) || Values->Num() != 3) return false;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        double Number = 0;
        if (!(*Values)[Axis]->TryGetNumber(Number) || !(Number > 0)) return false;
        Out[Axis] = Number;
    }
    return true;
}

// The populate pass: a particle's cell from its position relative to the effect, inside the grid's world box.
const TCHAR* PopulateOperations = TEXT(R"JSON({"operations":[
 {"op":"declare_input","name":"Grid","type":"NiagaraDataInterfaceNeighborGrid3D"},
 {"op":"add_node","kind":"map_get","as":"get","pins":[{"name":"Particles.Position","type":"Position"},{"name":"Engine.Owner.Position","type":"Position"}]},
 {"op":"add_node","kind":"custom_hlsl","as":"populate","inputs":[{"name":"Grid","type":"NiagaraDataInterfaceNeighborGrid3D"},{"name":"P","type":"Position"},{"name":"Center","type":"Position"}],
  "outputs":[{"name":"Added","type":"Bool"}],
  "hlsl":"float3 Size; Grid.GetWorldBBoxSize(Size);\nint NX; int NY; int NZ; Grid.GetNumCells(NX, NY, NZ);\nfloat3 Unit = (P - Center) / Size + 0.5;\nint IX = (int)floor(Unit.x * NX); int IY = (int)floor(Unit.y * NY); int IZ = (int)floor(Unit.z * NZ);\nAdded = false;\nif (IX >= 0 && IY >= 0 && IZ >= 0 && IX < NX && IY < NY && IZ < NZ)\n{\n    Grid.AddParticle(IX, IY, IZ, ExecIndex(), Added);\n}"},
 {"op":"add_node","kind":"map_set","as":"set","pins":[{"name":"Particles.InNeighborGrid","type":"Bool"}]},
 {"op":"connect","from":"inputs.Module.Grid","to":"populate.Grid"},
 {"op":"connect","from":"get.Particles.Position","to":"populate.P"},
 {"op":"connect","from":"get.Engine.Owner.Position","to":"populate.Center"},
 {"op":"connect","from":"populate.Added","to":"set.Particles.InNeighborGrid"}]})JSON");

// The query pass: every particle in the 27 cells around this one, read back by buffer index through a particle reader
// of the emitter itself; writes the neighbour count and a separation push (stronger the closer, zero at Radius).
const TCHAR* QueryOperations = TEXT(R"JSON({"operations":[
 {"op":"declare_input","name":"Grid","type":"NiagaraDataInterfaceNeighborGrid3D"},
 {"op":"declare_input","name":"Reader","type":"NiagaraDataInterfaceParticleRead"},
 {"op":"declare_input","name":"Radius","type":"Float"},
 {"op":"add_node","kind":"map_get","as":"get","pins":[{"name":"Particles.Position","type":"Position"},{"name":"Engine.Owner.Position","type":"Position"}]},
 {"op":"add_node","kind":"custom_hlsl","as":"query","inputs":[{"name":"Grid","type":"NiagaraDataInterfaceNeighborGrid3D"},{"name":"Reader","type":"NiagaraDataInterfaceParticleRead"},{"name":"P","type":"Position"},{"name":"Center","type":"Position"},{"name":"Radius","type":"Float"}],
  "outputs":[{"name":"Separation","type":"Vector"},{"name":"NeighborCount","type":"Int"}],
  "hlsl":"float3 Size; Grid.GetWorldBBoxSize(Size);\nint NX; int NY; int NZ; Grid.GetNumCells(NX, NY, NZ);\nint MaxN; Grid.MaxNeighborsPerCell(MaxN);\nfloat3 Unit = (P - Center) / Size + 0.5;\nint CX = (int)floor(Unit.x * NX); int CY = (int)floor(Unit.y * NY); int CZ = (int)floor(Unit.z * NZ);\nint Me = ExecIndex();\nSeparation = float3(0, 0, 0);\nNeighborCount = 0;\nfor (int DX = -1; DX <= 1; ++DX)\nfor (int DY = -1; DY <= 1; ++DY)\nfor (int DZ = -1; DZ <= 1; ++DZ)\n{\n    int IX = CX + DX; int IY = CY + DY; int IZ = CZ + DZ;\n    if (IX >= 0 && IY >= 0 && IZ >= 0 && IX < NX && IY < NY && IZ < NZ)\n    {\n        for (int K = 0; K < MaxN; ++K)\n        {\n            int Linear; Grid.NeighborGridIndexToLinear(IX, IY, IZ, K, Linear);\n            int Other; Grid.GetParticleNeighbor(Linear, Other);\n            if (Other >= 0 && Other != Me)\n            {\n                bool Valid; float3 Q;\n                Reader.GetPositionByIndex<Attribute=\"Position\">(Other, Valid, Q);\n                float3 D = P - Q; float L = length(D);\n                if (Valid && L > 0.0001 && L < Radius)\n                {\n                    Separation += D / L * (Radius - L) / Radius;\n                    NeighborCount += 1;\n                }\n            }\n        }\n    }\n}"},
 {"op":"add_node","kind":"map_set","as":"set","pins":[{"name":"Particles.Separation","type":"Vector"},{"name":"Particles.NeighborCount","type":"Int"}]},
 {"op":"connect","from":"inputs.Module.Grid","to":"query.Grid"},
 {"op":"connect","from":"inputs.Module.Reader","to":"query.Reader"},
 {"op":"connect","from":"inputs.Module.Radius","to":"query.Radius"},
 {"op":"connect","from":"get.Particles.Position","to":"query.P"},
 {"op":"connect","from":"get.Engine.Owner.Position","to":"query.Center"},
 {"op":"connect","from":"query.Separation","to":"set.Particles.Separation"},
 {"op":"connect","from":"query.NeighborCount","to":"set.Particles.NeighborCount"}]})JSON");

bool SetupNeighborGrid(FActionContext& Context)
{
    const FString GridName = GetJsonStringField(Context.Payload, TEXT("gridName"), TEXT("NeighborGrid"));
    FIntVector NumCells;
    FVector BoxSize;
    double MaxNeighbors = 8, Radius = 100;
    Context.Payload->TryGetNumberField(TEXT("maxNeighborsPerCell"), MaxNeighbors);
    Context.Payload->TryGetNumberField(TEXT("radius"), Radius);
    if (!ValidateNiagaraIdentifier(Context, GridName, TEXT("gridName"), false))
    {
        return true;
    }
    if (Context.EmitterName.IsEmpty() || GridName.IsEmpty()
        || !ReadIntVector(Context.Payload, TEXT("numCells"), FIntVector(32, 32, 4), 1, 512, NumCells)
        || !ReadVector(Context.Payload, TEXT("worldBBoxSize"), FVector(6400, 6400, 800), BoxSize)
        || MaxNeighbors < 1 || MaxNeighbors > 64 || MaxNeighbors != FMath::FloorToDouble(MaxNeighbors) || !(Radius > 0))
    {
        Context.SendError(TEXT("Require systemPath and emitterName. numCells: [x,y,z] integers 1..512 (default [32,32,4]); worldBBoxSize: [x,y,z] > 0 cm (default [6400,6400,800]); maxNeighborsPerCell: integer 1..64 (default 8); radius > 0 (default 100). Nothing was changed."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!LoadSystemAndEmitter(Context, System, Handle) || RefuseWhileEditorOpen(Context, System))
    {
        return true;
    }
    const FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
    if (!Data || Data->SimTarget != ENiagaraSimTarget::GPUComputeSim)
    {
        Context.SendError(FString::Printf(TEXT("Emitter '%s' simulates on the CPU; NeighborGrid3D runs on GPU emitters only. Switch it with set_niagara_sim_target target=GPU first. Nothing was changed."), *Context.EmitterName), TEXT("NOT_SUPPORTED"));
        return true;
    }
    const FString PopulateStage = GridName + TEXT("Populate");
    const FString QueryStage = GridName + TEXT("Query");
    // Everything this makes is checked first, so a second run is refused before it adds half a setup.
    TArray<FString> Taken;
    if (HasUserParameterNamed(System, GridName)) Taken.Add(TEXT("user parameter User.") + GridName);
    for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
    {
        const FString Name = Stage ? Stage->SimulationStageName.ToString() : FString();
        if (Name.Equals(PopulateStage, ESearchCase::IgnoreCase) || Name.Equals(QueryStage, ESearchCase::IgnoreCase)) Taken.Add(TEXT("simulation stage ") + Name);
    }
    for (const FString& Script : {PopulateStage, QueryStage})
    {
        if (StaticFindObject(UObject::StaticClass(), System, *Script)) Taken.Add(TEXT("scratch script ") + Script);
    }
    if (Taken.Num() > 0)
    {
        Context.SendError(FString::Printf(TEXT("Already in %s: %s. Pick another gridName. Nothing was changed."), *System->GetName(), *FString::Join(Taken, TEXT(", "))), TEXT("ALREADY_EXISTS"));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Steps;
    const auto Fail = [&](const FString& What)
    {
        Context.Result->SetArrayField(TEXT("steps"), Steps);
        Context.Result->SetBoolField(TEXT("saved"), false);
        Context.Subsystem->SendAutomationResponse(Context.RequestingSocket, Context.RequestId, false,
            FString::Printf(TEXT("Neighbor grid setup stopped at %s; the steps before it stay in memory (not saved). See steps."), *What),
            Context.Result, TEXT("SETUP_STEP_FAILED"));
        return true;
    };

    // 1. The grid, a user parameter both stages share.
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("set_niagara_data_interface"));
        Payload->RemoveField(TEXT("emitterName"));
        Payload->SetStringField(TEXT("userParameter"), GridName);
        Payload->SetStringField(TEXT("dataInterfaceType"), TEXT("NeighborGrid3D"));
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetNumberField(TEXT("MaxNeighborsPerCell"), MaxNeighbors);
        TSharedPtr<FJsonObject> Cells = MakeShared<FJsonObject>();
        Cells->SetNumberField(TEXT("X"), NumCells.X);
        Cells->SetNumberField(TEXT("Y"), NumCells.Y);
        Cells->SetNumberField(TEXT("Z"), NumCells.Z);
        Properties->SetObjectField(TEXT("NumCells"), Cells);
        TArray<TSharedPtr<FJsonValue>> Box{MakeShared<FJsonValueNumber>(BoxSize.X), MakeShared<FJsonValueNumber>(BoxSize.Y), MakeShared<FJsonValueNumber>(BoxSize.Z)};
        Properties->SetArrayField(TEXT("WorldBBoxSize"), Box);
        Properties->SetStringField(TEXT("SetResolutionMethod"), TEXT("Independent"));
        Payload->SetObjectField(TEXT("properties"), Properties);
        if (!RunNiagaraStep(Context, TEXT("grid"), Payload, Steps)) return Fail(TEXT("the grid user parameter"));
    }
    // 2. The two stages, populate first: a stage that writes the grid clears it first (ClearBeforeNonIterationStage).
    for (const FString& Stage : {PopulateStage, QueryStage})
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("add_simulation_stage"));
        Payload->SetStringField(TEXT("stageName"), Stage);
        if (!RunNiagaraStep(Context, TEXT("stage:") + Stage, Payload, Steps)) return Fail(TEXT("simulation stage ") + Stage);
    }
    // 3. A Scratch module in each stage.
    FString Modules[2];
    const TCHAR* Operations[2] = {PopulateOperations, QueryOperations};
    const FString StageNames[2] = {PopulateStage, QueryStage};
    for (int32 Index = 0; Index < 2; ++Index)
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("create_niagara_scratch_module"));
        Payload->SetStringField(TEXT("name"), StageNames[Index]);
        Payload->SetStringField(TEXT("stage"), StageNames[Index]);
        const TSharedPtr<FJsonObject> Ops = ParseJsonObject(Operations[Index]);
        Payload->SetArrayField(TEXT("operations"), Ops->GetArrayField(TEXT("operations")));
        TSharedPtr<FJsonObject> Created;
        if (!RunNiagaraStep(Context, TEXT("module:") + StageNames[Index], Payload, Steps, &Created)) return Fail(TEXT("the scratch module ") + StageNames[Index]);
        Modules[Index] = Created.IsValid() ? GetJsonStringField(Created, TEXT("moduleName"), StageNames[Index]) : StageNames[Index];
    }
    // 4. Compile once so the new module inputs exist, then wire them: both grids to the user parameter, the reader to
    // this emitter, the radius. Then compile again.
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("apply_niagara_scratch_module"));
        if (!RunNiagaraStep(Context, TEXT("compile"), Payload, Steps)) return Fail(TEXT("the first compile"));
    }
    for (const FString& Module : Modules)
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("bind_parameter_to_source"));
        Payload->SetStringField(TEXT("parameterName"), Module + TEXT(".Grid"));
        Payload->SetStringField(TEXT("sourceBinding"), TEXT("User.") + GridName);
        if (!RunNiagaraStep(Context, TEXT("bind:") + Module, Payload, Steps)) return Fail(TEXT("binding ") + Module + TEXT(".Grid"));
    }
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("set_niagara_data_interface"));
        Payload->SetStringField(TEXT("moduleInput"), Modules[1] + TEXT(".Reader"));
        Payload->SetObjectField(TEXT("properties"), ParseJsonObject(TEXT(R"({"EmitterBinding":{"BindingMode":"Self"}})")));
        if (!RunNiagaraStep(Context, TEXT("reader"), Payload, Steps)) return Fail(TEXT("the particle reader"));
    }
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("set_parameter_value"));
        Payload->SetStringField(TEXT("parameterName"), Modules[1] + TEXT(".Radius"));
        Payload->SetNumberField(TEXT("parameterValue"), Radius);
        if (!RunNiagaraStep(Context, TEXT("radius"), Payload, Steps)) return Fail(TEXT("the radius"));
    }
    TSharedPtr<FJsonObject> Compiled;
    {
        TSharedPtr<FJsonObject> Payload = StepPayload(Context, TEXT("apply_niagara_scratch_module"));
        Payload->SetBoolField(TEXT("save"), Context.bSave);
        if (!RunNiagaraStep(Context, TEXT("final compile"), Payload, Steps, &Compiled)) return Fail(TEXT("the final compile"));
    }
    Context.Result->SetArrayField(TEXT("steps"), Steps);
    Context.Result->SetStringField(TEXT("gridParameter"), TEXT("User.") + GridName);
    Context.Result->SetStringField(TEXT("populateStage"), PopulateStage);
    Context.Result->SetStringField(TEXT("queryStage"), QueryStage);
    Context.Result->SetStringField(TEXT("populateModule"), Modules[0]);
    Context.Result->SetStringField(TEXT("queryModule"), Modules[1]);
    TArray<TSharedPtr<FJsonValue>> Writes{MakeShared<FJsonValueString>(TEXT("Particles.InNeighborGrid")),
        MakeShared<FJsonValueString>(TEXT("Particles.NeighborCount")), MakeShared<FJsonValueString>(TEXT("Particles.Separation"))};
    Context.Result->SetArrayField(TEXT("writes"), Writes);
    Context.Result->SetBoolField(TEXT("compileOk"), true);
    bool bSaved = false;
    if (Compiled.IsValid()) Compiled->TryGetBoolField(TEXT("saved"), bSaved);
    Context.Result->SetBoolField(TEXT("saved"), bSaved);
    Context.Result->SetStringField(TEXT("next"), FString::Printf(TEXT("Use Particles.Separation in a later module (e.g. add it to Velocity in Particle Update); tune %s.Radius with set_parameter_value and the cell size with set_niagara_data_interface userParameter=%s. Cells cover worldBBoxSize centred on the effect."), *Modules[1], *GridName));
    Context.SendSuccess(true, TEXT("Neighbor grid set up: populate and query stages compiled."));
    return true;
}

UClass* FindDataChannelClass(const FString& Kind)
{
    const FString ClassName = TEXT("NiagaraDataChannel_") + Kind;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetName().Equals(ClassName, ESearchCase::IgnoreCase) && It->IsChildOf(UNiagaraDataChannel::StaticClass())
            && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
        {
            return *It;
        }
    }
    return nullptr;
}

bool CreateDataChannel(FActionContext& Context)
{
    const FString AssetPath = GetJsonStringField(Context.Payload, TEXT("assetPath"));
    const FString Kind = GetJsonStringField(Context.Payload, TEXT("channelType"), TEXT("Global"));
    const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
    if (AssetPath.IsEmpty() || !Context.Payload->TryGetArrayField(TEXT("variables"), Variables) || Variables->IsEmpty())
    {
        Context.SendError(TEXT("Require assetPath (/Game/.../NDC_Name) and variables [{name, type}] (at least one). Nothing was created."), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    UClass* ChannelClass = FindDataChannelClass(Kind);
    if (!ChannelClass)
    {
        Context.SendError(FString::Printf(TEXT("channelType must be Global or Islands (a loaded UNiagaraDataChannel_<Type> class); got '%s'. Nothing was created."), *Kind), TEXT("INVALID_ARGUMENT"));
        return true;
    }
    TArray<FNiagaraDataChannelVariable> Channel;
    for (const TSharedPtr<FJsonValue>& Value : *Variables)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FString Name, TypeName;
        if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("name"), Name) || !(*Entry)->TryGetStringField(TEXT("type"), TypeName) || Name.IsEmpty())
        {
            Context.SendError(TEXT("Each variables entry is {name, type}. Nothing was created."), TEXT("INVALID_ARGUMENT"));
            return true;
        }
        const FNiagaraTypeDefinition Type = ResolveNiagaraTypeByName(TypeName);
        if (!Type.IsValid() || !FNiagaraDataChannelVariable::IsAllowedType(Type))
        {
            Context.SendError(FString::Printf(TEXT("Variable '%s': type '%s' is not a data channel type. %s Nothing was created."), *Name, *TypeName, NiagaraTypeNamesHelp()), TEXT("INVALID_TYPE"));
            return true;
        }
        if (Channel.ContainsByPredicate([&](const FNiagaraDataChannelVariable& Each) { return Each.GetName() == FName(*Name); }))
        {
            Context.SendError(FString::Printf(TEXT("Variable '%s' is listed twice. Nothing was created."), *Name), TEXT("INVALID_ARGUMENT"));
            return true;
        }
        FNiagaraDataChannelVariable Variable;
        Variable.SetName(FName(*Name));
        Variable.SetType(FNiagaraDataChannelVariable::ToDataChannelType(Type));
        Channel.Add(Variable);
    }
    FString Folder, AssetName, Error;
    AssetPath.Split(TEXT("/"), &Folder, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    AssetName = FPackageName::ObjectPathToObjectName(AssetName);
    const FString PackageName = AssetName.IsEmpty() || !ValidateNiagaraIdentifier(Context, AssetName, TEXT("assetPath"), false)
        ? FString() : McpNiagaraCreatePackageName(Folder, AssetName, Error);
    if (PackageName.IsEmpty())
    {
        if (!Error.IsEmpty() || AssetName.IsEmpty()) Context.SendError(Error.IsEmpty() ? TEXT("assetPath has no asset name. Nothing was created.") : Error, TEXT("INVALID_ARGUMENT"));
        return true;
    }
    if (FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName))
    {
        Context.SendError(FString::Printf(TEXT("%s already exists. Nothing was created."), *PackageName), TEXT("ASSET_ALREADY_EXISTS"));
        return true;
    }
    UPackage* Package = CreatePackage(*PackageName);
    UNiagaraDataChannelAsset* Asset = NewObject<UNiagaraDataChannelAsset>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    UNiagaraDataChannel* DataChannel = NewObject<UNiagaraDataChannel>(Asset, ChannelClass, NAME_None, RF_Transactional);
    // DataChannel and ChannelVariables are private UPROPERTYs: written by reflection, then the channel's own change
    // handler rebuilds its layout as the asset editor does.
    FObjectProperty* ChannelProperty = FindFProperty<FObjectProperty>(UNiagaraDataChannelAsset::StaticClass(), TEXT("DataChannel"));
    FArrayProperty* VariablesProperty = FindFProperty<FArrayProperty>(UNiagaraDataChannel::StaticClass(), TEXT("ChannelVariables"));
    if (!ChannelProperty || !VariablesProperty)
    {
        Context.SendError(TEXT("This engine's data channel classes lack DataChannel/ChannelVariables. Nothing was saved."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    ChannelProperty->SetObjectPropertyValue_InContainer(Asset, DataChannel);
    *VariablesProperty->ContainerPtrToValuePtr<TArray<FNiagaraDataChannelVariable>>(DataChannel) = Channel;
    FPropertyChangedEvent Event(VariablesProperty, EPropertyChangeType::ValueSet);
    DataChannel->PostEditChangeProperty(Event);
    FAssetRegistryModule::AssetCreated(Asset);
    Asset->MarkPackageDirty();
    const bool bSaved = Context.bSave && McpSafeOperations::McpSafeAssetSave(Asset);
    TArray<TSharedPtr<FJsonValue>> Written;
    for (const FNiagaraDataChannelVariable& Variable : DataChannel->GetVariables())
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Variable.GetName().ToString());
        Row->SetStringField(TEXT("type"), Variable.GetType().GetName());
        Written.Add(MakeShared<FJsonValueObject>(Row));
    }
    Context.Result->SetStringField(TEXT("assetPath"), Asset->GetPathName());
    Context.Result->SetStringField(TEXT("channelType"), ChannelClass->GetName());
    Context.Result->SetArrayField(TEXT("variables"), Written);
    Context.Result->SetBoolField(TEXT("saved"), bSaved);
    Context.Result->SetStringField(TEXT("next"), FString::Printf(TEXT("Read it in an emitter with a Scratch module input of type NiagaraDataInterfaceDataChannelRead, then set_niagara_data_interface properties {\"Channel\": \"%s\"}; in HLSL Reader.Num(Count). Write it from a Blueprint with Write To Niagara Data Channel."), *Asset->GetPathName()));
    McpHandlerUtils::AddVerification(Context.Result, Asset);
    Context.SendSuccess(true, FString::Printf(TEXT("Data channel %s created with %d variable(s)."), *AssetName, Written.Num()));
    return true;
}
} // namespace

bool HandleBattleSetupAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("setup_niagara_neighbor_grid")) return SetupNeighborGrid(Context);
    if (SubAction == TEXT("create_niagara_data_channel")) return CreateDataChannel(Context);
    return false;
}
}

#else

namespace McpNiagaraAuthoringHandlers
{
bool HandleBattleSetupAction(FActionContext& Context, const FString& SubAction)
{
    if (SubAction == TEXT("setup_niagara_neighbor_grid") || SubAction == TEXT("create_niagara_data_channel"))
    {
        Context.SendError(TEXT("Built for UE 5.8 only."), TEXT("NOT_SUPPORTED"));
        return true;
    }
    return false;
}
}

#endif
