// GrassSceneProxy.h
// 简单的场景代理：用 GPU Instancing 绘制草

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "LocalVertexFactory.h"
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
    // 草叶 Mesh
    FStaticMeshVertexBuffers VertexBuffers;
    FLocalVertexFactory VertexFactory;
    FRawStaticIndexBuffer IndexBuffer;
    int32 NumVertices = 0;
    int32 NumIndices = 0;

    // 实例数据 (从 GPU buffer 来)
    FShaderResourceViewRHIRef PositionBufferSRV;
    int32 InstanceCount = 0;

    // 材质
    UMaterialInterface* Material = nullptr;
};
