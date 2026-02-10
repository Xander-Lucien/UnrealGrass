// GrassSceneProxy.cpp
// GPU Instancing 渲染草 + GPU Frustum Culling

#include "GrassSceneProxy.h"
#include "GrassComponent.h"
#include "GrassCullingViewExtension.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "SceneManagement.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RenderTargetPool.h"  // For GBlackTexture
#include "Engine/Texture2D.h"

// ============================================================================
// GPU Frustum Culling Compute Shader (支持 LOD)
// ============================================================================
class FGrassFrustumCullingCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGrassFrustumCullingCS);
    SHADER_USE_PARAMETER_STRUCT(FGrassFrustumCullingCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(StructuredBuffer<FVector3f>, InPositions)
        SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, InGrassData0)
        SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, InGrassData1)
        SHADER_PARAMETER_SRV(StructuredBuffer<float>, InGrassData2)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector3f>, OutVisiblePositions)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutVisibleGrassData0)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutVisibleGrassData1)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, OutVisibleGrassData2)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector3f>, OutVisiblePositionsLOD1)  // LOD 1 独立输出
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutVisibleGrassData0LOD1)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutVisibleGrassData1LOD1)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, OutVisibleGrassData2LOD1)
        SHADER_PARAMETER_UAV(RWBuffer<uint>, OutIndirectArgs)
        SHADER_PARAMETER_UAV(RWBuffer<uint>, OutIndirectArgsLOD1)  // LOD 1 的 Indirect Args
        SHADER_PARAMETER(uint32, TotalInstanceCount)
        SHADER_PARAMETER(uint32, IndexCountPerInstance)
        SHADER_PARAMETER(uint32, IndexCountPerInstanceLOD1)  // LOD 1 的索引数量
        SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
        SHADER_PARAMETER(FMatrix44f, LocalToWorld)
        SHADER_PARAMETER(float, BoundingRadius)
        SHADER_PARAMETER(float, MaxVisibleDistance)
        SHADER_PARAMETER(float, LOD0Distance)  // LOD 切换距离
        SHADER_PARAMETER(FVector3f, CameraPosition)
        // Hi-Z 遮挡剔除参数
        SHADER_PARAMETER_TEXTURE(Texture2D, HiZTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, HiZSampler)
        SHADER_PARAMETER(uint32, bEnableOcclusionCulling)
        SHADER_PARAMETER(FVector2f, HiZSize)
        SHADER_PARAMETER(FMatrix44f, ViewProjectionMatrix)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FGrassFrustumCullingCS, "/Plugin/UnrealGrass/Private/GrassFrustumCulling.usf", "MainCS", SF_Compute);

// 重置 Indirect Args 的 Compute Shader (支持 LOD)
class FGrassResetIndirectArgsCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGrassResetIndirectArgsCS);
    SHADER_USE_PARAMETER_STRUCT(FGrassResetIndirectArgsCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_UAV(RWBuffer<uint>, OutIndirectArgs)
        SHADER_PARAMETER_UAV(RWBuffer<uint>, OutIndirectArgsLOD1)  // LOD 1 的 Indirect Args
        SHADER_PARAMETER(uint32, IndexCountPerInstance)
        SHADER_PARAMETER(uint32, IndexCountPerInstanceLOD1)  // LOD 1 的索引数量
        SHADER_PARAMETER(uint32, TotalInstanceCount)  // 用于计算 LOD 1 的 StartInstanceLocation
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FGrassResetIndirectArgsCS, "/Plugin/UnrealGrass/Private/GrassFrustumCulling.usf", "ResetIndirectArgsCS", SF_Compute);

// ============================================================================
// FGrassSceneProxy 实现
// ============================================================================

