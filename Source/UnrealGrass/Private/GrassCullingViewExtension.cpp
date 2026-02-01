// GrassCullingViewExtension.cpp
// Scene View Extension implementation for GPU Culling

#include "GrassCullingViewExtension.h"
#include "GrassSceneProxy.h"
#include "SceneView.h"
#include "RenderGraphBuilder.h"
#include "RHICommandList.h"

// Debug CVar to show culling stats
static TAutoConsoleVariable<int32> CVarGrassCullingDebug(
    TEXT("r.Grass.CullingDebug"),
    0,
    TEXT("Show grass culling debug info: 0=Off, 1=On"),
    ECVF_RenderThreadSafe
);

TSharedPtr<FGrassCullingViewExtension, ESPMode::ThreadSafe> FGrassCullingViewExtension::Instance = nullptr;

FGrassCullingViewExtension::FGrassCullingViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
    UE_LOG(LogTemp, Log, TEXT("FGrassCullingViewExtension created"));
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
            Proxy->PerformGPUCulling(RHICmdList, PrimaryView);
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
            UE_LOG(LogTemp, Log, TEXT("GPU Culling executed for %d grass proxies"), TotalProxiesCulled);
        }
    }
}
