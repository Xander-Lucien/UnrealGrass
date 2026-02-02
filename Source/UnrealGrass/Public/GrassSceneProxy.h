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
    
    /** 使用默认三角形草叶初始化 (LOD 0 - 15 顶点高质量) */
    void InitDefaultGrassBlade();

    /** 初始化 LOD 1 草叶网格 (7 顶点简化版) */
    void InitLOD1GrassBlade();

    // ======== LOD 0 草叶 Mesh (15 顶点) ========
    FStaticMeshVertexBuffers VertexBuffers;
    FGrassVertexFactory VertexFactory;  // 使用自定义 Vertex Factory
    FRawStaticIndexBuffer IndexBuffer;
    int32 NumVertices = 0;
    int32 NumIndices = 0;
    int32 NumPrimitives = 0;  // 三角形数量

    // ======== LOD 1 草叶 Mesh (7 顶点) ========
    FStaticMeshVertexBuffers VertexBuffersLOD1;
    FGrassVertexFactory VertexFactoryLOD1;  // LOD 1 专用 Vertex Factory
    FRawStaticIndexBuffer IndexBufferLOD1;
    int32 NumVerticesLOD1 = 0;
    int32 NumIndicesLOD1 = 0;
    int32 NumPrimitivesLOD1 = 0;

    // ======== 实例数据 ========
    // 所有实例位置 Buffer (用于 Culling 输入)
    FBufferRHIRef PositionBuffer;
    FShaderResourceViewRHIRef PositionBufferSRV;
    int32 TotalInstanceCount = 0;  // 总实例数量

    // 草叶属性数据 Buffers (用于 Bezier 变形)
    FShaderResourceViewRHIRef GrassData0SRV;  // Height, Width, Tilt, Bend
    FShaderResourceViewRHIRef GrassData1SRV;  // TaperAmount, FacingDir.x, FacingDir.y, P1Offset
    FShaderResourceViewRHIRef GrassData2SRV;  // P2Offset

    // 可见实例位置 Buffer (Culling 输出，用于渲染)
    FBufferRHIRef VisiblePositionBuffer;
    FShaderResourceViewRHIRef VisiblePositionBufferSRV;
    FUnorderedAccessViewRHIRef VisiblePositionBufferUAV;

    // 可见实例属性 Buffers (Culling 输出，用于渲染)
    FBufferRHIRef VisibleGrassData0Buffer;  // Height, Width, Tilt, Bend
    FShaderResourceViewRHIRef VisibleGrassData0SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData0UAV;
    
    FBufferRHIRef VisibleGrassData1Buffer;  // TaperAmount, FacingDir.x, FacingDir.y, P1Offset
    FShaderResourceViewRHIRef VisibleGrassData1SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData1UAV;
    
    FBufferRHIRef VisibleGrassData2Buffer;  // P2Offset
    FShaderResourceViewRHIRef VisibleGrassData2SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData2UAV;

    // ======== Indirect Draw 支持 ========
    bool bUseIndirectDraw = false;
    FBufferRHIRef IndirectArgsBuffer;
    FUnorderedAccessViewRHIRef IndirectArgsBufferUAV;
    
    // LOD 1 的 Indirect Draw Args
    FBufferRHIRef IndirectArgsBufferLOD1;
    FUnorderedAccessViewRHIRef IndirectArgsBufferLOD1UAV;

    // ======== LOD 1 独立的 Visible Buffers ========
    FBufferRHIRef VisiblePositionBufferLOD1;
    FShaderResourceViewRHIRef VisiblePositionBufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisiblePositionBufferLOD1UAV;
    
    FBufferRHIRef VisibleGrassData0BufferLOD1;
    FShaderResourceViewRHIRef VisibleGrassData0BufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData0BufferLOD1UAV;
    
    FBufferRHIRef VisibleGrassData1BufferLOD1;
    FShaderResourceViewRHIRef VisibleGrassData1BufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData1BufferLOD1UAV;
    
    FBufferRHIRef VisibleGrassData2BufferLOD1;
    FShaderResourceViewRHIRef VisibleGrassData2BufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData2BufferLOD1UAV;

    // ======== GPU Culling 参数 ========
    bool bEnableFrustumCulling = false;
    bool bEnableDistanceCulling = false;
    float MaxVisibleDistance = 10000.0f;
    float GrassBoundingRadius = 50.0f;

    // ======== LOD 参数 ========
    bool bEnableLOD = true;
    float LOD0Distance = 1000.0f;

    // ======== 草叶外观参数 ========
    float CurvedNormalAmount = 0.5f;  // 弯曲法线程度

    // 标记当前帧是否已执行剔除
    mutable bool bCullingPerformedThisFrame = false;
    mutable uint32 LastFrameNumber = 0;

    // 材质
    UMaterialInterface* Material = nullptr;
};