FGrassSceneProxy::FGrassSceneProxy(UGrassComponent* Component)
: FPrimitiveSceneProxy(Component)
, VertexFactory(GetScene().GetFeatureLevel(), "GrassVertexFactory")
, VertexFactoryLOD1(GetScene().GetFeatureLevel(), "GrassVertexFactoryLOD1")  // LOD 1 Vertex Factory
, PositionBuffer(Component->PositionBuffer)
, PositionBufferSRV(Component->PositionBufferSRV)
, TotalInstanceCount(Component->InstanceCount)
, GrassData0SRV(Component->GrassDataBufferSRV)
, GrassData1SRV(Component->GrassData1BufferSRV)
, GrassData2SRV(Component->GrassData2BufferSRV)
, VisiblePositionBuffer(Component->VisiblePositionBuffer)
, VisiblePositionBufferSRV(Component->VisiblePositionBufferSRV)
, VisiblePositionBufferUAV(Component->VisiblePositionBufferUAV)
, VisibleGrassData0Buffer(Component->VisibleGrassData0Buffer)
, VisibleGrassData0SRV(Component->VisibleGrassData0BufferSRV)
, VisibleGrassData0UAV(Component->VisibleGrassData0BufferUAV)
, VisibleGrassData1Buffer(Component->VisibleGrassData1Buffer)
, VisibleGrassData1SRV(Component->VisibleGrassData1BufferSRV)
, VisibleGrassData1UAV(Component->VisibleGrassData1BufferUAV)
, VisibleGrassData2Buffer(Component->VisibleGrassData2Buffer)
, VisibleGrassData2SRV(Component->VisibleGrassData2BufferSRV)
, VisibleGrassData2UAV(Component->VisibleGrassData2BufferUAV)
, bUseIndirectDraw(Component->bUseIndirectDraw)
, IndirectArgsBuffer(Component->IndirectArgsBuffer)
, IndirectArgsBufferUAV(Component->IndirectArgsBufferUAV)
, IndirectArgsBufferLOD1(Component->IndirectArgsBufferLOD1)  // LOD 1 Indirect Args
, IndirectArgsBufferLOD1UAV(Component->IndirectArgsBufferLOD1UAV)
, VisiblePositionBufferLOD1(Component->VisiblePositionBufferLOD1)  // LOD 1 独立 Visible Buffers
, VisiblePositionBufferLOD1SRV(Component->VisiblePositionBufferLOD1SRV)
, VisiblePositionBufferLOD1UAV(Component->VisiblePositionBufferLOD1UAV)
, VisibleGrassData0BufferLOD1(Component->VisibleGrassData0BufferLOD1)
, VisibleGrassData0BufferLOD1SRV(Component->VisibleGrassData0BufferLOD1SRV)
, VisibleGrassData0BufferLOD1UAV(Component->VisibleGrassData0BufferLOD1UAV)
, VisibleGrassData1BufferLOD1(Component->VisibleGrassData1BufferLOD1)
, VisibleGrassData1BufferLOD1SRV(Component->VisibleGrassData1BufferLOD1SRV)
, VisibleGrassData1BufferLOD1UAV(Component->VisibleGrassData1BufferLOD1UAV)
, VisibleGrassData2BufferLOD1(Component->VisibleGrassData2BufferLOD1)
, VisibleGrassData2BufferLOD1SRV(Component->VisibleGrassData2BufferLOD1SRV)
, VisibleGrassData2BufferLOD1UAV(Component->VisibleGrassData2BufferLOD1UAV)
, bEnableFrustumCulling(Component->bEnableFrustumCulling)
, bEnableDistanceCulling(Component->bEnableDistanceCulling)
, bEnableOcclusionCulling(Component->bEnableOcclusionCulling)
, MaxVisibleDistance(Component->MaxVisibleDistance)
, GrassBoundingRadius(Component->GrassBoundingRadius)
, bEnableLOD(Component->bEnableLOD)  // LOD 参数
, LOD0Distance(Component->LOD0Distance)
, CurvedNormalAmount(Component->RenderParameters.CurvedNormalAmount)  // 弯曲法线参数 (从 RenderParameters 获取)
, ViewRotationAmount(Component->RenderParameters.ViewRotationAmount)  // 视角依赖旋转参数 (对马岛之魂风格)
, Material(Component->GrassMaterial)
{
    bVerifyUsedMaterials = false;

    if (!Material)
    {
        Material = UMaterial::GetDefaultMaterial(MD_Surface);
    }

    FTextureRHIRef WindNoiseTextureRHI;
    if (Component->WindNoiseTexture && Component->WindNoiseTexture->GetResource())
    {
        WindNoiseTextureRHI = Component->WindNoiseTexture->GetResource()->TextureRHI;
    }

    const FVector2f WindNoiseScale = FVector2f(Component->WindNoiseScale.X, Component->WindNoiseScale.Y);
    const float WindNoiseStrength = Component->WindNoiseStrength;
    const float WindNoiseSpeed = Component->WindNoiseSpeed;
    
    // 正弦波风参数
    const float WindWaveSpeed = Component->WindWaveSpeed;
    const float WindWaveAmplitude = Component->WindWaveAmplitude;
    const float WindSinOffsetRange = Component->WindSinOffsetRange;
    const float WindPushTipForward = Component->WindPushTipForward;
    const float LocalWindRotateAmount = Component->LocalWindRotateAmount;

    // IMPORTANT: GPU Culling is not yet fully implemented
    // Always use PositionBufferSRV (all instances) for now
    // When GPU Culling is properly implemented, switch to VisiblePositionBufferSRV
    if (bEnableFrustumCulling && bUseIndirectDraw && VisiblePositionBufferSRV.IsValid())
    {
        // GPU Culling enabled: use Visible buffers (will be filled by culling shader)
        VertexFactory.SetInstancePositionSRV(VisiblePositionBufferSRV.GetReference(), TotalInstanceCount);
        
        // 使用可见属性 Buffer（与位置同步的属性数据）
        VertexFactory.SetGrassDataSRV(
            VisibleGrassData0SRV.IsValid() ? VisibleGrassData0SRV.GetReference() : nullptr,
            VisibleGrassData1SRV.IsValid() ? VisibleGrassData1SRV.GetReference() : nullptr,
            VisibleGrassData2SRV.IsValid() ? VisibleGrassData2SRV.GetReference() : nullptr
        );
        UE_LOG(LogTemp, Log, TEXT("Using Visible Buffers for rendering (GPU Culling enabled, %d max instances)"), TotalInstanceCount);
    }
    else
    {
        // No GPU Culling: use all positions and original grass data
        VertexFactory.SetInstancePositionSRV(PositionBufferSRV.GetReference(), TotalInstanceCount);
        
        // Set grass blade data SRVs for Bezier deformation
        VertexFactory.SetGrassDataSRV(
            GrassData0SRV.IsValid() ? GrassData0SRV.GetReference() : nullptr,
            GrassData1SRV.IsValid() ? GrassData1SRV.GetReference() : nullptr,
            GrassData2SRV.IsValid() ? GrassData2SRV.GetReference() : nullptr
        );
        UE_LOG(LogTemp, Log, TEXT("Using original Buffers for rendering (%d instances)"), TotalInstanceCount);
    }
    UE_LOG(LogTemp, Log, TEXT("Grass data SRVs set: Data0=%d, Data1=%d, Data2=%d"), 
        GrassData0SRV.IsValid() ? 1 : 0, GrassData1SRV.IsValid() ? 1 : 0, GrassData2SRV.IsValid() ? 1 : 0);

    // 初始化 LOD 0 Mesh 数据
    if (Component->GrassMesh && Component->GrassMesh->GetRenderData() && 
        Component->GrassMesh->GetRenderData()->LODResources.Num() > 0)
    {
        InitFromStaticMesh(Component->GrassMesh);
    }
    else
    {
        InitDefaultGrassBlade();
    }
    
    // 设置 LOD 0 的 LOD 级别
    VertexFactory.SetLODLevel(0);
    // 设置弯曲法线程度
    VertexFactory.SetCurvedNormalAmount(CurvedNormalAmount);
    // 设置视角依赖旋转强度 (对马岛之魂风格)
    VertexFactory.SetViewRotationAmount(ViewRotationAmount);
    VertexFactory.SetWindNoiseParameters(WindNoiseTextureRHI, WindNoiseScale, WindNoiseStrength, WindNoiseSpeed);
    VertexFactory.SetWindWaveParameters(WindWaveSpeed, WindWaveAmplitude, WindSinOffsetRange, WindPushTipForward);
    VertexFactory.SetLocalWindRotateAmount(LocalWindRotateAmount);

    // 初始化 LOD 1 Mesh 数据 (7 顶点简化版)
    InitLOD1GrassBlade();
    
    // 为 LOD 1 Vertex Factory 设置 SRV (使用 LOD 1 独立的 Visible Buffers)
    if (bEnableFrustumCulling && bUseIndirectDraw && VisiblePositionBufferLOD1SRV.IsValid())
    {
        VertexFactoryLOD1.SetInstancePositionSRV(VisiblePositionBufferLOD1SRV.GetReference(), TotalInstanceCount);
        VertexFactoryLOD1.SetGrassDataSRV(
            VisibleGrassData0BufferLOD1SRV.IsValid() ? VisibleGrassData0BufferLOD1SRV.GetReference() : nullptr,
            VisibleGrassData1BufferLOD1SRV.IsValid() ? VisibleGrassData1BufferLOD1SRV.GetReference() : nullptr,
            VisibleGrassData2BufferLOD1SRV.IsValid() ? VisibleGrassData2BufferLOD1SRV.GetReference() : nullptr
        );
    }
    else
    {
        VertexFactoryLOD1.SetInstancePositionSRV(PositionBufferSRV.GetReference(), TotalInstanceCount);
        VertexFactoryLOD1.SetGrassDataSRV(
            GrassData0SRV.IsValid() ? GrassData0SRV.GetReference() : nullptr,
            GrassData1SRV.IsValid() ? GrassData1SRV.GetReference() : nullptr,
            GrassData2SRV.IsValid() ? GrassData2SRV.GetReference() : nullptr
        );
    }
    
    // 设置 LOD 1 的 LOD 级别
    VertexFactoryLOD1.SetLODLevel(1);
    // 设置弯曲法线程度 (LOD 1 使用相同的值)
    VertexFactoryLOD1.SetCurvedNormalAmount(CurvedNormalAmount);
    // 设置视角依赖旋转强度 (LOD 1 使用相同的值)
    VertexFactoryLOD1.SetViewRotationAmount(ViewRotationAmount);
    VertexFactoryLOD1.SetWindNoiseParameters(WindNoiseTextureRHI, WindNoiseScale, WindNoiseStrength, WindNoiseSpeed);
    VertexFactoryLOD1.SetWindWaveParameters(WindWaveSpeed, WindWaveAmplitude, WindSinOffsetRange, WindPushTipForward);
    VertexFactoryLOD1.SetLocalWindRotateAmount(LocalWindRotateAmount);

    // 初始化渲染资源 (LOD 0)
    FStaticMeshVertexBuffers* VertexBuffersPtr = &VertexBuffers;
    FRawStaticIndexBuffer* IndexBufferPtr = &IndexBuffer;
    FGrassVertexFactory* VertexFactoryPtr = &VertexFactory;
    
    // 初始化渲染资源 (LOD 1)
    FStaticMeshVertexBuffers* VertexBuffersLOD1Ptr = &VertexBuffersLOD1;
    FRawStaticIndexBuffer* IndexBufferLOD1Ptr = &IndexBufferLOD1;
    FGrassVertexFactory* VertexFactoryLOD1Ptr = &VertexFactoryLOD1;
    
    ENQUEUE_RENDER_COMMAND(InitGrassResources)(
        [VertexBuffersPtr, IndexBufferPtr, VertexFactoryPtr, 
         VertexBuffersLOD1Ptr, IndexBufferLOD1Ptr, VertexFactoryLOD1Ptr](FRHICommandListImmediate& RHICmdList)
        {
            // 初始化 LOD 0 资源
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

            // 初始化 LOD 1 资源
            VertexBuffersLOD1Ptr->PositionVertexBuffer.InitResource(RHICmdList);
            VertexBuffersLOD1Ptr->StaticMeshVertexBuffer.InitResource(RHICmdList);
            VertexBuffersLOD1Ptr->ColorVertexBuffer.InitResource(RHICmdList);
            IndexBufferLOD1Ptr->InitResource(RHICmdList);

            FLocalVertexFactory::FDataType DataLOD1;
            VertexBuffersLOD1Ptr->PositionVertexBuffer.BindPositionVertexBuffer(VertexFactoryLOD1Ptr, DataLOD1);
            VertexBuffersLOD1Ptr->StaticMeshVertexBuffer.BindTangentVertexBuffer(VertexFactoryLOD1Ptr, DataLOD1);
            VertexBuffersLOD1Ptr->StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(VertexFactoryLOD1Ptr, DataLOD1);
            VertexBuffersLOD1Ptr->StaticMeshVertexBuffer.BindLightMapVertexBuffer(VertexFactoryLOD1Ptr, DataLOD1, 0);
            VertexBuffersLOD1Ptr->ColorVertexBuffer.BindColorVertexBuffer(VertexFactoryLOD1Ptr, DataLOD1);
            
            VertexFactoryLOD1Ptr->SetData(RHICmdList, DataLOD1);
            VertexFactoryLOD1Ptr->InitResource(RHICmdList);
        }
    );
    
    FlushRenderingCommands();
    
    // Register with ViewExtension for GPU Culling
    if (bEnableFrustumCulling && bUseIndirectDraw)
    {
        FGrassCullingViewExtension::Get()->RegisterGrassProxy(this);
    }
    
    UE_LOG(LogTemp, Log, TEXT("FGrassSceneProxy created: %d instances, LOD0=%d verts/%d tris, LOD1=%d verts/%d tris, IndirectDraw=%d, FrustumCulling=%d, LOD=%d (dist=%.0f)"), 
        TotalInstanceCount, NumVertices, NumPrimitives, NumVerticesLOD1, NumPrimitivesLOD1, 
        bUseIndirectDraw ? 1 : 0, bEnableFrustumCulling ? 1 : 0, bEnableLOD ? 1 : 0, LOD0Distance);
}

