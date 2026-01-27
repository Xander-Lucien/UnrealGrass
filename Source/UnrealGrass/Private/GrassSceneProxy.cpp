// GrassSceneProxy.cpp
// GPU Instancing 绘制草

#include "GrassSceneProxy.h"
#include "GrassComponent.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "SceneManagement.h"

FGrassSceneProxy::FGrassSceneProxy(UGrassComponent* Component)
    : FPrimitiveSceneProxy(Component)
    , VertexFactory(GetScene().GetFeatureLevel(), "GrassVertexFactory")
    , PositionBufferSRV(Component->PositionBufferSRV)
    , InstanceCount(Component->InstanceCount)
    , Material(Component->GrassMaterial)
{
    bVerifyUsedMaterials = false;
    
    if (!Material)
    {
        Material = UMaterial::GetDefaultMaterial(MD_Surface);
    }

    // 创建一个正常大小的三角形草叶
    TArray<FVector3f> Positions = {
        FVector3f(-5, 0, 0),
        FVector3f(5, 0, 0),
        FVector3f(0, 0, 50)
    };

    TArray<uint32> Indices = { 0, 1, 2 };
    NumVertices = 3;
    NumIndices = 3;

    VertexBuffers.PositionVertexBuffer.Init(Positions);
    VertexBuffers.StaticMeshVertexBuffer.Init(NumVertices, 1);

    for (int32 i = 0; i < NumVertices; i++)
    {
        VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(i, 
            FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1));
        VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(i, 0, 
            FVector2f(Positions[i].X / 10.0f + 0.5f, 1.0f - Positions[i].Z / 50.0f));
    }

    VertexBuffers.ColorVertexBuffer.Init(NumVertices);
    for (int32 i = 0; i < NumVertices; i++)
    {
        VertexBuffers.ColorVertexBuffer.VertexColor(i) = FColor::White;
    }

    IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);

    FStaticMeshVertexBuffers* VertexBuffersPtr = &VertexBuffers;
    FRawStaticIndexBuffer* IndexBufferPtr = &IndexBuffer;
    FLocalVertexFactory* VertexFactoryPtr = &VertexFactory;
    
    ENQUEUE_RENDER_COMMAND(InitGrassResources)(
        [VertexBuffersPtr, IndexBufferPtr, VertexFactoryPtr](FRHICommandListImmediate& RHICmdList)
        {
            VertexBuffersPtr->PositionVertexBuffer.InitResource(RHICmdList);
            VertexBuffersPtr->StaticMeshVertexBuffer.InitResource(RHICmdList);
            VertexBuffersPtr->ColorVertexBuffer.InitResource(RHICmdList);
            IndexBufferPtr->InitResource(RHICmdList);

            FLocalVertexFactory::FDataType Data;
            VertexBuffersPtr->PositionVertexBuffer.BindPositionVertexBuffer(VertexFactoryPtr, Data);
            VertexBuffersPtr->StaticMeshVertexBuffer.BindTangentVertexBuffer(VertexFactoryPtr, Data);
            VertexBuffersPtr->StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(VertexFactoryPtr, Data);
            VertexBuffersPtr->StaticMeshVertexBuffer.BindLightMapVertexBuffer(VertexFactoryPtr, Data, 0);
            VertexBuffersPtr->ColorVertexBuffer.BindColorVertexBuffer(VertexFactoryPtr, Data);
            
            VertexFactoryPtr->SetData(RHICmdList, Data);
            VertexFactoryPtr->InitResource(RHICmdList);
        }
    );
    
    FlushRenderingCommands();
    
    UE_LOG(LogTemp, Log, TEXT("FGrassSceneProxy created with %d instances"), InstanceCount);
}

FGrassSceneProxy::~FGrassSceneProxy()
{
    VertexBuffers.PositionVertexBuffer.ReleaseResource();
    VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
    VertexBuffers.ColorVertexBuffer.ReleaseResource();
    IndexBuffer.ReleaseResource();
    VertexFactory.ReleaseResource();
}

SIZE_T FGrassSceneProxy::GetTypeHash() const
{
    static size_t UniquePointer;
    return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FGrassSceneProxy::GetMemoryFootprint() const
{
    return sizeof(*this);
}

FPrimitiveViewRelevance FGrassSceneProxy::GetViewRelevance(const FSceneView* View) const
{
    FPrimitiveViewRelevance Result;
    Result.bDrawRelevance = IsShown(View);
    Result.bDynamicRelevance = true;
    Result.bRenderInMainPass = true;
    return Result;
}

void FGrassSceneProxy::GetDynamicMeshElements(
    const TArray<const FSceneView*>& Views,
    const FSceneViewFamily& ViewFamily,
    uint32 VisibilityMap,
    FMeshElementCollector& Collector) const
{
    for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
    {
        if (!(VisibilityMap & (1 << ViewIndex)))
            continue;

        // 使用 GPU Instancing 渲染所有草叶实例
        FMeshBatch& Mesh = Collector.AllocateMesh();
        Mesh.VertexFactory = &VertexFactory;
        Mesh.MaterialRenderProxy = Material->GetRenderProxy();
        Mesh.Type = PT_TriangleList;
        Mesh.DepthPriorityGroup = SDPG_World;
        Mesh.bCanApplyViewModeOverrides = true;
        Mesh.ReverseCulling = false;
        Mesh.CastShadow = false;
        Mesh.bDisableBackfaceCulling = true;

        FMeshBatchElement& Element = Mesh.Elements[0];
        Element.IndexBuffer = &IndexBuffer;
        Element.FirstIndex = 0;
        Element.NumPrimitives = 1;
        Element.MinVertexIndex = 0;
        Element.MaxVertexIndex = NumVertices - 1;
        Element.NumInstances = InstanceCount;  // 渲染所有实例！
        Element.PrimitiveUniformBuffer = GetUniformBuffer();

        Collector.AddMesh(ViewIndex, Mesh);
    }
}
