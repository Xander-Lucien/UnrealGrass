// GrassCullingViewExtension.cpp
// Scene View Extension implementation for GPU Culling with Hi-Z Occlusion Culling

#include "GrassCullingViewExtension.h"
#include "GrassSceneProxy.h"
#include "SceneView.h"
#include "RenderGraphBuilder.h"
#include "RHICommandList.h"
#include "RHI.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"

// Debug CVar to show culling stats
static TAutoConsoleVariable<int32> CVarGrassCullingDebug(
    TEXT("r.Grass.CullingDebug"),
    0,
    TEXT("Show grass culling debug info: 0=Off, 1=On"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarGrassHiZDebug(
    TEXT("r.Grass.HiZDebug"),
    0,
    TEXT("Show Hi-Z debug info: 0=Off, 1=On"),
    ECVF_RenderThreadSafe
);

// ============================================================================
// Hi-Z Build Compute Shader (从 Scene Depth 生成 Hi-Z Mip 0)
// ============================================================================
class FGrassHiZBuildMip0CS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGrassHiZBuildMip0CS);
    SHADER_USE_PARAMETER_STRUCT(FGrassHiZBuildMip0CS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_TEXTURE(Texture2D, SrcDepthTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SrcDepthSampler)
        SHADER_PARAMETER_UAV(RWTexture2D<float>, DstHiZMip0)
        SHADER_PARAMETER(FIntPoint, SrcSize)
        SHADER_PARAMETER(FIntPoint, DstSize)
        SHADER_PARAMETER(FVector2f, InvSrcSize)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FGrassHiZBuildMip0CS, "/Plugin/UnrealGrass/Private/GrassHiZBuild.usf", "BuildHiZMip0CS", SF_Compute);

// ============================================================================
// Hi-Z Mip 降采样 Compute Shader
// ============================================================================
class FGrassHiZDownsampleCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGrassHiZDownsampleCS);
    SHADER_USE_PARAMETER_STRUCT(FGrassHiZDownsampleCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_TEXTURE(Texture2D, SrcMipTexture)
        SHADER_PARAMETER_UAV(RWTexture2D<float>, DstHiZMip0)
        SHADER_PARAMETER(FIntPoint, SrcMipSize)
        SHADER_PARAMETER(FIntPoint, DstMipSize)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FGrassHiZDownsampleCS, "/Plugin/UnrealGrass/Private/GrassHiZBuild.usf", "DownsampleMipCS", SF_Compute);

TSharedPtr<FGrassCullingViewExtension, ESPMode::ThreadSafe> FGrassCullingViewExtension::Instance = nullptr;

FGrassCullingViewExtension::FGrassCullingViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
    , HiZSize(0, 0)
    , LastViewProjectionMatrix(FMatrix::Identity)
    , bHiZValid(false)
    , LastFrameNumberHiZBuilt(0)
{
    UE_LOG(LogTemp, Log, TEXT("FGrassCullingViewExtension created with Hi-Z support"));
}

TSharedPtr<FGrassCullingViewExtension, ESPMode::ThreadSafe> FGrassCullingViewExtension::Get()
{
    if (!Instance.IsValid())
    {
        Instance = FSceneViewExtensions::NewExtension<FGrassCullingViewExtension>();
        UE_LOG(LogTemp, Log, TEXT("FGrassCullingViewExtension singleton initialized"));
    }
    return Instance;
}

void FGrassCullingViewExtension::RegisterGrassProxy(FGrassSceneProxy* Proxy)
{
    if (Proxy)
    {
        FScopeLock Lock(&ProxiesLock);
        RegisteredProxies.Add(Proxy);
        UE_LOG(LogTemp, Log, TEXT("Registered grass proxy for GPU Culling. Total: %d"), RegisteredProxies.Num());
    }
}

void FGrassCullingViewExtension::UnregisterGrassProxy(FGrassSceneProxy* Proxy)
{
    if (Proxy)
    {
        FScopeLock Lock(&ProxiesLock);
        RegisteredProxies.Remove(Proxy);
        UE_LOG(LogTemp, Log, TEXT("Unregistered grass proxy. Remaining: %d"), RegisteredProxies.Num());
    }
}