void FGrassSceneProxy::InitFromStaticMesh(UStaticMesh* StaticMesh)
{
    const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[0];
    
    NumVertices = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
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
        FVector3f TangentX = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
        FVector3f TangentZ = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i);
        FVector3f TangentY = FVector3f::CrossProduct(TangentZ, TangentX);
        VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(i, TangentX, TangentY, TangentZ);
        
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
    for (int32 i = 0; i < NumIndices; i++)
    {
        Indices[i] = LOD.IndexBuffer.GetIndex(i);
    }
    IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);
    
    UE_LOG(LogTemp, Log, TEXT("Initialized grass from StaticMesh: %s (%d vertices, %d triangles, %d UVs)"), 
        *StaticMesh->GetName(), NumVertices, NumPrimitives, NumTexCoords);
}

void FGrassSceneProxy::InitDefaultGrassBlade()
{
    // High quality grass blade mesh with 15 vertices (7 segments)
    // Original data: Vector3(0, Height, Width) where Width is signed (negative=left, positive=right)
    // Unreal coordinate: X = Width (left/right), Y = 0 (depth), Z = Height (up)
    // Scale: multiply by 100 to convert to centimeters
    const float Scale = 100.0f;
    
    // 15 vertices arranged as pairs from bottom to top
    // Index order matches original: 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14
    TArray<FVector3f> Positions = {
        FVector3f( 0.03445f * Scale, 0.0f, 0.15599f * Scale),  // 0: Row1 Right
        FVector3f(-0.03444f * Scale, 0.0f, 0.0f),              // 1: Bottom Left
        FVector3f( 0.03444f * Scale, 0.0f, 0.0f),              // 2: Bottom Right
        FVector3f(-0.03445f * Scale, 0.0f, 0.15599f * Scale),  // 3: Row1 Left
        FVector3f(-0.03193f * Scale, 0.0f, 0.27249f * Scale),  // 4: Row2 Left
        FVector3f( 0.03193f * Scale, 0.0f, 0.27249f * Scale),  // 5: Row2 Right
        FVector3f(-0.02942f * Scale, 0.0f, 0.38111f * Scale),  // 6: Row3 Left
        FVector3f( 0.02942f * Scale, 0.0f, 0.38111f * Scale),  // 7: Row3 Right
        FVector3f(-0.02620f * Scale, 0.0f, 0.47325f * Scale),  // 8: Row4 Left
        FVector3f( 0.02620f * Scale, 0.0f, 0.47325f * Scale),  // 9: Row4 Right
        FVector3f(-0.02338f * Scale, 0.0f, 0.55531f * Scale),  // 10: Row5 Left
        FVector3f( 0.02338f * Scale, 0.0f, 0.55531f * Scale),  // 11: Row5 Right
        FVector3f(-0.01728f * Scale, 0.0f, 0.63064f * Scale),  // 12: Row6 Left
        FVector3f( 0.01728f * Scale, 0.0f, 0.63064f * Scale),  // 13: Row6 Right
        FVector3f( 0.0f,             0.0f, 0.70819f * Scale),  // 14: Tip
    };

    // 13 triangles - CCW winding for front face (normal pointing +Y)
    TArray<uint32> Indices = {
        // Bottom quad (2 triangles) - connecting bottom edge to row 1
        1, 0, 2,    // BL -> R1R -> BR (front face)
        1, 3, 0,    // BL -> R1L -> R1R (front face)
        // Row 1 to Row 2
        3, 5, 0,    // R1L -> R2R -> R1R
        3, 4, 5,    // R1L -> R2L -> R2R
        // Row 2 to Row 3
        4, 7, 5,    // R2L -> R3R -> R2R
        4, 6, 7,    // R2L -> R3L -> R3R
        // Row 3 to Row 4
        6, 9, 7,    // R3L -> R4R -> R3R
        6, 8, 9,    // R3L -> R4L -> R4R
        // Row 4 to Row 5
        8, 11, 9,   // R4L -> R5R -> R4R
        8, 10, 11,  // R4L -> R5L -> R5R
        // Row 5 to Row 6
        10, 13, 11, // R5L -> R6R -> R5R
        10, 12, 13, // R5L -> R6L -> R6R
        // Row 6 to Tip
        12, 14, 13, // R6L -> Tip -> R6R
    };

    NumVertices = Positions.Num();
    NumIndices = Indices.Num();
    NumPrimitives = NumIndices / 3;

    VertexBuffers.PositionVertexBuffer.Init(Positions);
    VertexBuffers.StaticMeshVertexBuffer.Init(NumVertices, 1);

    // Max height for UV calculation
    const float MaxHeight = 0.70819f * Scale;
    const float MaxWidth = 0.03445f * Scale;

    for (int32 i = 0; i < NumVertices; i++)
    {
        // 计算草叶的切线空间
        // 法线指向 +Y 方向（正面朝向）- 这是初始状态，会在 Shader 中根据变形重新计算
        // TangentX = 宽度方向 (+X)
        // TangentY = 法线方向 (+Y) - 由 TangentX x TangentZ 计算得出
        // TangentZ = 高度方向 (+Z) - 但我们需要法线，所以这里存储法线
        FVector3f TangentX(1.0f, 0.0f, 0.0f);  // 沿宽度方向 (U)
        FVector3f TangentZ(0.0f, 1.0f, 0.0f);  // 法线方向 (草叶正面朝Y)
        FVector3f TangentY = FVector3f::CrossProduct(TangentZ, TangentX);
        
        VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(i, TangentX, TangentY, TangentZ);
        
        // UV: U = normalized X position (0 at left edge, 1 at right edge)
        //     V = normalized height (0 at bottom, 1 at top)
        // 这样材质可以根据 V 来做高度渐变效果
        float U = (Positions[i].X + MaxWidth) / (2.0f * MaxWidth);
        float V = Positions[i].Z / MaxHeight;
        VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(i, 0, FVector2f(FMath::Clamp(U, 0.0f, 1.0f), FMath::Clamp(V, 0.0f, 1.0f)));
    }

    VertexBuffers.ColorVertexBuffer.Init(NumVertices);
    for (int32 i = 0; i < NumVertices; i++)
    {
        // 顶点颜色存储草叶信息:
        // R = 归一化高度 (0 = 根部, 1 = 顶部) - 用于风动画和渐变
        // G = 左右侧标识 (0 = 左侧, 1 = 右侧) - 用于某些效果
        // B = 1 (保留)
        // A = 1 (保留)
        float HeightRatio = Positions[i].Z / MaxHeight;
        float SideRatio = (Positions[i].X + MaxWidth) / (2.0f * MaxWidth);  // 0 = left, 1 = right
        
        uint8 R = static_cast<uint8>(FMath::Clamp(HeightRatio * 255.0f, 0.0f, 255.0f));
        uint8 G = static_cast<uint8>(FMath::Clamp(SideRatio * 255.0f, 0.0f, 255.0f));
        uint8 B = 255;
        uint8 A = 255;
        VertexBuffers.ColorVertexBuffer.VertexColor(i) = FColor(R, G, B, A);
    }

    IndexBuffer.SetIndices(Indices, EIndexBufferStride::Force32Bit);
    
    UE_LOG(LogTemp, Log, TEXT("Initialized high-quality grass blade LOD0 (%d vertices, %d triangles)"), 
        NumVertices, NumPrimitives);
}

