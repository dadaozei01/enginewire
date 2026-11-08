#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MCP/Gateway/McpNativeGatewayExecuteReceiptBuild.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpNiagaraRemovalErrorRecovery,
    "EngineWire.Niagara.RemoveUnusedUserParameter.ErrorRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpNiagaraRemovalErrorRecovery::RunTest(const FString&)
{
    const FMcpReceiptContext Context;
    for (const TCHAR* Code : {TEXT("PARAM_NOT_FOUND"), TEXT("PARAMETER_IN_USE")})
    {
        const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("saved"), false);
        const TSharedPtr<FJsonObject> Receipt = McpBuildGatewayExecuteReceipt(
            TEXT("manage_effect.remove_niagara_user_parameter"), nullptr, Context,
            false, TEXT("Parameter removal refused"), Result, Code);
        TestEqual(TEXT("Handler error preserved"), Receipt->GetStringField(TEXT("errorCode")), FString(Code));
        const TSharedPtr<FJsonObject>* NextCall = nullptr;
        const bool HasNext = Receipt->TryGetObjectField(TEXT("nextCall"), NextCall);
        TestTrue(TEXT("Refusal has executable recovery call"), HasNext);
        if (HasNext && NextCall)
        {
            TestEqual(TEXT("Recovery describes before getting fresh consent"), (*NextCall)->GetStringField(TEXT("operation")), FString(TEXT("describe")));
            TestEqual(TEXT("Recovery names the remove action"), (*NextCall)->GetStringField(TEXT("action")), FString(TEXT("remove_niagara_user_parameter")));
            TestFalse(TEXT("No stale consent is reused"), (*NextCall)->HasField(TEXT("consent")));
        }
    }
    return true;
}
#endif
