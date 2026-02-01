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
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutGrassData0)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutGrassData1)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, OutGrassData2)
        SHADER_PARAMETER(int32, GridSize)
        SHADER_PARAMETER(float, Spacing)
        SHADER_PARAMETER(float, JitterStrength)
        SHADER_PARAMETER(int32, NumClumps)
        SHADER_PARAMETER(int32, NumClumpTypes)
        SHADER_PARAMETER(float, PullToCentre)
        SHADER_PARAMETER(float, PointInSameDirection)
        SHADER_PARAMETER(float, BaseHeight)
        SHADER_PARAMETER(float, HeightRandom)
        SHADER_PARAMETER(float, BaseWidth)
        SHADER_PARAMETER(float, WidthRandom)
        SHADER_PARAMETER(float, BaseTilt)
        SHADER_PARAMETER(float, TiltRandom)
        SHADER_PARAMETER(float, BaseBend)
        SHADER_PARAMETER(float, BendRandom)
        SHADER_PARAMETER(float, TaperAmount)
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
    
    // Clump 参数
    int32 CapturedNumClumps = NumClumps;
    int32 CapturedNumClumpTypes = NumClumpTypes;
    float CapturedPullToCentre = ClumpParameters.PullToCentre;
    float CapturedPointInSameDirection = ClumpParameters.PointInSameDirection;
    
    // Blade 参数
    float CapturedBaseHeight = ClumpParameters.BaseHeight;
    float CapturedHeightRandom = ClumpParameters.HeightRandom;
    float CapturedBaseWidth = ClumpParameters.BaseWidth;
    float CapturedWidthRandom = ClumpParameters.WidthRandom;
    float CapturedBaseTilt = ClumpParameters.BaseTilt;
    float CapturedTiltRandom = ClumpParameters.TiltRandom;
    float CapturedBaseBend = ClumpParameters.BaseBend;
    float CapturedBendRandom = ClumpParameters.BendRandom;
    float CapturedTaperAmount = ClumpParameters.TaperAmount;

    UE_LOG(LogTemp, Log, TEXT("Generating %d grass positions on GPU (FrustumCulling=%d, NumClumps=%d)..."), 
        InstanceCount, CapturedEnableFrustumCulling ? 1 : 0, CapturedNumClumps);

    ENQUEUE_RENDER_COMMAND(GenerateGrassPositions)(
        [this, CapturedGridSize, CapturedSpacing, CapturedJitterStrength, 
         CapturedUseIndirectDraw, CapturedEnableFrustumCulling,
         CapturedNumClumps, CapturedNumClumpTypes, CapturedPullToCentre, CapturedPointInSameDirection,
         CapturedBaseHeight, CapturedHeightRandom, CapturedBaseWidth, CapturedWidthRandom,
         CapturedBaseTilt, CapturedTiltRandom, CapturedBaseBend, CapturedBendRandom, CapturedTaperAmount](FRHICommandListImmediate& RHICmdList)
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

            // ========== 创建草叶数据 Buffers ==========
            // GrassData0: Height, Width, Tilt, Bend (float4)
            FRHIBufferCreateDesc Data0Desc = FRHIBufferCreateDesc::CreateStructured(
                TEXT("GrassData0Buffer"),
                Total * sizeof(FVector4f),
                sizeof(FVector4f))
                .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                .SetInitialState(ERHIAccess::UAVCompute);
            FBufferRHIRef GrassData0Buffer = RHICmdList.CreateBuffer(Data0Desc);
            auto Data0UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
            FUnorderedAccessViewRHIRef Data0UAV = RHICmdList.CreateUnorderedAccessView(GrassData0Buffer, Data0UAVDesc);
            auto Data0SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
            GrassDataBufferSRV = RHICmdList.CreateShaderResourceView(GrassData0Buffer, Data0SRVDesc);
            GrassDataBuffer = GrassData0Buffer;

            // GrassData1: TaperAmount, FacingDir.x, FacingDir.y, P1Offset (float4)
            FRHIBufferCreateDesc Data1Desc = FRHIBufferCreateDesc::CreateStructured(
                TEXT("GrassData1Buffer"),
                Total * sizeof(FVector4f),
                sizeof(FVector4f))
                .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                .SetInitialState(ERHIAccess::UAVCompute);
            GrassData1Buffer = RHICmdList.CreateBuffer(Data1Desc);
            auto Data1UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
            FUnorderedAccessViewRHIRef Data1UAV = RHICmdList.CreateUnorderedAccessView(GrassData1Buffer, Data1UAVDesc);
            auto Data1SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
            GrassData1BufferSRV = RHICmdList.CreateShaderResourceView(GrassData1Buffer, Data1SRVDesc);

            // GrassData2: P2Offset (float)
            FRHIBufferCreateDesc Data2Desc = FRHIBufferCreateDesc::CreateStructured(
                TEXT("GrassData2Buffer"),
                Total * sizeof(float),
                sizeof(float))
                .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                .SetInitialState(ERHIAccess::UAVCompute);
            GrassData2Buffer = RHICmdList.CreateBuffer(Data2Desc);
            auto Data2UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
            FUnorderedAccessViewRHIRef Data2UAV = RHICmdList.CreateUnorderedAccessView(GrassData2Buffer, Data2UAVDesc);
            auto Data2SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
            GrassData2BufferSRV = RHICmdList.CreateShaderResourceView(GrassData2Buffer, Data2SRVDesc);

            // ========== 执行位置生成 Compute Shader ==========
            TShaderMapRef<FGrassPositionCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            FGrassPositionCS::FParameters Params;
            Params.OutPositions = UAV;
            Params.OutGrassData0 = Data0UAV;
            Params.OutGrassData1 = Data1UAV;
            Params.OutGrassData2 = Data2UAV;
            Params.GridSize = CapturedGridSize;
            Params.Spacing = CapturedSpacing;
            Params.JitterStrength = CapturedJitterStrength;
            Params.NumClumps = CapturedNumClumps;
            Params.NumClumpTypes = CapturedNumClumpTypes;
            Params.PullToCentre = CapturedPullToCentre;
            Params.PointInSameDirection = CapturedPointInSameDirection;
            Params.BaseHeight = CapturedBaseHeight;
            Params.HeightRandom = CapturedHeightRandom;
            Params.BaseWidth = CapturedBaseWidth;
            Params.WidthRandom = CapturedWidthRandom;
            Params.BaseTilt = CapturedBaseTilt;
            Params.TiltRandom = CapturedTiltRandom;
            Params.BaseBend = CapturedBaseBend;
            Params.BendRandom = CapturedBendRandom;
            Params.TaperAmount = CapturedTaperAmount;

            FComputeShaderUtils::Dispatch(RHICmdList, CS, Params,
                FIntVector(
                    FMath::DivideAndRoundUp(CapturedGridSize, 8),
                    FMath::DivideAndRoundUp(CapturedGridSize, 8),
                    1));

            RHICmdList.Transition(FRHITransitionInfo(PositionBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
            RHICmdList.Transition(FRHITransitionInfo(GrassData0Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
            RHICmdList.Transition(FRHITransitionInfo(GrassData1Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
            RHICmdList.Transition(FRHITransitionInfo(GrassData2Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));

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

                // ========== 创建可见实例属性 Buffers（用于剔除输出）==========
                // VisibleGrassData0: Height, Width, Tilt, Bend (float4)
                {
                    FRHIBufferCreateDesc VisibleData0Desc = FRHIBufferCreateDesc::CreateStructured(
                        TEXT("GrassVisibleData0Buffer"),
                        Total * sizeof(FVector4f),
                        sizeof(FVector4f))
                        .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                        .SetInitialState(ERHIAccess::CopyDest);
                    VisibleGrassData0Buffer = RHICmdList.CreateBuffer(VisibleData0Desc);
                    
                    // Copy initial data
                    RHICmdList.Transition(FRHITransitionInfo(GrassData0Buffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                    RHICmdList.CopyBufferRegion(VisibleGrassData0Buffer, 0, GrassData0Buffer, 0, Total * sizeof(FVector4f));
                    RHICmdList.Transition(FRHITransitionInfo(GrassData0Buffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0Buffer, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
                    
                    auto VisibleData0UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData0BufferUAV = RHICmdList.CreateUnorderedAccessView(VisibleGrassData0Buffer, VisibleData0UAVDesc);
                    auto VisibleData0SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData0BufferSRV = RHICmdList.CreateShaderResourceView(VisibleGrassData0Buffer, VisibleData0SRVDesc);
                }
                
                // VisibleGrassData1: TaperAmount, FacingDir.x, FacingDir.y, P1Offset (float4)
                {
                    FRHIBufferCreateDesc VisibleData1Desc = FRHIBufferCreateDesc::CreateStructured(
                        TEXT("GrassVisibleData1Buffer"),
                        Total * sizeof(FVector4f),
                        sizeof(FVector4f))
                        .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                        .SetInitialState(ERHIAccess::CopyDest);
                    VisibleGrassData1Buffer = RHICmdList.CreateBuffer(VisibleData1Desc);
                    
                    RHICmdList.Transition(FRHITransitionInfo(GrassData1Buffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                    RHICmdList.CopyBufferRegion(VisibleGrassData1Buffer, 0, GrassData1Buffer, 0, Total * sizeof(FVector4f));
                    RHICmdList.Transition(FRHITransitionInfo(GrassData1Buffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1Buffer, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
                    
                    auto VisibleData1UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData1BufferUAV = RHICmdList.CreateUnorderedAccessView(VisibleGrassData1Buffer, VisibleData1UAVDesc);
                    auto VisibleData1SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData1BufferSRV = RHICmdList.CreateShaderResourceView(VisibleGrassData1Buffer, VisibleData1SRVDesc);
                }
                
                // VisibleGrassData2: P2Offset (float)
                {
                    FRHIBufferCreateDesc VisibleData2Desc = FRHIBufferCreateDesc::CreateStructured(
                        TEXT("GrassVisibleData2Buffer"),
                        Total * sizeof(float),
                        sizeof(float))
                        .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                        .SetInitialState(ERHIAccess::CopyDest);
                    VisibleGrassData2Buffer = RHICmdList.CreateBuffer(VisibleData2Desc);
                    
                    RHICmdList.Transition(FRHITransitionInfo(GrassData2Buffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                    RHICmdList.CopyBufferRegion(VisibleGrassData2Buffer, 0, GrassData2Buffer, 0, Total * sizeof(float));
                    RHICmdList.Transition(FRHITransitionInfo(GrassData2Buffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2Buffer, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
                    
                    auto VisibleData2UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData2BufferUAV = RHICmdList.CreateUnorderedAccessView(VisibleGrassData2Buffer, VisibleData2UAVDesc);
                    auto VisibleData2SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData2BufferSRV = RHICmdList.CreateShaderResourceView(VisibleGrassData2Buffer, VisibleData2SRVDesc);
                }

                UE_LOG(LogTemp, Log, TEXT("Created Visible Buffers for GPU Culling (initialized with all %d instances)"), Total);
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
                IndirectArgs[0] = 39;    // IndexCountPerInstance (15 vertices, 13 triangles = 39 indices)
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