void FGrassSceneProxy::InitLOD1GrassBlade()
{
    // LOD 1: Simplified grass blade mesh with 7 vertices (3 segments)
    // Same scale and coordinate system as LOD 0
    const float Scale = 100.0f;
    
    // 7 vertices: Bottom pair, Middle pair, Upper pair, Tip
    // Simplified from 15 vertices to 7 vertices
    TArray<FVector3f> Positions = {
        FVector3f(-0.03444f * Scale, 0.0f, 0.0f),              // 0: Bottom Left
        FVector3f( 0.03444f * Scale, 0.0f, 0.0f),              // 1: Bottom Right
        FVector3f(-0.03193f * Scale, 0.0f, 0.27249f * Scale),  // 2: Middle Left
        FVector3f( 0.03193f * Scale, 0.0f, 0.27249f * Scale),  // 3: Middle Right
        FVector3f(-0.02338f * Scale, 0.0f, 0.55531f * Scale),  // 4: Upper Left
        FVector3f( 0.02338f * Scale, 0.0f, 0.55531f * Scale),  // 5: Upper Right
        FVector3f( 0.0f,             0.0f, 0.70819f * Scale),  // 6: Tip
    };

    // 5 triangles - CCW winding for front face
    TArray<uint32> Indices = {
        // Bottom to Middle
        0, 3, 1,    // BL -> MR -> BR
        0, 2, 3,    // BL -> ML -> MR
        // Middle to Upper
        2, 5, 3,    // ML -> UR -> MR
        2, 4, 5,    // ML -> UL -> UR
        // Upper to Tip
        4, 6, 5,    // UL -> Tip -> UR
    };

    NumVerticesLOD1 = Positions.Num();
    NumIndicesLOD1 = Indices.Num();
    NumPrimitivesLOD1 = NumIndicesLOD1 / 3;

    VertexBuffersLOD1.PositionVertexBuffer.Init(Positions);
    VertexBuffersLOD1.StaticMeshVertexBuffer.Init(NumVerticesLOD1, 1);

    // Max height for UV calculation (same as LOD 0)
    const float MaxHeight = 0.70819f * Scale;
    const float MaxWidth = 0.03444f * Scale;

    for (int32 i = 0; i < NumVerticesLOD1; i++)
    {
        // 切线空间设置 - 与 LOD 0 相同
        FVector3f TangentX(1.0f, 0.0f, 0.0f);
        FVector3f TangentZ(0.0f, 1.0f, 0.0f);
        FVector3f TangentY = FVector3f::CrossProduct(TangentZ, TangentX);
        
        VertexBuffersLOD1.StaticMeshVertexBuffer.SetVertexTangents(i, TangentX, TangentY, TangentZ);
        
        // UV: U = normalized X position, V = normalized height
        float U = (Positions[i].X + MaxWidth) / (2.0f * MaxWidth);
        float V = Positions[i].Z / MaxHeight;
        VertexBuffersLOD1.StaticMeshVertexBuffer.SetVertexUV(i, 0, FVector2f(FMath::Clamp(U, 0.0f, 1.0f), FMath::Clamp(V, 0.0f, 1.0f)));
    }

    VertexBuffersLOD1.ColorVertexBuffer.Init(NumVerticesLOD1);
    for (int32 i = 0; i < NumVerticesLOD1; i++)
    {
        // 顶点颜色与 LOD 0 相同格式
        float HeightRatio = Positions[i].Z / MaxHeight;
        float SideRatio = (Positions[i].X + MaxWidth) / (2.0f * MaxWidth);
        
        uint8 R = static_cast<uint8>(FMath::Clamp(HeightRatio * 255.0f, 0.0f, 255.0f));
        uint8 G = static_cast<uint8>(FMath::Clamp(SideRatio * 255.0f, 0.0f, 255.0f));
        uint8 B = 255;
        uint8 A = 255;
        VertexBuffersLOD1.ColorVertexBuffer.VertexColor(i) = FColor(R, G, B, A);
    }

    IndexBufferLOD1.SetIndices(Indices, EIndexBufferStride::Force32Bit);
    
    UE_LOG(LogTemp, Log, TEXT("Initialized simplified grass blade LOD1 (%d vertices, %d triangles)"), 
        NumVerticesLOD1, NumPrimitivesLOD1);
}

