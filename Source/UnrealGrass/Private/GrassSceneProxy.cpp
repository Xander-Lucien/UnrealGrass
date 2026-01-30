// GrassSceneProxy.cpp
// GPU Instancing 渲染草

#include "GrassSceneProxy.h"
#include "GrassComponent.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "SceneManagement.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

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

    // 设置实例位置缓冲区 SRV 到 Vertex Factory
    VertexFactory.SetInstancePositionSRV(PositionBufferSRV.GetReference(), InstanceCount);

    // 根据是否有自定义 Mesh 选择初始化方式
    if (Component->GrassMesh && Component->GrassMesh->GetRenderData() && 
        Component->GrassMesh->GetRenderData()->LODResources.Num() > 0)
    {
        InitFromStaticMesh(Component->GrassMesh);
    }
    else
    {
        InitDefaultGrassBlade();
    }

    // 需要捕获原始指针用于 render command
    FStaticMeshVertexBuffers* VertexBuffersPtr = &VertexBuffers;
    FRawStaticIndexBuffer* IndexBufferPtr = &IndexBuffer;
    FGrassVertexFactory* VertexFactoryPtr = &VertexFactory;
    
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
    
    UE_LOG(LogTemp, Log, TEXT("FGrassSceneProxy created with %d instances, %d vertices, %d triangles"), 
        InstanceCount, NumVertices, NumPrimitives);
}

void FGrassSceneProxy::InitFromStaticMesh(UStaticMesh* StaticMesh)
{
    const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
    
    // 获取顶点数量
    NumVertices = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
    
    // 获取索引数量和三角形数量
    NumIndices = LOD.IndexBuffer.GetNumIndices();
    NumPrimitives = NumIndices / 3;
    
    // 复制位置数据
    TArray<FVector3f> Positions;
    Positions.SetNum(NumVertices);
    for (int32 i = 0; i < NumVertices; i++)
    {
        Positions[i] = LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(i);
    }
    VertexBuffers.PositionVertexBuffer.Init(Positions);
    
    // 复制切线和 UV 数据
    const int32 NumTexCoords = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
    VertexBuffers.StaticMeshVertexBuffer.Init(NumVertices, NumTexCoords);
    
    for (int32 i = 0; i < NumVertices; i++)
    {
        // 切线数据
        FVector3f TangentX = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
        FVector3f TangentZ = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i);
        FVector3f TangentY = FVector3f::CrossProduct(TangentZ, TangentX);
        VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(i, TangentX, TangentY, TangentZ);
        
        // UV 数据
        for (int32 UVIndex = 0; UVIndex < NumTexCoords; UVIndex++)
        {
            FVector2f UV = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, UVIndex);
            VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(i, UVIndex, UV);
        }
    }
    
    // 复制顶点颜色
    VertexBuffers.ColorVertexBuffer.Init(NumVertices);
    if (LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0)
    {
        for (int32 i = 0; i < NumVertices; i++)
        {
            VertexBuffers.ColorVertexBuffer.VertexColor(i) = LOD.VertexBuffers.ColorVertexBuffer.VertexColor(i);
        }
    }
    else
    {
        for (int32 i = 0; i < NumVertices; i++)
        {
            VertexBuffers.ColorVertexBuffer.VertexColor(i) = FColor::White;
        }
    }
    
    // 复制索引数据
    TArray<uint32> Indices;
    Indices.SetNum(NumIndices);
    if (LOD.IndexBuffer.Is32Bit())
    {
        for (int32 i = 0; i < NumIndices; i++)
        {
            Indices[i] = LOD.IndexBuffer.GetIndex(i);
        }
    }
    else
    {
        for (int32 i = 0; i < NumIndices; i++)
        {
            Indices[i] = LOD.IndexBuffer.GetIndex(i);
        }
    }
    IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);
    
    UE_LOG(LogTemp, Log, TEXT("Initialized grass from StaticMesh: %s (%d vertices, %d triangles, %d UVs)"), 
        *StaticMesh->GetName(), NumVertices, NumPrimitives, NumTexCoords);
}

void FGrassSceneProxy::InitDefaultGrassBlade()
{
    // 创建一个简单的三角形草叶 (双面)
    TArray<FVector3f> Positions = {
        FVector3f(-5, 0, 0),
        FVector3f(5, 0, 0),
        FVector3f(0, 0, 50)
    };

    TArray<uint32> Indices = { 0, 1, 2 };
    NumVertices = 3;
    NumIndices = 3;
    NumPrimitives = 1;

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
    
    UE_LOG(LogTemp, Log, TEXT("Initialized default grass blade (triangle)"));
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
    if (InstanceCount == 0)
        return;

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
        Mesh.bDisableBackfaceCulling = true;  // 双面渲染草叶

        FMeshBatchElement& Element = Mesh.Elements[0];
        Element.IndexBuffer = &IndexBuffer;
        Element.FirstIndex = 0;
        Element.NumPrimitives = NumPrimitives;  // 使用实际的三角形数量
        Element.MinVertexIndex = 0;
        Element.MaxVertexIndex = NumVertices - 1;
        Element.NumInstances = InstanceCount;  // GPU Instancing: 渲染所有实例
        Element.PrimitiveUniformBuffer = GetUniformBuffer();

        Collector.AddMesh(ViewIndex, Mesh);
    }
}