void FGrassCullingViewExtension::EnsureHiZTexture(FRHICommandListImmediate& RHICmdList, FIntPoint SceneDepthSize)
{
    // Hi-Z Mip 0 的尺寸是 Scene Depth 的一半
    FIntPoint DesiredSize = FIntPoint(
        FMath::Max(1, SceneDepthSize.X / 2),
        FMath::Max(1, SceneDepthSize.Y / 2)
    );
    
    // 需要创建新纹理或调整大小
    if (!HiZTexture.IsValid() || HiZSize != DesiredSize)
    {
        HiZSize = DesiredSize;
        
        // 计算需要的 Mip 级别数量 (最小尺寸为 1x1)
        int32 NumMips = FMath::CeilLogTwo(FMath::Max(HiZSize.X, HiZSize.Y)) + 1;
        NumMips = FMath::Clamp(NumMips, 1, 10);  // 限制最大 Mip 级别
        
        // 创建 Hi-Z 纹理
        FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("GrassHiZTexture"))
            .SetExtent(HiZSize.X, HiZSize.Y)
            .SetFormat(PF_R32_FLOAT)
            .SetNumMips(NumMips)
            .SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV);
        
        HiZTexture = RHICreateTexture(Desc);
        
        // 创建 SRV (UE 5.6 使用 FRHIViewDesc 新 API)
        HiZTextureSRV = RHICmdList.CreateShaderResourceView(
            HiZTexture.GetReference(),
            FRHIViewDesc::CreateTextureSRV()
                .SetDimensionFromTexture(HiZTexture.GetReference())
        );
        
        bHiZValid = false;  // 新创建的纹理还没有有效数据
        
        UE_LOG(LogTemp, Log, TEXT("Created Hi-Z texture: %dx%d, %d mips"), HiZSize.X, HiZSize.Y, NumMips);
    }
}