void FGrassSceneProxy::PerformGPUCullingRenderThread(FRHICommandListImmediate& RHICmdList, const FMatrix& ViewProjectionMatrix, const FVector& ViewOrigin, const FMatrix& LocalToWorldMatrix) const
{
    if (!bEnableFrustumCulling || !VisiblePositionBufferUAV.IsValid() || !IndirectArgsBufferUAV.IsValid())
    {
        return;
    }

    // Avoid executing multiple times per frame
    uint32 CurrentFrameNumber = GFrameNumber;
    if (bCullingPerformedThisFrame && LastFrameNumber == CurrentFrameNumber)
    {
        return;
    }
    bCullingPerformedThisFrame = true;
    LastFrameNumber = CurrentFrameNumber;

    // 计算距离淡出参数，传递给剔除着色器
    float FadeAtten = 1.0f;
    FVector BoundsCenter = GetBounds().Origin;
    float DistanceSq = FVector::DistSquared(FVector(ViewOrigin), BoundsCenter);
    
    const float FadeStartDistSq = 4000.0f * 4000.0f; // 40米
    const float FadeEndDistSq = 5000.0f * 5000.0f;   // 50米
    
    if (DistanceSq > FadeStartDistSq)
    {
        FadeAtten = FMath::Clamp((FadeEndDistSq - DistanceSq) / (FadeEndDistSq - FadeStartDistSq), 0.0f, 1.0f);
    }
    
    // 如果淡出效果很强，可以提前返回避免不必要的剔除计算
    if (FadeAtten < 0.1f)
    {
        return;
    }

    // 检查 LOD 功能是否完全可用（需要所有必要的 buffer）
    const bool bLODFullyEnabled = bEnableLOD && 
        IndirectArgsBufferLOD1.IsValid() && 
        IndirectArgsBufferLOD1UAV.IsValid() &&
        VisiblePositionBufferLOD1.IsValid() &&
        VisiblePositionBufferLOD1UAV.IsValid();

    // ========== Step 1: Reset Indirect Args Buffer (LOD 0 and LOD 1) ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));
        if (bLODFullyEnabled)
        {
            RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));
        }

        TShaderMapRef<FGrassResetIndirectArgsCS> ResetCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassResetIndirectArgsCS::FParameters ResetParams;
        ResetParams.OutIndirectArgs = IndirectArgsBufferUAV;
        ResetParams.OutIndirectArgsLOD1 = bLODFullyEnabled ? IndirectArgsBufferLOD1UAV : IndirectArgsBufferUAV;
        ResetParams.IndexCountPerInstance = NumIndices;
        ResetParams.IndexCountPerInstanceLOD1 = bLODFullyEnabled ? NumIndicesLOD1 : NumIndices;
        ResetParams.TotalInstanceCount = TotalInstanceCount;
        
        FComputeShaderUtils::Dispatch(RHICmdList, ResetCS, ResetParams, FIntVector(1, 1, 1));
    }

    // ========== Step 2: Execute Frustum Culling ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        
        // LOD 1 独立 Buffer 状态转换
        if (bLODFullyEnabled)
        {
            RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        }

        TShaderMapRef<FGrassFrustumCullingCS> CullingCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassFrustumCullingCS::FParameters CullingParams;
        
        CullingParams.InPositions = PositionBufferSRV;
        CullingParams.InGrassData0 = GrassData0SRV;
        CullingParams.InGrassData1 = GrassData1SRV;
        CullingParams.InGrassData2 = GrassData2SRV;
        CullingParams.OutVisiblePositions = VisiblePositionBufferUAV;
        CullingParams.OutVisibleGrassData0 = VisibleGrassData0UAV;
        CullingParams.OutVisibleGrassData1 = VisibleGrassData1UAV;
        CullingParams.OutVisibleGrassData2 = VisibleGrassData2UAV;
        // LOD 1 独立输出 Buffers - 只有当 LOD 完全启用时才使用独立 buffer
        CullingParams.OutVisiblePositionsLOD1 = bLODFullyEnabled ? VisiblePositionBufferLOD1UAV : VisiblePositionBufferUAV;
        CullingParams.OutVisibleGrassData0LOD1 = bLODFullyEnabled ? VisibleGrassData0BufferLOD1UAV : VisibleGrassData0UAV;
        CullingParams.OutVisibleGrassData1LOD1 = bLODFullyEnabled ? VisibleGrassData1BufferLOD1UAV : VisibleGrassData1UAV;
        CullingParams.OutVisibleGrassData2LOD1 = bLODFullyEnabled ? VisibleGrassData2BufferLOD1UAV : VisibleGrassData2UAV;
        CullingParams.OutIndirectArgs = IndirectArgsBufferUAV;
        CullingParams.OutIndirectArgsLOD1 = bLODFullyEnabled ? IndirectArgsBufferLOD1UAV : IndirectArgsBufferUAV;
        CullingParams.TotalInstanceCount = TotalInstanceCount;
        CullingParams.IndexCountPerInstance = NumIndices;
        CullingParams.IndexCountPerInstanceLOD1 = bLODFullyEnabled ? NumIndicesLOD1 : NumIndices;
        // 只有当 LOD 完全启用时才传递 LOD0Distance，否则传递 0（禁用 LOD 分离）
        CullingParams.LOD0Distance = bLODFullyEnabled ? LOD0Distance : 0.0f;
        
        // Extract frustum planes from ViewProjectionMatrix
        FPlane FrustumPlanes[6];
        
        // Left
        FrustumPlanes[0] = FPlane(
            ViewProjectionMatrix.M[0][3] + ViewProjectionMatrix.M[0][0],
            ViewProjectionMatrix.M[1][3] + ViewProjectionMatrix.M[1][0],
            ViewProjectionMatrix.M[2][3] + ViewProjectionMatrix.M[2][0],
            ViewProjectionMatrix.M[3][3] + ViewProjectionMatrix.M[3][0]);
        // Right
        FrustumPlanes[1] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][0],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][0],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][0],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][0]);
        // Bottom
        FrustumPlanes[2] = FPlane(
            ViewProjectionMatrix.M[0][3] + ViewProjectionMatrix.M[0][1],
            ViewProjectionMatrix.M[1][3] + ViewProjectionMatrix.M[1][1],
            ViewProjectionMatrix.M[2][3] + ViewProjectionMatrix.M[2][1],
            ViewProjectionMatrix.M[3][3] + ViewProjectionMatrix.M[3][1]);
        // Top
        FrustumPlanes[3] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][1],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][1],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][1],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][1]);
        // Near
        FrustumPlanes[4] = FPlane(
            ViewProjectionMatrix.M[0][2],
            ViewProjectionMatrix.M[1][2],
            ViewProjectionMatrix.M[2][2],
            ViewProjectionMatrix.M[3][2]);
        // Far
        FrustumPlanes[5] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][2],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][2],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][2],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][2]);

        // Normalize planes and pass to shader
        for (int i = 0; i < 6; i++)
        {
            float Length = FMath::Sqrt(
                FrustumPlanes[i].X * FrustumPlanes[i].X + 
                FrustumPlanes[i].Y * FrustumPlanes[i].Y + 
                FrustumPlanes[i].Z * FrustumPlanes[i].Z);
            if (Length > SMALL_NUMBER)
            {
                FrustumPlanes[i].X /= Length;
                FrustumPlanes[i].Y /= Length;
                FrustumPlanes[i].Z /= Length;
                FrustumPlanes[i].W /= Length;
            }
            CullingParams.FrustumPlanes[i] = FVector4f(
                FrustumPlanes[i].X, FrustumPlanes[i].Y, FrustumPlanes[i].Z, FrustumPlanes[i].W);
        }
        
        // LocalToWorld transform matrix
        CullingParams.LocalToWorld = FMatrix44f(LocalToWorldMatrix);
        
        // Culling parameters
        CullingParams.BoundingRadius = GrassBoundingRadius;
        CullingParams.MaxVisibleDistance = bEnableDistanceCulling ? MaxVisibleDistance : 0.0f;
        CullingParams.CameraPosition = FVector3f(ViewOrigin);

        // Dispatch
        int32 NumGroups = FMath::DivideAndRoundUp((int32)TotalInstanceCount, 64);
        FComputeShaderUtils::Dispatch(RHICmdList, CullingCS, CullingParams, FIntVector(NumGroups, 1, 1));
    }

    // ========== Step 3: Transition resource states ==========
    RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
    if (bLODFullyEnabled)
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
        // LOD 1 独立 Buffer 状态转换
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    }
}

