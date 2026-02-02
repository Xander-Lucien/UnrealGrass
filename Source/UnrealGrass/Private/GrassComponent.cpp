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
        // Voronoi Texture 输入 (用于 O(1) 查找最近 Clump)
        SHADER_PARAMETER_SRV(Texture2D<float4>, InVoronoiTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, InVoronoiTextureSampler)
        // Clump Buffer 输入
        SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, InClumpData0) // Centre.xy, Direction.xy
        SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, InClumpData1) // HeightScale, WidthScale, WindPhase, Padding
        // 输出 Buffers
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
// Compute Shader 定义 - Clump 生成
// ============================================================================
class FClumpGenerationCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FClumpGenerationCS);
    SHADER_USE_PARAMETER_STRUCT(FClumpGenerationCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, OutClumpData0) // Centre.xy, Direction.xy
        SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, OutClumpData1) // HeightScale, WidthScale, WindPhase, Padding
        SHADER_PARAMETER(int32, NumClumps)
        SHADER_PARAMETER(float, HeightVariation)
        SHADER_PARAMETER(float, WidthVariation)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FClumpGenerationCS, "/Plugin/UnrealGrass/Private/GrassClumpCS.usf", "MainCS", SF_Compute);

// ============================================================================
// Compute Shader 定义 - Voronoi Texture 生成
// ============================================================================
class FVoronoiGenerationCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FVoronoiGenerationCS);
    SHADER_USE_PARAMETER_STRUCT(FVoronoiGenerationCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, InClumpData0) // Centre.xy, Direction.xy
        SHADER_PARAMETER_UAV(RWTexture2D<float4>, OutVoronoiTexture)
        SHADER_PARAMETER(int32, NumClumps)
        SHADER_PARAMETER(int32, TextureSize)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FVoronoiGenerationCS, "/Plugin/UnrealGrass/Private/GrassVoronoiCS.usf", "MainCS", SF_Compute);

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
    int32 CapturedVoronoiTextureSize = VoronoiTextureSize;
    float CapturedPullToCentre = ClumpParameters.PullToCentre;
    float CapturedPointInSameDirection = ClumpParameters.PointInSameDirection;
    float CapturedClumpHeightVariation = ClumpHeightVariation;
    float CapturedClumpWidthVariation = ClumpWidthVariation;
    
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

    UE_LOG(LogTemp, Log, TEXT("Generating %d grass positions on GPU (FrustumCulling=%d, NumClumps=%d, VoronoiSize=%d)..."), 
        InstanceCount, CapturedEnableFrustumCulling ? 1 : 0, CapturedNumClumps, CapturedVoronoiTextureSize);

    ENQUEUE_RENDER_COMMAND(GenerateGrassPositions)(
        [this, CapturedGridSize, CapturedSpacing, CapturedJitterStrength, 
         CapturedUseIndirectDraw, CapturedEnableFrustumCulling,
         CapturedNumClumps, CapturedNumClumpTypes, CapturedVoronoiTextureSize, CapturedPullToCentre, CapturedPointInSameDirection,
         CapturedClumpHeightVariation, CapturedClumpWidthVariation,
         CapturedBaseHeight, CapturedHeightRandom, CapturedBaseWidth, CapturedWidthRandom,
         CapturedBaseTilt, CapturedTiltRandom, CapturedBaseBend, CapturedBendRandom, CapturedTaperAmount](FRHICommandListImmediate& RHICmdList)
        {
            int32 Total = CapturedGridSize * CapturedGridSize;

            // ========== 创建 Clump Buffer ==========
            // ClumpData 使用两个 float4 来存储:
            // ClumpData0: Centre.x, Centre.y, Direction.x, Direction.y
            // ClumpData1: HeightScale, WidthScale, WindPhase, Padding
            {
                // 创建 ClumpData0 Buffer
                FRHIBufferCreateDesc ClumpData0Desc = FRHIBufferCreateDesc::CreateStructured(
                    TEXT("GrassClumpData0Buffer"),
                    CapturedNumClumps * sizeof(FVector4f),
                    sizeof(FVector4f))
                    .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                    .SetInitialState(ERHIAccess::UAVCompute);
                FBufferRHIRef ClumpData0Buffer = RHICmdList.CreateBuffer(ClumpData0Desc);
                
                // 创建 ClumpData1 Buffer
                FRHIBufferCreateDesc ClumpData1Desc = FRHIBufferCreateDesc::CreateStructured(
                    TEXT("GrassClumpData1Buffer"),
                    CapturedNumClumps * sizeof(FVector4f),
                    sizeof(FVector4f))
                    .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                    .SetInitialState(ERHIAccess::UAVCompute);
                FBufferRHIRef ClumpData1Buffer = RHICmdList.CreateBuffer(ClumpData1Desc);

                // 创建 UAV
                auto ClumpData0UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(CapturedNumClumps);
                FUnorderedAccessViewRHIRef ClumpData0UAV = RHICmdList.CreateUnorderedAccessView(ClumpData0Buffer, ClumpData0UAVDesc);
                auto ClumpData1UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(CapturedNumClumps);
                FUnorderedAccessViewRHIRef ClumpData1UAV = RHICmdList.CreateUnorderedAccessView(ClumpData1Buffer, ClumpData1UAVDesc);

                // 执行 Clump 生成 Compute Shader
                TShaderMapRef<FClumpGenerationCS> ClumpCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                FClumpGenerationCS::FParameters ClumpParams;
                ClumpParams.OutClumpData0 = ClumpData0UAV;
                ClumpParams.OutClumpData1 = ClumpData1UAV;
                ClumpParams.NumClumps = CapturedNumClumps;
                ClumpParams.HeightVariation = CapturedClumpHeightVariation;
                ClumpParams.WidthVariation = CapturedClumpWidthVariation;

                FComputeShaderUtils::Dispatch(RHICmdList, ClumpCS, ClumpParams,
                    FIntVector(FMath::DivideAndRoundUp(CapturedNumClumps, 64), 1, 1));

                // 转换到 SRV 状态
                RHICmdList.Transition(FRHITransitionInfo(ClumpData0Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
                RHICmdList.Transition(FRHITransitionInfo(ClumpData1Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));

                // 创建 SRV
                auto ClumpSRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(CapturedNumClumps);
                ClumpBufferSRV = RHICmdList.CreateShaderResourceView(ClumpData0Buffer, ClumpSRVDesc);
                ClumpBuffer = ClumpData0Buffer;
                
                // 创建 ClumpData1 的 SRV
                auto Clump1SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(CapturedNumClumps);
                ClumpData1BufferSRV = RHICmdList.CreateShaderResourceView(ClumpData1Buffer, Clump1SRVDesc);
                this->ClumpData1Buffer = ClumpData1Buffer;

                UE_LOG(LogTemp, Log, TEXT("Created ClumpBuffer with %d clumps (HeightVar=%.2f, WidthVar=%.2f)"), 
                    CapturedNumClumps, CapturedClumpHeightVariation, CapturedClumpWidthVariation);
            }

            // ========== 创建 Voronoi Texture (预计算 Clump 查找表) ==========
            // 使用 Compute Shader 渲染 Voronoi 图到纹理，后续草叶生成时 O(1) 采样
            {
                // 创建 Voronoi Texture (RGBA32F 格式)
                const FRHITextureCreateDesc VoronoiTextureDesc = FRHITextureCreateDesc::Create2D(
                    TEXT("GrassVoronoiTexture"),
                    CapturedVoronoiTextureSize,
                    CapturedVoronoiTextureSize,
                    PF_A32B32G32R32F)  // 32位浮点精度确保 Clump Index 准确
                    .SetFlags(ETextureCreateFlags::UAV | ETextureCreateFlags::ShaderResource)
                    .SetInitialState(ERHIAccess::UAVCompute);
                
                VoronoiTexture = RHICmdList.CreateTexture(VoronoiTextureDesc);
                
                // 创建 UAV 用于 Voronoi CS 写入 (使用新版 FRHIViewDesc API)
                VoronoiTextureUAV = RHICmdList.CreateUnorderedAccessView(VoronoiTexture, FRHIViewDesc::CreateTextureUAV().SetDimensionFromTexture(VoronoiTexture));
                
                // 执行 Voronoi 生成 Compute Shader
                TShaderMapRef<FVoronoiGenerationCS> VoronoiCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
                FVoronoiGenerationCS::FParameters VoronoiParams;
                VoronoiParams.InClumpData0 = ClumpBufferSRV;
                VoronoiParams.OutVoronoiTexture = VoronoiTextureUAV;
                VoronoiParams.NumClumps = CapturedNumClumps;
                VoronoiParams.TextureSize = CapturedVoronoiTextureSize;
                
                FComputeShaderUtils::Dispatch(RHICmdList, VoronoiCS, VoronoiParams,
                    FIntVector(
                        FMath::DivideAndRoundUp(CapturedVoronoiTextureSize, 8),
                        FMath::DivideAndRoundUp(CapturedVoronoiTextureSize, 8),
                        1));
                
                // 转换到 SRV 状态供后续采样
                RHICmdList.Transition(FRHITransitionInfo(VoronoiTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
                
                // 创建 SRV (使用新版 FRHIViewDesc API)
                VoronoiTextureSRV = RHICmdList.CreateShaderResourceView(VoronoiTexture, FRHIViewDesc::CreateTextureSRV().SetDimensionFromTexture(VoronoiTexture));
                
                UE_LOG(LogTemp, Log, TEXT("Created Voronoi Texture (%dx%d) for O(1) Clump lookup"), 
                    CapturedVoronoiTextureSize, CapturedVoronoiTextureSize);
            }

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
            // Voronoi Texture 输入 (用于 O(1) 查找最近 Clump)
            Params.InVoronoiTexture = VoronoiTextureSRV;
            Params.InVoronoiTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
            // ClumpBuffer 输入
            Params.InClumpData0 = ClumpBufferSRV;
            Params.InClumpData1 = ClumpData1BufferSRV;
            // 输出 Buffers
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
                    
                    // Copy initial data
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
                    
                    // Copy initial data
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

            // ========== 创建 Indirect Draw Args Buffer (LOD 0 - 15 顶点, 39 索引) ==========
            if (CapturedUseIndirectDraw)
            {
                const uint32 IndirectArgsSize = 5 * sizeof(uint32);
                
                // LOD 0 IndirectArgsBuffer
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
                
                // 初始化 Indirect Args for LOD 0
                RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::CopyDest));
                uint32* IndirectArgs = (uint32*)RHICmdList.LockBuffer(IndirectArgsBuffer, 0, IndirectArgsSize, RLM_WriteOnly);
                IndirectArgs[0] = 39;    // IndexCountPerInstance (15 vertices, 13 triangles = 39 indices)
                IndirectArgs[1] = Total; // InstanceCount
                IndirectArgs[2] = 0;     // StartIndexLocation
                IndirectArgs[3] = 0;     // BaseVertexLocation
                IndirectArgs[4] = 0;     // StartInstanceLocation
                RHICmdList.UnlockBuffer(IndirectArgsBuffer);
                RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::CopyDest, ERHIAccess::IndirectArgs));
                
                UE_LOG(LogTemp, Log, TEXT("Created IndirectArgsBuffer (LOD 0) with UAV for GPU Culling"));

                // ========== 创建 LOD 1 的 Indirect Draw Args Buffer (7 顶点, 15 索引) ==========
                FRHIBufferCreateDesc IndirectDescLOD1 = FRHIBufferCreateDesc::Create(
                    TEXT("GrassIndirectArgsBufferLOD1"),
                    IndirectArgsSize,
                    sizeof(uint32),
                    EBufferUsageFlags::DrawIndirect | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource)
                    .SetInitialState(ERHIAccess::IndirectArgs);
                
                IndirectArgsBufferLOD1 = RHICmdList.CreateBuffer(IndirectDescLOD1);

                // 创建 UAV 用于 Culling Shader 写入
                auto IndirectUAVDescLOD1 = FRHIViewDesc::CreateBufferUAV()
                    .SetType(FRHIViewDesc::EBufferType::Raw);
                IndirectArgsBufferLOD1UAV = RHICmdList.CreateUnorderedAccessView(IndirectArgsBufferLOD1, IndirectUAVDescLOD1);
                
                // 初始化 Indirect Args for LOD 1
                RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::IndirectArgs, ERHIAccess::CopyDest));
                uint32* IndirectArgsLOD1 = (uint32*)RHICmdList.LockBuffer(IndirectArgsBufferLOD1, 0, IndirectArgsSize, RLM_WriteOnly);
                IndirectArgsLOD1[0] = 15;    // IndexCountPerInstance (7 vertices, 5 triangles = 15 indices)
                IndirectArgsLOD1[1] = 0;     // InstanceCount (starts at 0, filled by culling shader)
                IndirectArgsLOD1[2] = 0;     // StartIndexLocation
                IndirectArgsLOD1[3] = 0;     // BaseVertexLocation
                IndirectArgsLOD1[4] = 0;     // StartInstanceLocation (LOD 1 从 index 0 开始)
                RHICmdList.UnlockBuffer(IndirectArgsBufferLOD1);
                RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::CopyDest, ERHIAccess::IndirectArgs));
                
                UE_LOG(LogTemp, Log, TEXT("Created IndirectArgsBufferLOD1 with UAV for GPU Culling"));

                // ========== 创建 LOD 1 的独立 Visible Buffers ==========
                // LOD 1 使用独立的 buffer，从 index 0 开始存储，避免与 LOD 0 冲突
                
                // VisiblePositionBufferLOD1
                {
                    FRHIBufferCreateDesc VisibleDescLOD1 = FRHIBufferCreateDesc::CreateStructured(
                        TEXT("GrassVisiblePositionBufferLOD1"),
                        Total * sizeof(FVector3f),
                        sizeof(FVector3f))
                        .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                        .SetInitialState(ERHIAccess::CopyDest);

                    VisiblePositionBufferLOD1 = RHICmdList.CreateBuffer(VisibleDescLOD1);

                    // Copy initial data to ensure valid data before GPU Culling runs
                    RHICmdList.Transition(FRHITransitionInfo(PositionBuffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                    RHICmdList.CopyBufferRegion(VisiblePositionBufferLOD1, 0, PositionBuffer, 0, Total * sizeof(FVector3f));
                    RHICmdList.Transition(FRHITransitionInfo(PositionBuffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                    RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBufferLOD1, ERHIAccess::CopyDest, ERHIAccess::SRVMask));

                    auto VisibleLOD1UAVDesc = FRHIViewDesc::CreateBufferUAV()
                        .SetType(FRHIViewDesc::EBufferType::Structured)
                        .SetNumElements(Total);
                    VisiblePositionBufferLOD1UAV = RHICmdList.CreateUnorderedAccessView(VisiblePositionBufferLOD1, VisibleLOD1UAVDesc);

                    auto VisibleLOD1SRVDesc = FRHIViewDesc::CreateBufferSRV()
                        .SetType(FRHIViewDesc::EBufferType::Structured)
                        .SetNumElements(Total);
                    VisiblePositionBufferLOD1SRV = RHICmdList.CreateShaderResourceView(VisiblePositionBufferLOD1, VisibleLOD1SRVDesc);
                }

                // VisibleGrassData0BufferLOD1
                {
                    FRHIBufferCreateDesc VisibleData0DescLOD1 = FRHIBufferCreateDesc::CreateStructured(
                        TEXT("GrassVisibleData0BufferLOD1"),
                        Total * sizeof(FVector4f),
                        sizeof(FVector4f))
                        .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                        .SetInitialState(ERHIAccess::CopyDest);
                    VisibleGrassData0BufferLOD1 = RHICmdList.CreateBuffer(VisibleData0DescLOD1);
                    
                    // Copy initial data
                    RHICmdList.Transition(FRHITransitionInfo(GrassData0Buffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                    RHICmdList.CopyBufferRegion(VisibleGrassData0BufferLOD1, 0, GrassData0Buffer, 0, Total * sizeof(FVector4f));
                    RHICmdList.Transition(FRHITransitionInfo(GrassData0Buffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0BufferLOD1, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
                    
                    auto VisibleData0LOD1UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData0BufferLOD1UAV = RHICmdList.CreateUnorderedAccessView(VisibleGrassData0BufferLOD1, VisibleData0LOD1UAVDesc);
                    auto VisibleData0LOD1SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData0BufferLOD1SRV = RHICmdList.CreateShaderResourceView(VisibleGrassData0BufferLOD1, VisibleData0LOD1SRVDesc);
                }
                
                // VisibleGrassData1BufferLOD1
                {
                    FRHIBufferCreateDesc VisibleData1DescLOD1 = FRHIBufferCreateDesc::CreateStructured(
                        TEXT("GrassVisibleData1BufferLOD1"),
                        Total * sizeof(FVector4f),
                        sizeof(FVector4f))
                        .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                        .SetInitialState(ERHIAccess::CopyDest);
                    VisibleGrassData1BufferLOD1 = RHICmdList.CreateBuffer(VisibleData1DescLOD1);
                    
                    // Copy initial data
                    RHICmdList.Transition(FRHITransitionInfo(GrassData1Buffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                    RHICmdList.CopyBufferRegion(VisibleGrassData1BufferLOD1, 0, GrassData1Buffer, 0, Total * sizeof(FVector4f));
                    RHICmdList.Transition(FRHITransitionInfo(GrassData1Buffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1BufferLOD1, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
                    
                    auto VisibleData1LOD1UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData1BufferLOD1UAV = RHICmdList.CreateUnorderedAccessView(VisibleGrassData1BufferLOD1, VisibleData1LOD1UAVDesc);
                    auto VisibleData1LOD1SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData1BufferLOD1SRV = RHICmdList.CreateShaderResourceView(VisibleGrassData1BufferLOD1, VisibleData1LOD1SRVDesc);
                }
                
                // VisibleGrassData2BufferLOD1
                {
                    FRHIBufferCreateDesc VisibleData2DescLOD1 = FRHIBufferCreateDesc::CreateStructured(
                        TEXT("GrassVisibleData2BufferLOD1"),
                        Total * sizeof(float),
                        sizeof(float))
                        .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::SourceCopy)
                        .SetInitialState(ERHIAccess::CopyDest);
                    VisibleGrassData2BufferLOD1 = RHICmdList.CreateBuffer(VisibleData2DescLOD1);
                    
                    // Copy initial data
                    RHICmdList.Transition(FRHITransitionInfo(GrassData2Buffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
                    RHICmdList.CopyBufferRegion(VisibleGrassData2BufferLOD1, 0, GrassData2Buffer, 0, Total * sizeof(float));
                    RHICmdList.Transition(FRHITransitionInfo(GrassData2Buffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
                    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2BufferLOD1, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
                    
                    auto VisibleData2LOD1UAVDesc = FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData2BufferLOD1UAV = RHICmdList.CreateUnorderedAccessView(VisibleGrassData2BufferLOD1, VisibleData2LOD1UAVDesc);
                    auto VisibleData2LOD1SRVDesc = FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetNumElements(Total);
                    VisibleGrassData2BufferLOD1SRV = RHICmdList.CreateShaderResourceView(VisibleGrassData2BufferLOD1, VisibleData2LOD1SRVDesc);
                }

                UE_LOG(LogTemp, Log, TEXT("Created LOD 1 independent Visible Buffers (initialized with all %d instances)"), Total);
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
