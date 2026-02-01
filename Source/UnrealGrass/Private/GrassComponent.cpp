// GrassComponent.cpp
// Compute Shader 生成草位置 + GPU Frustum Culling

#include "GrassComponent.h"
#include "GrassSceneProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "Materials/Material.h"

// ============================================================================
// Compute Shader 定义 - 位置生成
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
    bWantsInitializeComponent = true;
}

void UGrassComponent::BeginPlay()
{
    Super::BeginPlay();
    
    if (InstanceCount == 0)
    {
        UE_LOG(LogTemp, Log, TEXT("GrassComponent::BeginPlay - Auto generating grass..."));
        GenerateGrass();
    }
}

void UGrassComponent::OnRegister()
{
    Super::OnRegister();
    
    if (GetWorld() && GetWorld()->IsGameWorld() == false)
    {
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
    bool CapturedUseIndirectDraw = bUseIndirectDraw;
    bool CapturedEnableFrustumCulling = bEnableFrustumCulling;

    UE_LOG(LogTemp, Log, TEXT("Generating %d grass positions on GPU (FrustumCulling=%d)..."), 
        InstanceCount, CapturedEnableFrustumCulling ? 1 : 0);

    ENQUEUE_RENDER_COMMAND(GenerateGrassPositions)(
        [this, CapturedGridSize, CapturedSpacing, CapturedJitterStrength, 
         CapturedUseIndirectDraw, CapturedEnableFrustumCulling](FRHICommandListImmediate& RHICmdList)
        {
            int32 Total = CapturedGridSize * CapturedGridSize;

            // ========== 创建所有实例位置的 StructuredBuffer ==========
            FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateStructured(
                TEXT("GrassPositionBuffer"),
                Total * sizeof(FVector3f),
                sizeof(FVector3f))
                .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                .SetInitialState(ERHIAccess::UAVCompute);

            PositionBuffer = RHICmdList.CreateBuffer(Desc);

            // UAV for compute shader
            auto UAVDesc = FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetNumElements(Total);
            FUnorderedAccessViewRHIRef UAV = RHICmdList.CreateUnorderedAccessView(PositionBuffer, UAVDesc);

            // SRV for culling shader input
            auto SRVDesc = FRHIViewDesc::CreateBufferSRV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetNumElements(Total);
            PositionBufferSRV = RHICmdList.CreateShaderResourceView(PositionBuffer, SRVDesc);

            // ========== 执行位置生成 Compute Shader ==========
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

            RHICmdList.Transition(FRHITransitionInfo(PositionBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));

            // ========== 创建可见实例位置 Buffer（用于剔除输出）==========
            if (CapturedEnableFrustumCulling || CapturedUseIndirectDraw)
            {
                FRHIBufferCreateDesc VisibleDesc = FRHIBufferCreateDesc::CreateStructured(
                    TEXT("GrassVisiblePositionBuffer"),
                    Total * sizeof(FVector3f),
                    sizeof(FVector3f))
                    .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                    .SetInitialState(ERHIAccess::CopyDest);

                VisiblePositionBuffer = RHICmdList.CreateBuffer(VisibleDesc);

                // Copy all positions from PositionBuffer to VisiblePositionBuffer as initial data
                // This ensures rendering works even before culling is executed
                RHICmdList.Transition(FRHITransitionInfo(PositionBuffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                RHICmdList.CopyBufferRegion(VisiblePositionBuffer, 0, PositionBuffer, 0, Total * sizeof(FVector3f));
                RHICmdList.Transition(FRHITransitionInfo(PositionBuffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::CopyDest, ERHIAccess::SRVMask));

                // UAV for culling output
                auto VisibleUAVDesc = FRHIViewDesc::CreateBufferUAV()
                    .SetType(FRHIViewDesc::EBufferType::Structured)
                    .SetNumElements(Total);
                VisiblePositionBufferUAV = RHICmdList.CreateUnorderedAccessView(VisiblePositionBuffer, VisibleUAVDesc);

                // SRV for rendering
                auto VisibleSRVDesc = FRHIViewDesc::CreateBufferSRV()
                    .SetType(FRHIViewDesc::EBufferType::Structured)
                    .SetNumElements(Total);
                VisiblePositionBufferSRV = RHICmdList.CreateShaderResourceView(VisiblePositionBuffer, VisibleSRVDesc);

                UE_LOG(LogTemp, Log, TEXT("Created VisiblePositionBuffer for GPU Culling (initialized with all %d positions)"), Total);
            }

            // ========== 创建 Indirect Draw Args Buffer ==========
            if (CapturedUseIndirectDraw)
            {
                const uint32 IndirectArgsSize = 5 * sizeof(uint32);
                
                FRHIBufferCreateDesc IndirectDesc = FRHIBufferCreateDesc::Create(
                    TEXT("GrassIndirectArgsBuffer"),
                    IndirectArgsSize,
                    sizeof(uint32),
                    EBufferUsageFlags::DrawIndirect | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                    .SetInitialState(ERHIAccess::IndirectArgs);
                
                IndirectArgsBuffer = RHICmdList.CreateBuffer(IndirectDesc);

                // 创建 UAV 用于 Culling Shader 写入
                auto IndirectUAVDesc = FRHIViewDesc::CreateBufferUAV()
                    .SetType(FRHIViewDesc::EBufferType::Raw);
                IndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(IndirectArgsBuffer, IndirectUAVDesc);
                
                // 初始化 Indirect Args
                RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::CopyDest));
                uint32* IndirectArgs = (uint32*)RHICmdList.LockBuffer(IndirectArgsBuffer, 0, IndirectArgsSize, RLM_WriteOnly);
                IndirectArgs[0] = 3;     // IndexCountPerInstance (默认三角形)
                IndirectArgs[1] = Total; // InstanceCount
                IndirectArgs[2] = 0;     // StartIndexLocation
                IndirectArgs[3] = 0;     // BaseVertexLocation
                IndirectArgs[4] = 0;     // StartInstanceLocation
                RHICmdList.UnlockBuffer(IndirectArgsBuffer);
                RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::CopyDest, ERHIAccess::IndirectArgs));
                
                UE_LOG(LogTemp, Log, TEXT("Created IndirectArgsBuffer with UAV for GPU Culling"));
            }
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
    UE_LOG(LogTemp, Log, TEXT("CreateSceneProxy: Creating FGrassSceneProxy with GPU Culling=%d"), bEnableFrustumCulling ? 1 : 0);
    return new FGrassSceneProxy(this);
}

FBoxSphereBounds UGrassComponent::CalcBounds(const FTransform& LocalToWorld) const
{
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