void FGrassSceneProxy::PerformGPUCulling(FRHICommandListImmediate& RHICmdList, const FSceneView* View) const
{
    if (!bEnableFrustumCulling || !VisiblePositionBufferUAV.IsValid() || !IndirectArgsBufferUAV.IsValid())
    {
        return;
    }

    // 避免同一帧重复执行
    uint32 CurrentFrameNumber = GFrameNumber;
    if (bCullingPerformedThisFrame && LastFrameNumber == CurrentFrameNumber)
    {
        return;
    }
    bCullingPerformedThisFrame = true;
    LastFrameNumber = CurrentFrameNumber;

    // 检查 LOD 功能是否完全可用（需要所有必要的 buffer）
    const bool bLODFullyEnabled = bEnableLOD && 
        IndirectArgsBufferLOD1.IsValid() && 
        IndirectArgsBufferLOD1UAV.IsValid() &&
        VisiblePositionBufferLOD1.IsValid() &&
        VisiblePositionBufferLOD1UAV.IsValid();

    // ========== Step 1: 重置 Indirect Args Buffer (LOD 0 和 LOD 1) ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));
        if (bLODFullyEnabled)
        {
            RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));
        }

        TShaderMapRef<FGrassResetIndirectArgsCS> ResetCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassResetIndirectArgsCS::FParameters ResetParams;
        ResetParams.OutIndirectArgs = IndirectArgsBufferUAV;
        ResetParams.OutIndirectArgsLOD1 = bLODFullyEnabled ? IndirectArgsBufferLOD1UAV : IndirectArgsBufferUAV;
        ResetParams.IndexCountPerInstance = NumIndices;
        ResetParams.IndexCountPerInstanceLOD1 = bLODFullyEnabled ? NumIndicesLOD1 : NumIndices;
        ResetParams.TotalInstanceCount = TotalInstanceCount;
        
        FComputeShaderUtils::Dispatch(RHICmdList, ResetCS, ResetParams, FIntVector(1, 1, 1));
    }

    // ========== Step 2: 执行 Frustum Culling ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        
        // LOD 1 独立 Buffer 状态转换
        if (bLODFullyEnabled)
        {
            RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        }

        TShaderMapRef<FGrassFrustumCullingCS> CullingCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassFrustumCullingCS::FParameters CullingParams;
        
        CullingParams.InPositions = PositionBufferSRV;
        CullingParams.InGrassData0 = GrassData0SRV;
        CullingParams.InGrassData1 = GrassData1SRV;
        CullingParams.InGrassData2 = GrassData2SRV;
        CullingParams.OutVisiblePositions = VisiblePositionBufferUAV;
        CullingParams.OutVisibleGrassData0 = VisibleGrassData0UAV;
        CullingParams.OutVisibleGrassData1 = VisibleGrassData1UAV;
        CullingParams.OutVisibleGrassData2 = VisibleGrassData2UAV;
        // LOD 1 独立输出 Buffers - 只有当 LOD 完全启用时才使用独立 buffer
        CullingParams.OutVisiblePositionsLOD1 = bLODFullyEnabled ? VisiblePositionBufferLOD1UAV : VisiblePositionBufferUAV;
        CullingParams.OutVisibleGrassData0LOD1 = bLODFullyEnabled ? VisibleGrassData0BufferLOD1UAV : VisibleGrassData0UAV;
        CullingParams.OutVisibleGrassData1LOD1 = bLODFullyEnabled ? VisibleGrassData1BufferLOD1UAV : VisibleGrassData1UAV;
        CullingParams.OutVisibleGrassData2LOD1 = bLODFullyEnabled ? VisibleGrassData2BufferLOD1UAV : VisibleGrassData2UAV;
        CullingParams.OutIndirectArgs = IndirectArgsBufferUAV;
        CullingParams.OutIndirectArgsLOD1 = bLODFullyEnabled ? IndirectArgsBufferLOD1UAV : IndirectArgsBufferUAV;
        CullingParams.TotalInstanceCount = TotalInstanceCount;
        CullingParams.IndexCountPerInstance = NumIndices;
        CullingParams.IndexCountPerInstanceLOD1 = bLODFullyEnabled ? NumIndicesLOD1 : NumIndices;
        // 只有当 LOD 完全启用时才传递 LOD0Distance，否则传递 0（禁用 LOD 分离）
        CullingParams.LOD0Distance = bLODFullyEnabled ? LOD0Distance : 0.0f;
        
        // 提取视锥平面
        const FMatrix ViewProjectionMatrix = View->ViewMatrices.GetViewProjectionMatrix();
        FPlane FrustumPlanes[6];
        
        // 提取视锥的 6 个平面
        // Left
        FrustumPlanes[0] = FPlane(
            ViewProjectionMatrix.M[0][3] + ViewProjectionMatrix.M[0][0],
            ViewProjectionMatrix.M[1][3] + ViewProjectionMatrix.M[1][0],
            ViewProjectionMatrix.M[2][3] + ViewProjectionMatrix.M[2][0],
            ViewProjectionMatrix.M[3][3] + ViewProjectionMatrix.M[3][0]);
        // Right
        FrustumPlanes[1] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][0],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][0],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][0],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][0]);
        // Bottom
        FrustumPlanes[2] = FPlane(
            ViewProjectionMatrix.M[0][3] + ViewProjectionMatrix.M[0][1],
            ViewProjectionMatrix.M[1][3] + ViewProjectionMatrix.M[1][1],
            ViewProjectionMatrix.M[2][3] + ViewProjectionMatrix.M[2][1],
            ViewProjectionMatrix.M[3][3] + ViewProjectionMatrix.M[3][1]);
        // Top
        FrustumPlanes[3] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][1],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][1],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][1],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][1]);
        // Near
        FrustumPlanes[4] = FPlane(
            ViewProjectionMatrix.M[0][2],
            ViewProjectionMatrix.M[1][2],
            ViewProjectionMatrix.M[2][2],
            ViewProjectionMatrix.M[3][2]);
        // Far
        FrustumPlanes[5] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][2],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][2],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][2],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][2]);

        // 归一化平面并传递给 shader
        for (int i = 0; i < 6; i++)
        {
            float Length = FMath::Sqrt(
                FrustumPlanes[i].X * FrustumPlanes[i].X + 
                FrustumPlanes[i].Y * FrustumPlanes[i].Y + 
                FrustumPlanes[i].Z * FrustumPlanes[i].Z);
            if (Length > SMALL_NUMBER)
            {
                FrustumPlanes[i].X /= Length;
                FrustumPlanes[i].Y /= Length;
                FrustumPlanes[i].Z /= Length;
                FrustumPlanes[i].W /= Length;
            }
            CullingParams.FrustumPlanes[i] = FVector4f(
                FrustumPlanes[i].X, FrustumPlanes[i].Y, FrustumPlanes[i].Z, FrustumPlanes[i].W);
        }
        
        // LocalToWorld 变换矩阵
        CullingParams.LocalToWorld = FMatrix44f(GetLocalToWorld());
        
        // 剔除参数
        CullingParams.BoundingRadius = GrassBoundingRadius;
        CullingParams.MaxVisibleDistance = bEnableDistanceCulling ? MaxVisibleDistance : 0.0f;
        CullingParams.CameraPosition = FVector3f(View->ViewMatrices.GetViewOrigin());

        // Dispatch
        int32 NumGroups = FMath::DivideAndRoundUp((int32)TotalInstanceCount, 64);
        FComputeShaderUtils::Dispatch(RHICmdList, CullingCS, CullingParams, FIntVector(NumGroups, 1, 1));
    }

    // ========== Step 3: 转换资源状态 ==========
    RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
    if (bLODFullyEnabled)
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    }
}