void FGrassCullingViewExtension::BuildHiZFromSceneDepth(
    FRHICommandListImmediate& RHICmdList,
    FRHITexture* SceneDepthTexture,
    FIntPoint DepthSize)
{
    if (!SceneDepthTexture || !HiZTexture.IsValid())
    {
        return;
    }
    
    // -------- Mip 0: 从 Scene Depth 降采样 --------
    {
        TShaderMapRef<FGrassHiZBuildMip0CS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassHiZBuildMip0CS::FParameters Params;
        
        Params.SrcDepthTexture = SceneDepthTexture;
        Params.SrcDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        
        // UE 5.6: 使用新的 FRHIViewDesc API 创建 UAV
        Params.DstHiZMip0 = RHICmdList.CreateUnorderedAccessView(
            HiZTexture.GetReference(),
            FRHIViewDesc::CreateTextureUAV()
                .SetDimensionFromTexture(HiZTexture.GetReference())
                .SetMipLevel(0)
        );
        
        Params.SrcSize = DepthSize;
        Params.DstSize = HiZSize;
        Params.InvSrcSize = FVector2f(1.0f / DepthSize.X, 1.0f / DepthSize.Y);
        
        FIntVector GroupCount = FIntVector(
            FMath::DivideAndRoundUp(HiZSize.X, 8),
            FMath::DivideAndRoundUp(HiZSize.Y, 8),
            1
        );
        
        FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, Params, GroupCount);
    }
    
    // 同步 UAV
    RHICmdList.Transition(FRHITransitionInfo(HiZTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    
    // -------- 生成更高级别的 Mip --------
    int32 NumMips = HiZTexture->GetNumMips();
    FIntPoint CurrentSize = HiZSize;
    
    for (int32 MipLevel = 1; MipLevel < NumMips && CurrentSize.X > 1 && CurrentSize.Y > 1; ++MipLevel)
    {
        FIntPoint SrcSize = CurrentSize;
        CurrentSize = FIntPoint(
            FMath::Max(1, CurrentSize.X / 2),
            FMath::Max(1, CurrentSize.Y / 2)
        );
        
        TShaderMapRef<FGrassHiZDownsampleCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassHiZDownsampleCS::FParameters Params;
        
        Params.SrcMipTexture = HiZTexture;
        
        // UE 5.6: 使用新的 FRHIViewDesc API 创建 UAV
        Params.DstHiZMip0 = RHICmdList.CreateUnorderedAccessView(
            HiZTexture.GetReference(),
            FRHIViewDesc::CreateTextureUAV()
                .SetDimensionFromTexture(HiZTexture.GetReference())
                .SetMipLevel((uint8)MipLevel)
        );
        
        Params.SrcMipSize = SrcSize;
        Params.DstMipSize = CurrentSize;
        
        // 转换到 UAV 状态
        RHICmdList.Transition(FRHITransitionInfo(HiZTexture, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        
        FIntVector GroupCount = FIntVector(
            FMath::DivideAndRoundUp(CurrentSize.X, 8),
            FMath::DivideAndRoundUp(CurrentSize.Y, 8),
            1
        );
        
        FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, Params, GroupCount);
        
        // 转换回 SRV 状态
        RHICmdList.Transition(FRHITransitionInfo(HiZTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    }
    
    bHiZValid = true;
    
    if (CVarGrassHiZDebug.GetValueOnRenderThread() > 0)
    {
        static uint32 LastLogFrame = 0;
        if (GFrameNumber - LastLogFrame > 60)
        {
            LastLogFrame = GFrameNumber;
            UE_LOG(LogTemp, Log, TEXT("Hi-Z built: %dx%d, %d mips"), HiZSize.X, HiZSize.Y, NumMips);
        }
    }
}

void FGrassCullingViewExtension::PostRenderBasePassDeferred_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneView& InView,
    const FRenderTargetBindingSlots& RenderTargets,
    TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
    check(IsInRenderingThread());
    
    // 只在有注册的草地代理时构建 Hi-Z
    if (RegisteredProxies.Num() == 0)
    {
        return;
    }
    
    // 检查是否需要遮挡剔除
    bool bNeedOcclusionCulling = false;
    {
        FScopeLock Lock(&ProxiesLock);
        for (FGrassSceneProxy* Proxy : RegisteredProxies)
        {
            if (Proxy && Proxy->bEnableOcclusionCulling)
            {
                bNeedOcclusionCulling = true;
                break;
            }
        }
    }
    
    if (!bNeedOcclusionCulling)
    {
        return;
    }
    
    // 避免同一帧多次构建
    if (LastFrameNumberHiZBuilt == GFrameNumber)
    {
        return;
    }
    LastFrameNumberHiZBuilt = GFrameNumber;
    
    // 保存当前帧的视图投影矩阵 (供下一帧使用)
    LastViewProjectionMatrix = InView.ViewMatrices.GetViewProjectionMatrix();
    
    // 获取场景深度尺寸 (UE 5.6 使用 UnscaledViewRect)
    FIntPoint DepthSize = InView.UnscaledViewRect.Size();
    if (DepthSize.X <= 0 || DepthSize.Y <= 0)
    {
        return;
    }
    
    // 确保 Hi-Z 纹理存在
    FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
    EnsureHiZTexture(RHICmdList, DepthSize);
    
    if (!HiZTexture.IsValid())
    {
        return;
    }
    
    // 获取深度 Render Target
    FRHITexture* DepthTexture = nullptr;
    if (RenderTargets.DepthStencil.GetTexture())
    {
        DepthTexture = RenderTargets.DepthStencil.GetTexture()->GetRHI();
    }
    
    if (DepthTexture)
    {
        // 构建 Hi-Z
        BuildHiZFromSceneDepth(RHICmdList, DepthTexture, DepthSize);
    }
    else if (CVarGrassHiZDebug.GetValueOnRenderThread() > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("Hi-Z: Depth texture not available from RenderTargets"));
    }
}

void FGrassCullingViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
    check(IsInRenderingThread());
    
    if (RegisteredProxies.Num() == 0)
    {
        return;
    }

    // Get the primary view for culling
    const FSceneView* PrimaryView = nullptr;
    for (int32 ViewIndex = 0; ViewIndex < InViewFamily.Views.Num(); ++ViewIndex)
    {
        if (InViewFamily.Views[ViewIndex])
        {
            PrimaryView = InViewFamily.Views[ViewIndex];
            break;
        }
    }

    if (!PrimaryView)
    {
        return;
    }

    // Execute GPU Culling for all registered proxies
    FScopeLock Lock(&ProxiesLock);
    
    FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
    
    int32 TotalProxiesCulled = 0;
    for (FGrassSceneProxy* Proxy : RegisteredProxies)
    {
        if (Proxy)
        {
            // 传递 Hi-Z 信息给 Proxy 进行遮挡剔除
            // 注意：使用上一帧的 Hi-Z 进行遮挡剔除（时序正确）
            Proxy->PerformGPUCullingWithHiZ(
                RHICmdList,
                PrimaryView,
                bHiZValid ? HiZTexture.GetReference() : nullptr,
                HiZSize,
                LastViewProjectionMatrix
            );
            TotalProxiesCulled++;
        }
    }
    
    // Debug output
    if (CVarGrassCullingDebug.GetValueOnRenderThread() > 0)
    {
        static uint32 LastLogFrame = 0;
        if (GFrameNumber - LastLogFrame > 60) // Log every ~1 second at 60fps
        {
            LastLogFrame = GFrameNumber;
            UE_LOG(LogTemp, Log, TEXT("GPU Culling executed for %d grass proxies (Hi-Z %s)"), 
                TotalProxiesCulled, bHiZValid ? TEXT("enabled") : TEXT("disabled"));
        }
    }
}
