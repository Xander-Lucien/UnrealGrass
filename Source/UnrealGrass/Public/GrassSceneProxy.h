// GrassSceneProxy.h
// 简单的草地场景代理 GPU Instancing 渲染草

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GrassVertexFactory.h"
#include "StaticMeshResources.h"

class UGrassComponent;

class FGrassSceneProxy : public FPrimitiveSceneProxy
{
public:
    FGrassSceneProxy(UGrassComponent* Component);
    virtual ~FGrassSceneProxy();

    virtual SIZE_T GetTypeHash() const override;
    virtual uint32 GetMemoryFootprint() const override;
    virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
    virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

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

    // 实例数据 (保持 SRV 引用)
    FShaderResourceViewRHIRef PositionBufferSRV;
    int32 InstanceCount = 0;

    // 材质
    UMaterialInterface* Material = nullptr;
};