void FGrassSceneProxy::PerformGPUCullingWithHiZ(
    FRHICommandListImmediate& RHICmdList,
    const FSceneView* View,
    FRHITexture* HiZTexture,
    FIntPoint HiZSize,
    const FMatrix& HiZViewProjectionMatrix) const
{
    if (!bEnableFrustumCulling || !VisiblePositionBufferUAV.IsValid() || !IndirectArgsBufferUAV.IsValid())
    {
        return;
    }

    // 避免同一帧重复执行
    uint32 CurrentFrameNumber = GFrameNumber;
    if (bCullingPerformedThisFrame && LastFrameNumber == CurrentFrameNumber)
    {
        return;
    }
    bCullingPerformedThisFrame = true;
    LastFrameNumber = CurrentFrameNumber;

    // 检查 LOD 功能是否完全可用
    const bool bLODFullyEnabled = bEnableLOD && 
        IndirectArgsBufferLOD1.IsValid() && 
        IndirectArgsBufferLOD1UAV.IsValid() &&
        VisiblePositionBufferLOD1.IsValid() &&
        VisiblePositionBufferLOD1UAV.IsValid();

    // ========== Step 1: 重置 Indirect Args Buffer ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));
        if (bLODFullyEnabled)
        {
            RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));
        }

        TShaderMapRef<FGrassResetIndirectArgsCS> ResetCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassResetIndirectArgsCS::FParameters ResetParams;
        ResetParams.OutIndirectArgs = IndirectArgsBufferUAV;
        ResetParams.OutIndirectArgsLOD1 = bLODFullyEnabled ? IndirectArgsBufferLOD1UAV : IndirectArgsBufferUAV;
        ResetParams.IndexCountPerInstance = NumIndices;
        ResetParams.IndexCountPerInstanceLOD1 = bLODFullyEnabled ? NumIndicesLOD1 : NumIndices;
        ResetParams.TotalInstanceCount = TotalInstanceCount;
        
        FComputeShaderUtils::Dispatch(RHICmdList, ResetCS, ResetParams, FIntVector(1, 1, 1));
    }

    // ========== Step 2: 执行 Frustum + Hi-Z Occlusion Culling ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2Buffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        
        if (bLODFullyEnabled)
        {
            RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
            RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2BufferLOD1, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));
        }

        TShaderMapRef<FGrassFrustumCullingCS> CullingCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassFrustumCullingCS::FParameters CullingParams;
        
        // Input buffers
        CullingParams.InPositions = PositionBufferSRV;
        CullingParams.InGrassData0 = GrassData0SRV;
        CullingParams.InGrassData1 = GrassData1SRV;
        CullingParams.InGrassData2 = GrassData2SRV;
        
        // Output buffers (LOD 0)
        CullingParams.OutVisiblePositions = VisiblePositionBufferUAV;
        CullingParams.OutVisibleGrassData0 = VisibleGrassData0UAV;
        CullingParams.OutVisibleGrassData1 = VisibleGrassData1UAV;
        CullingParams.OutVisibleGrassData2 = VisibleGrassData2UAV;
        
        // Output buffers (LOD 1)
        CullingParams.OutVisiblePositionsLOD1 = bLODFullyEnabled ? VisiblePositionBufferLOD1UAV : VisiblePositionBufferUAV;
        CullingParams.OutVisibleGrassData0LOD1 = bLODFullyEnabled ? VisibleGrassData0BufferLOD1UAV : VisibleGrassData0UAV;
        CullingParams.OutVisibleGrassData1LOD1 = bLODFullyEnabled ? VisibleGrassData1BufferLOD1UAV : VisibleGrassData1UAV;
        CullingParams.OutVisibleGrassData2LOD1 = bLODFullyEnabled ? VisibleGrassData2BufferLOD1UAV : VisibleGrassData2UAV;
        
        CullingParams.OutIndirectArgs = IndirectArgsBufferUAV;
        CullingParams.OutIndirectArgsLOD1 = bLODFullyEnabled ? IndirectArgsBufferLOD1UAV : IndirectArgsBufferUAV;
        
        // Instance params
        CullingParams.TotalInstanceCount = TotalInstanceCount;
        CullingParams.IndexCountPerInstance = NumIndices;
        CullingParams.IndexCountPerInstanceLOD1 = bLODFullyEnabled ? NumIndicesLOD1 : NumIndices;
        CullingParams.LOD0Distance = bLODFullyEnabled ? LOD0Distance : 0.0f;
        
        // 提取视锥平面
        const FMatrix ViewProjectionMatrix = View->ViewMatrices.GetViewProjectionMatrix();
        FPlane FrustumPlanes[6];
        
        // Left
        FrustumPlanes[0] = FPlane(
            ViewProjectionMatrix.M[0][3] + ViewProjectionMatrix.M[0][0],
            ViewProjectionMatrix.M[1][3] + ViewProjectionMatrix.M[1][0],
            ViewProjectionMatrix.M[2][3] + ViewProjectionMatrix.M[2][0],
            ViewProjectionMatrix.M[3][3] + ViewProjectionMatrix.M[3][0]);
        // Right
        FrustumPlanes[1] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][0],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][0],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][0],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][0]);
        // Bottom
        FrustumPlanes[2] = FPlane(
            ViewProjectionMatrix.M[0][3] + ViewProjectionMatrix.M[0][1],
            ViewProjectionMatrix.M[1][3] + ViewProjectionMatrix.M[1][1],
            ViewProjectionMatrix.M[2][3] + ViewProjectionMatrix.M[2][1],
            ViewProjectionMatrix.M[3][3] + ViewProjectionMatrix.M[3][1]);
        // Top
        FrustumPlanes[3] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][1],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][1],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][1],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][1]);
        // Near
        FrustumPlanes[4] = FPlane(
            ViewProjectionMatrix.M[0][2],
            ViewProjectionMatrix.M[1][2],
            ViewProjectionMatrix.M[2][2],
            ViewProjectionMatrix.M[3][2]);
        // Far
        FrustumPlanes[5] = FPlane(
            ViewProjectionMatrix.M[0][3] - ViewProjectionMatrix.M[0][2],
            ViewProjectionMatrix.M[1][3] - ViewProjectionMatrix.M[1][2],
            ViewProjectionMatrix.M[2][3] - ViewProjectionMatrix.M[2][2],
            ViewProjectionMatrix.M[3][3] - ViewProjectionMatrix.M[3][2]);

        // 归一化平面
        for (int i = 0; i < 6; i++)
        {
            float Length = FMath::Sqrt(
                FrustumPlanes[i].X * FrustumPlanes[i].X + 
                FrustumPlanes[i].Y * FrustumPlanes[i].Y + 
                FrustumPlanes[i].Z * FrustumPlanes[i].Z);
            if (Length > SMALL_NUMBER)
            {
                FrustumPlanes[i].X /= Length;
                FrustumPlanes[i].Y /= Length;
                FrustumPlanes[i].Z /= Length;
                FrustumPlanes[i].W /= Length;
            }
            CullingParams.FrustumPlanes[i] = FVector4f(
                FrustumPlanes[i].X, FrustumPlanes[i].Y, FrustumPlanes[i].Z, FrustumPlanes[i].W);
        }
        
        CullingParams.LocalToWorld = FMatrix44f(GetLocalToWorld());
        CullingParams.BoundingRadius = GrassBoundingRadius;
        CullingParams.MaxVisibleDistance = bEnableDistanceCulling ? MaxVisibleDistance : 0.0f;
        CullingParams.CameraPosition = FVector3f(View->ViewMatrices.GetViewOrigin());
        
        // ========== Hi-Z 遮挡剔除参数 ==========
        const bool bUseHiZ = bEnableOcclusionCulling && HiZTexture != nullptr && HiZSize.X > 0 && HiZSize.Y > 0;
        
        CullingParams.bEnableOcclusionCulling = bUseHiZ ? 1 : 0;
        CullingParams.HiZTexture = bUseHiZ ? HiZTexture : GBlackTexture->TextureRHI.GetReference();
        CullingParams.HiZSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        CullingParams.HiZSize = bUseHiZ ? FVector2f(HiZSize.X, HiZSize.Y) : FVector2f(1.0f, 1.0f);
        // 使用上一帧的 ViewProjectionMatrix 进行遮挡测试（因为 Hi-Z 是上一帧生成的）
        CullingParams.ViewProjectionMatrix = FMatrix44f(HiZViewProjectionMatrix);

        // Dispatch
        int32 NumGroups = FMath::DivideAndRoundUp((int32)TotalInstanceCount, 64);
        FComputeShaderUtils::Dispatch(RHICmdList, CullingCS, CullingParams, FIntVector(NumGroups, 1, 1));
    }

    // ========== Step 3: 转换资源状态 ==========
    RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2Buffer, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
    if (bLODFullyEnabled)
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData0BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData1BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
        RHICmdList.Transition(FRHITransitionInfo(VisibleGrassData2BufferLOD1, ERHIAccess::UAVCompute, ERHIAccess::SRVMask));
    }
}

