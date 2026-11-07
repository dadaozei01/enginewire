#include "Domains/NiagaraAuthoring/McpNiagaraContext.h"

namespace McpNiagaraAuthoringHandlers
{
bool AddDataInterfaceUserParameter(UNiagaraSystem* System, const FString& ParamName, UClass* DataInterfaceClass)
{
    if (!System || ParamName.IsEmpty() || !DataInterfaceClass || !DataInterfaceClass->IsChildOf(UNiagaraDataInterface::StaticClass()) || DataInterfaceClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return false;
    }
    FNiagaraUserRedirectionParameterStore& UserStore = System->GetExposedParameters();
    FNiagaraVariable Param{FNiagaraTypeDefinition(DataInterfaceClass), FName(*ParamName)};
    UserStore.AddParameter(Param, true);
    if (!UserStore.FindParameterVariable(Param))
    {
        return false;
    }
    UNiagaraDataInterface* DataInterface = NewObject<UNiagaraDataInterface>(System, DataInterfaceClass,
        MakeUniqueObjectName(System, DataInterfaceClass, FName(*ParamName)), RF_Transactional);
    if (!DataInterface)
    {
        return false;
    }
    UserStore.SetDataInterface(DataInterface, Param);
    return UserStore.GetDataInterface(Param) == DataInterface;
}

bool HasUserParameterNamed(UNiagaraSystem* System, const FString& ParamName)
{
    const FName Plain(*ParamName);
    const FName User(*(TEXT("User.") + ParamName));
    for (const FNiagaraVariableWithOffset& Variable : System->GetExposedParameters().ReadParameterVariables())
    {
        if (Variable.GetName() == Plain || Variable.GetName() == User)
        {
            return true;
        }
    }
    return false;
}

// A placed effect's own colour or rate read back only as the raw bytes of its component's override store. Each user
// parameter of the system is read from the component that runs it (componentName picks one of several), and the
// ones whose value differs from the system's default are listed as overridden.
bool AddPlacedUserParameters(FActionContext& Context, UNiagaraSystem* System)
{
    const FString ActorName = GetJsonStringField(Context.Payload, TEXT("actorName"));
    if (ActorName.IsEmpty()) return true;
    const FString ComponentName = GetJsonStringField(Context.Payload, TEXT("componentName"));
    AActor* Actor = McpHandlerUtils::FindActorByName(ActorName);
    UNiagaraComponent* Component = nullptr;
    TArray<FString> Names;
    for (UNiagaraComponent* Each : TInlineComponentArray<UNiagaraComponent*>(Actor))
    {
        Names.Add(Each->GetName());
        const bool bNamed = ComponentName.IsEmpty() ? Each->GetAsset() == System : Each->GetName().Equals(ComponentName, ESearchCase::IgnoreCase);
        Component = !Component && bNamed ? Each : Component;
    }
    if (!Component)
    {
        Context.SendError(!Actor ? FString::Printf(TEXT("Actor '%s' not found"), *ActorName)
                                 : FString::Printf(TEXT("Actor '%s' has no Niagara component %s; it has: %s"), *ActorName,
                                                   ComponentName.IsEmpty() ? *(TEXT("running ") + System->GetName()) : *ComponentName,
                                                   Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", "))),
                          Actor ? TEXT("COMPONENT_NOT_FOUND") : TEXT("ACTOR_NOT_FOUND"));
        return false;
    }
    TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Overridden;
    TArray<FNiagaraVariable> Params;
    System->GetExposedParameters().GetParameters(Params);
    for (const FNiagaraVariable& Param : Params)
    {
        const TSharedPtr<FJsonValue> Value = ReadNiagaraValue(Component->GetOverrideParameters(), Param);
        const TSharedPtr<FJsonValue> Default = ReadNiagaraValue(System->GetExposedParameters(), Param);
        if (!Value.IsValid()) continue;
        Values->SetField(Param.GetName().ToString(), Value);
        if (!Default.IsValid() || McpHandlerUtils::JsonValueToString(Value) != McpHandlerUtils::JsonValueToString(Default))
        {
            Overridden.Add(MakeShared<FJsonValueString>(Param.GetName().ToString()));
        }
    }
    TSharedPtr<FJsonObject> Placed = McpHandlerUtils::CreateResultObject();
    Placed->SetStringField(TEXT("actorName"), Actor->GetActorLabel());
    Placed->SetStringField(TEXT("componentName"), Component->GetName());
    Placed->SetObjectField(TEXT("values"), Values);
    Placed->SetArrayField(TEXT("overridden"), Overridden);
    Context.Result->SetObjectField(TEXT("placed"), Placed);
    return true;
}

FNiagaraTypeDefinition ResolveNiagaraTypeByName(const FString& ParamType)
{
    const auto Is = [&ParamType](const TCHAR* Name) { return ParamType.Equals(Name, ESearchCase::IgnoreCase); };
    if (Is(TEXT("Float"))) return FNiagaraTypeDefinition::GetFloatDef();
    if (Is(TEXT("Int")) || Is(TEXT("Integer")) || Is(TEXT("Int32"))) return FNiagaraTypeDefinition::GetIntDef();
    if (Is(TEXT("Bool")) || Is(TEXT("Boolean"))) return FNiagaraTypeDefinition::GetBoolDef();
    if (Is(TEXT("Vector")) || Is(TEXT("Vec3")) || Is(TEXT("Vector3"))) return FNiagaraTypeDefinition::GetVec3Def();
    if (Is(TEXT("Vector2")) || Is(TEXT("Vec2"))) return FNiagaraTypeDefinition::GetVec2Def();
    if (Is(TEXT("Vector4")) || Is(TEXT("Vec4"))) return FNiagaraTypeDefinition::GetVec4Def();
    if (Is(TEXT("LinearColor")) || Is(TEXT("Color"))) return FNiagaraTypeDefinition::GetColorDef();
    if (Is(TEXT("Position"))) return FNiagaraTypeDefinition::GetPositionDef();
    if (Is(TEXT("Quat")) || Is(TEXT("Quaternion"))) return FNiagaraTypeDefinition::GetQuatDef();
    return FNiagaraTypeDefinition();
}

const TCHAR* NiagaraTypeNamesHelp()
{
    return TEXT("Use Float, Int, Bool, Vector, Vector2, Vector4, Color, Position or Quat.");
}
}
