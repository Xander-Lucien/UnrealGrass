// GrassCullingViewExtension.h
// Scene View Extension for executing GPU Culling at the correct render stage

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FGrassSceneProxy;

/**
 * View Extension that executes GPU Culling for all registered grass proxies
 * before the main render pass begins.
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
    virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override {}
    virtual void PostRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override {}

    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override { return true; }

    /** Register a grass proxy for culling */
    void RegisterGrassProxy(FGrassSceneProxy* Proxy);
    
    /** Unregister a grass proxy */
    void UnregisterGrassProxy(FGrassSceneProxy* Proxy);

    /** Get the singleton instance */
    static TSharedPtr<FGrassCullingViewExtension, ESPMode::ThreadSafe> Get();

private:
    /** All registered grass proxies that need culling */
    TSet<FGrassSceneProxy*> RegisteredProxies;
    
    /** Critical section for thread-safe access */
    FCriticalSection ProxiesLock;

    /** Singleton instance */
    static TSharedPtr<FGrassCullingViewExtension, ESPMode::ThreadSafe> Instance;
};
