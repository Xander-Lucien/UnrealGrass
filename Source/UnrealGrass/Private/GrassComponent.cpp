// GrassComponent.cpp
// Compute Shader 生成草位置

#include "GrassComponent.h"
#include "GrassSceneProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "Materials/Material.h"

// ============================================================================
// Compute Shader 定义
// ============================================================================
class FGrassPositionCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGrassPositionCS);
    SHADER_USE_PARAMETER_STRUCT(FGrassPositionCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector3f>, OutPositions)
        SHADER_PARAMETER(int32, GridSize)
        SHADER_PARAMETER(float, Spacing)
        SHADER_PARAMETER(float, JitterStrength)
    END_SHADER_PARAMETER_STRUCT()


    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FGrassPositionCS, "/Plugin/UnrealGrass/Private/GrassPositionCS.usf", "MainCS", SF_Compute);

// ============================================================================
// UGrassComponent 实现
// ============================================================================
UGrassComponent::UGrassComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    // 确保组件在游戏中可见
    bWantsInitializeComponent = true;
}

void UGrassComponent::BeginPlay()
{
    Super::BeginPlay();
    
    // 游戏开始时自动生成草地
    if (InstanceCount == 0)
    {
        UE_LOG(LogTemp, Log, TEXT("GrassComponent::BeginPlay - Auto generating grass..."));
        GenerateGrass();
    }
}

void UGrassComponent::OnRegister()
{
    Super::OnRegister();
    
    // 编辑器中注册时，如果还没有生成数据则自动生成
    // 这确保了在编辑器 viewport 中也能看到草地
    if (GetWorld() && GetWorld()->IsGameWorld() == false)
    {
        // 仅在编辑器中，且数据为空时自动生成
        if (InstanceCount == 0)
        {
            UE_LOG(LogTemp, Log, TEXT("GrassComponent::OnRegister - Auto generating grass in editor..."));
            GenerateGrass();
        }
    }
}

void UGrassComponent::GenerateGrass()
{
    InstanceCount = GridSize * GridSize;
    int32 CapturedGridSize = GridSize;
    float CapturedSpacing = Spacing;
    float CapturedJitterStrength = JitterStrength;


    UE_LOG(LogTemp, Log, TEXT("Generating %d grass positions on GPU..."), InstanceCount);

    ENQUEUE_RENDER_COMMAND(GenerateGrassPositions)(
        [this, CapturedGridSize, CapturedSpacing, CapturedJitterStrength](FRHICommandListImmediate& RHICmdList)

        {
            int32 Total = CapturedGridSize * CapturedGridSize;

            // 创建 StructuredBuffer
            FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateStructured(
                TEXT("GrassPositionBuffer"),
                Total * sizeof(FVector3f),
                sizeof(FVector3f))
                .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                .SetInitialState(ERHIAccess::UAVCompute);

            PositionBuffer = RHICmdList.CreateBuffer(Desc);

            // UAV
            auto UAVDesc = FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetNumElements(Total);
            FUnorderedAccessViewRHIRef UAV = RHICmdList.CreateUnorderedAccessView(PositionBuffer, UAVDesc);

            // SRV (给渲染用)
            auto SRVDesc = FRHIViewDesc::CreateBufferSRV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetNumElements(Total);
            PositionBufferSRV = RHICmdList.CreateShaderResourceView(PositionBuffer, SRVDesc);

            // 执行 Compute Shader
            TShaderMapRef<FGrassPositionCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FGrassPositionCS::FParameters Params;
            Params.OutPositions = UAV;
            Params.GridSize = CapturedGridSize;
            Params.Spacing = CapturedSpacing;
            Params.JitterStrength = CapturedJitterStrength;


            FComputeShaderUtils::Dispatch(RHICmdList, CS, Params,
                FIntVector(
                    FMath::DivideAndRoundUp(CapturedGridSize, 8),
                    FMath::DivideAndRoundUp(CapturedGridSize, 8),
                    1));

            // 转换到 SRV 状态
            RHICmdList.Transition(FRHITransitionInfo(PositionBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        }
    );

    FlushRenderingCommands();
    MarkRenderStateDirty();

    UE_LOG(LogTemp, Log, TEXT("Done. %d grass instances ready."), InstanceCount);
}

FPrimitiveSceneProxy* UGrassComponent::CreateSceneProxy()
{
    if (InstanceCount == 0 || !PositionBufferSRV.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("CreateSceneProxy: InstanceCount=%d, SRV Valid=%d"), InstanceCount, PositionBufferSRV.IsValid());
        return nullptr;
    }
    UE_LOG(LogTemp, Log, TEXT("CreateSceneProxy: Creating FGrassSceneProxy"));
    return new FGrassSceneProxy(this);
}

FBoxSphereBounds UGrassComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    // 边界包含所有草叶：网格大小 + 草叶高度（50）
    float HalfSize = GridSize * Spacing * 0.5f + 100.0f;
    FBox Box(FVector(-HalfSize, -HalfSize, -10), FVector(HalfSize, HalfSize, 100));
    return FBoxSphereBounds(Box).TransformBy(LocalToWorld);
}

void UGrassComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
    if (GrassMaterial)
    {
        OutMaterials.Add(GrassMaterial);
    }
}