FGrassSceneProxy::~FGrassSceneProxy()
{
    // Unregister from ViewExtension
    if (bEnableFrustumCulling && bUseIndirectDraw)
    {
        if (auto Extension = FGrassCullingViewExtension::Get())
        {
            Extension->UnregisterGrassProxy(this);
        }
    }
    
    // 释放 LOD 0 资源
    VertexBuffers.PositionVertexBuffer.ReleaseResource();
    VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
    VertexBuffers.ColorVertexBuffer.ReleaseResource();
    IndexBuffer.ReleaseResource();
    VertexFactory.ReleaseResource();
    
    // 释放 LOD 1 资源
    VertexBuffersLOD1.PositionVertexBuffer.ReleaseResource();
    VertexBuffersLOD1.StaticMeshVertexBuffer.ReleaseResource();
    VertexBuffersLOD1.ColorVertexBuffer.ReleaseResource();
    IndexBufferLOD1.ReleaseResource();
    VertexFactoryLOD1.ReleaseResource();
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
    Result.bRenderInMainPass = ShouldRenderInMainPass();
    Result.bRenderInDepthPass = ShouldRenderInDepthPass();
    Result.bRenderCustomDepth = ShouldRenderCustomDepth();
    Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
    Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
    
    // 启用动态光照
    Result.bDynamicRelevance = true;
    
    // 支持光照贴图
    Result.bStaticRelevance = false; // 不使用静态光照
    
    // 支持阴影
    Result.bShadowRelevance = IsShadowCast(View);
    
    // 设置材质相关标志
    Result.bUsesSingleLayerWaterMaterial = false;
    
    // 草叶渲染为不透明物体
    // 远处的淡出效果应该通过材质中的 Dithered Opacity 或距离剔除来实现
    // 而不是通过切换到透明渲染（透明渲染会带来排序问题和性能开销）
    Result.bOpaque = true;
    
    // 更新 bVelocityRelevance（需要在设置 bOpaque 之后）
    Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
    
    return Result;
}

// NOTE: 远处草叶的淡出效果建议通过以下方式实现：
// 1. 材质中使用 Dithered Opacity (抖动透明) - 避免透明排序问题
// 2. GPU Culling 中的距离剔除 (MaxVisibleDistance 参数)
// 3. LOD 系统在远处使用更简化的网格
// 
// 避免在 GetViewRelevance 中动态切换 Opaque/Translucent，
// 因为这会导致渲染排序问题和显著的性能开销。

void FGrassSceneProxy::GetDynamicMeshElements(
    const TArray<const FSceneView*>& Views,
    const FSceneViewFamily& ViewFamily,
    uint32 VisibilityMap,
    FMeshElementCollector& Collector) const
{
    if (TotalInstanceCount == 0)
        return;

    // Validate material
    if (!Material)
    {
        return;
    }

    FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();
    if (!MaterialProxy)
    {
        return;
    }

    // NOTE: GPU Culling is now executed by FGrassCullingViewExtension::PreRenderViewFamily_RenderThread()
    // before this function is called, so we don't need to call PerformGPUCulling here.

    for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
    {
        if (!(VisibilityMap & (1 << ViewIndex)))
            continue;

        // Validate vertex factory is initialized
        if (!VertexFactory.IsInitialized())
        {
            continue;
        }

        // ========== LOD 0: 高质量草叶 (15 顶点) ==========
        {
            FMeshBatch& Mesh = Collector.AllocateMesh();
            Mesh.VertexFactory = &VertexFactory;
            Mesh.MaterialRenderProxy = MaterialProxy;
            Mesh.Type = PT_TriangleList;
            Mesh.DepthPriorityGroup = SDPG_World;
            Mesh.bCanApplyViewModeOverrides = true;
            Mesh.ReverseCulling = false;
            Mesh.CastShadow = false;
            Mesh.bDisableBackfaceCulling = true;

            FMeshBatchElement& Element = Mesh.Elements[0];
            Element.IndexBuffer = &IndexBuffer;
            Element.FirstIndex = 0;
            Element.MinVertexIndex = 0;
            Element.MaxVertexIndex = NumVertices - 1;
            Element.PrimitiveUniformBuffer = GetUniformBuffer();

            if (bUseIndirectDraw && IndirectArgsBuffer.IsValid())
            {
                // Indirect Draw: GPU driven draw call
                Element.NumPrimitives = 0;
                Element.NumInstances = 0;
                Element.IndirectArgsBuffer = IndirectArgsBuffer;
                Element.IndirectArgsOffset = 0;
            }
            else
            {
                // Standard GPU Instancing (without culling)
                Element.NumPrimitives = NumPrimitives;
                Element.NumInstances = TotalInstanceCount;
            }

            Collector.AddMesh(ViewIndex, Mesh);
        }

        // ========== LOD 1: 简化草叶 (7 顶点) ==========
        if (bEnableLOD && VertexFactoryLOD1.IsInitialized() && bUseIndirectDraw && IndirectArgsBufferLOD1.IsValid())
        {
            FMeshBatch& MeshLOD1 = Collector.AllocateMesh();
            MeshLOD1.VertexFactory = &VertexFactoryLOD1;
            MeshLOD1.MaterialRenderProxy = MaterialProxy;
            MeshLOD1.Type = PT_TriangleList;
            MeshLOD1.DepthPriorityGroup = SDPG_World;
            MeshLOD1.bCanApplyViewModeOverrides = true;
            MeshLOD1.ReverseCulling = false;
            MeshLOD1.CastShadow = false;
            MeshLOD1.bDisableBackfaceCulling = true;

            FMeshBatchElement& ElementLOD1 = MeshLOD1.Elements[0];
            ElementLOD1.IndexBuffer = &IndexBufferLOD1;
            ElementLOD1.FirstIndex = 0;
            ElementLOD1.MinVertexIndex = 0;
            ElementLOD1.MaxVertexIndex = NumVerticesLOD1 - 1;
            ElementLOD1.PrimitiveUniformBuffer = GetUniformBuffer();

            // LOD 1 uses its own IndirectArgsBuffer
            ElementLOD1.NumPrimitives = 0;
            ElementLOD1.NumInstances = 0;
            ElementLOD1.IndirectArgsBuffer = IndirectArgsBufferLOD1;
            ElementLOD1.IndirectArgsOffset = 0;

            Collector.AddMesh(ViewIndex, MeshLOD1);
        }
    }
}