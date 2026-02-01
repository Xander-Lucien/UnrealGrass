// GrassSceneProxy.h
// GPU Instancing 草地场景代理 + GPU Frustum Culling

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GrassVertexFactory.h"
#include "StaticMeshResources.h"

class UGrassComponent;
class FGrassCullingViewExtension;

class FGrassSceneProxy : public FPrimitiveSceneProxy
{
    friend class FGrassCullingViewExtension;
    
public:
    FGrassSceneProxy(UGrassComponent* Component);
    virtual ~FGrassSceneProxy();

    virtual SIZE_T GetTypeHash() const override;
    virtual uint32 GetMemoryFootprint() const override;
    virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
    virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

    /** 执行 GPU Frustum Culling (必须在渲染线程调用) */
    void PerformGPUCulling(FRHICommandListImmediate& RHICmdList, const FSceneView* View) const;

    /** 在渲染线程上执行 GPU Frustum Culling (使用预提取的数据) */
    void PerformGPUCullingRenderThread(FRHICommandListImmediate& RHICmdList, const FMatrix& ViewProjectionMatrix, const FVector& ViewOrigin, const FMatrix& LocalToWorldMatrix) const;

    /** 是否启用了 GPU Culling */
    bool IsGPUCullingEnabled() const { return bEnableFrustumCulling && bUseIndirectDraw; }

private:
    /** 从 UStaticMesh 初始化顶点和索引缓冲区 */
    void InitFromStaticMesh(UStaticMesh* StaticMesh);
    
    /** 使用默认三角形草叶初始化 */
    void InitDefaultGrassBlade();

    // 草叶 Mesh
    FStaticMeshVertexBuffers VertexBuffers;
    FGrassVertexFactory VertexFactory;  // 使用自定义 Vertex Factory
    FRawStaticIndexBuffer IndexBuffer;
    int32 NumVertices = 0;
    int32 NumIndices = 0;
    int32 NumPrimitives = 0;  // 三角形数量

    // ======== 实例数据 ========
    // 所有实例位置 Buffer (用于 Culling 输入)
    FBufferRHIRef PositionBuffer;
    FShaderResourceViewRHIRef PositionBufferSRV;
    int32 TotalInstanceCount = 0;  // 总实例数量

    // 可见实例位置 Buffer (Culling 输出，用于渲染)
    FBufferRHIRef VisiblePositionBuffer;
    FShaderResourceViewRHIRef VisiblePositionBufferSRV;
    FUnorderedAccessViewRHIRef VisiblePositionBufferUAV;

    // ======== Indirect Draw 支持 ========
    bool bUseIndirectDraw = false;
    FBufferRHIRef IndirectArgsBuffer;
    FUnorderedAccessViewRHIRef IndirectArgsBufferUAV;

    // ======== GPU Culling 参数 ========
    bool bEnableFrustumCulling = false;
    bool bEnableDistanceCulling = false;
    float MaxVisibleDistance = 10000.0f;
    float GrassBoundingRadius = 50.0f;

    // 标记当前帧是否已执行剔除
    mutable bool bCullingPerformedThisFrame = false;
    mutable uint32 LastFrameNumber = 0;

    // 材质
    UMaterialInterface* Material = nullptr;
};

