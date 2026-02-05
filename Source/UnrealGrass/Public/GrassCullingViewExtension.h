// GrassCullingViewExtension.h
// Scene View Extension for executing GPU Culling at the correct render stage

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "RenderGraphResources.h"

class FGrassSceneProxy;

/**
 * View Extension that executes GPU Culling for all registered grass proxies
 * before the main render pass begins.
 * 
 * 同时负责生成 Hi-Z (Hierarchical Z-Buffer) 用于遮挡剔除
 */
class FGrassCullingViewExtension : public FSceneViewExtensionBase
{
public:
    FGrassCullingViewExtension(const FAutoRegister& AutoRegister);
    virtual ~FGrassCullingViewExtension() = default;

    // FSceneViewExtensionBase interface
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
    
    /** Called on render thread before rendering begins - this is where we execute GPU Culling */
    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
    
    virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override {}
    
    /** Called after base pass - we can access scene depth here to build Hi-Z */
    virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;
    
    virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override {}
    virtual void PostRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override {}

    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override { return true; }

    /** Register a grass proxy for culling */
    void RegisterGrassProxy(FGrassSceneProxy* Proxy);
    
    /** Unregister a grass proxy */
    void UnregisterGrassProxy(FGrassSceneProxy* Proxy);

    /** Get the singleton instance */
    static TSharedPtr<FGrassCullingViewExtension, ESPMode::ThreadSafe> Get();

    /** 获取 Hi-Z 纹理 (用于遮挡剔除) */
    FTextureRHIRef GetHiZTexture() const { return HiZTexture; }
    
    /** 获取 Hi-Z 纹理尺寸 */
    FIntPoint GetHiZSize() const { return HiZSize; }

private:
    /** All registered grass proxies that need culling */
    TSet<FGrassSceneProxy*> RegisteredProxies;
    
    /** Critical section for thread-safe access */
    FCriticalSection ProxiesLock;

    /** Singleton instance */
    static TSharedPtr<FGrassCullingViewExtension, ESPMode::ThreadSafe> Instance;

    // ======== Hi-Z 资源 ========
    /** Hi-Z 纹理 (包含多级 Mip) */
    FTextureRHIRef HiZTexture;
    FShaderResourceViewRHIRef HiZTextureSRV;
    
    /** Hi-Z 尺寸 (Mip 0 的尺寸) */
    FIntPoint HiZSize;
    
    /** 上一帧的视图投影矩阵 (用于遮挡剔除) */
    FMatrix LastViewProjectionMatrix;
    
    /** 上一帧是否有效 (第一帧没有 Hi-Z) */
    bool bHiZValid = false;
    
    /** 上一帧的帧号 */
    uint32 LastFrameNumberHiZBuilt = 0;
    
    /** 创建或调整 Hi-Z 纹理大小 */
    void EnsureHiZTexture(FRHICommandListImmediate& RHICmdList, FIntPoint SceneDepthSize);
    
    /** 从场景深度构建 Hi-Z */
    void BuildHiZFromSceneDepth(FRHICommandListImmediate& RHICmdList, FRHITexture* SceneDepthTexture, FIntPoint DepthSize);
};
