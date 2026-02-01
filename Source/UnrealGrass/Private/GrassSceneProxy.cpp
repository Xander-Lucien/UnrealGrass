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


// ============================================================================
// GPU Frustum Culling Compute Shader
// ============================================================================
class FGrassFrustumCullingCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGrassFrustumCullingCS);
    SHADER_USE_PARAMETER_STRUCT(FGrassFrustumCullingCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(StructuredBuffer<FVector3f>, InPositions)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector3f>, OutVisiblePositions)
        SHADER_PARAMETER_UAV(RWBuffer<uint>, OutIndirectArgs)
        SHADER_PARAMETER(uint32, TotalInstanceCount)
        SHADER_PARAMETER(uint32, IndexCountPerInstance)
        SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
        SHADER_PARAMETER(FMatrix44f, LocalToWorld)
        SHADER_PARAMETER(float, BoundingRadius)
        SHADER_PARAMETER(float, MaxVisibleDistance)
        SHADER_PARAMETER(FVector3f, CameraPosition)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FGrassFrustumCullingCS, "/Plugin/UnrealGrass/Private/GrassFrustumCulling.usf", "MainCS", SF_Compute);

// 重置 Indirect Args 的 Compute Shader
class FGrassResetIndirectArgsCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FGrassResetIndirectArgsCS);
    SHADER_USE_PARAMETER_STRUCT(FGrassResetIndirectArgsCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_UAV(RWBuffer<uint>, OutIndirectArgs)
        SHADER_PARAMETER(uint32, IndexCountPerInstance)
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
    , PositionBuffer(Component->PositionBuffer)
    , PositionBufferSRV(Component->PositionBufferSRV)
    , TotalInstanceCount(Component->InstanceCount)
    , VisiblePositionBuffer(Component->VisiblePositionBuffer)
    , VisiblePositionBufferSRV(Component->VisiblePositionBufferSRV)
    , VisiblePositionBufferUAV(Component->VisiblePositionBufferUAV)
    , bUseIndirectDraw(Component->bUseIndirectDraw)
    , IndirectArgsBuffer(Component->IndirectArgsBuffer)
    , IndirectArgsBufferUAV(Component->IndirectArgsBufferUAV)
    , bEnableFrustumCulling(Component->bEnableFrustumCulling)
    , bEnableDistanceCulling(Component->bEnableDistanceCulling)
    , MaxVisibleDistance(Component->MaxVisibleDistance)
    , GrassBoundingRadius(Component->GrassBoundingRadius)
    , Material(Component->GrassMaterial)
{
    bVerifyUsedMaterials = false;
    
    if (!Material)
    {
        Material = UMaterial::GetDefaultMaterial(MD_Surface);
    }

    // IMPORTANT: GPU Culling is not yet fully implemented
    // Always use PositionBufferSRV (all instances) for now
    // When GPU Culling is properly implemented, switch to VisiblePositionBufferSRV
    if (bEnableFrustumCulling && bUseIndirectDraw && VisiblePositionBufferSRV.IsValid())
    {
        // GPU Culling enabled: use VisiblePositionBufferSRV (will be filled by culling shader)
        VertexFactory.SetInstancePositionSRV(VisiblePositionBufferSRV.GetReference(), TotalInstanceCount);
        UE_LOG(LogTemp, Log, TEXT("Using VisiblePositionBufferSRV for rendering (GPU Culling enabled, %d max instances)"), TotalInstanceCount);
    }
    else
    {
        // No GPU Culling: use all positions
        VertexFactory.SetInstancePositionSRV(PositionBufferSRV.GetReference(), TotalInstanceCount);
        UE_LOG(LogTemp, Log, TEXT("Using PositionBufferSRV for rendering (%d instances)"), TotalInstanceCount);
    }

    // 初始化 Mesh 数据
    if (Component->GrassMesh && Component->GrassMesh->GetRenderData() && 
        Component->GrassMesh->GetRenderData()->LODResources.Num() > 0)
    {
        InitFromStaticMesh(Component->GrassMesh);
    }
    else
    {
        InitDefaultGrassBlade();
    }

    // 初始化渲染资源
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
    
    // Register with ViewExtension for GPU Culling
    if (bEnableFrustumCulling && bUseIndirectDraw)
    {
        FGrassCullingViewExtension::Get()->RegisterGrassProxy(this);
    }
    
    UE_LOG(LogTemp, Log, TEXT("FGrassSceneProxy created: %d instances, %d vertices, %d triangles, IndirectDraw=%d, FrustumCulling=%d"), 
        TotalInstanceCount, NumVertices, NumPrimitives, bUseIndirectDraw ? 1 : 0, bEnableFrustumCulling ? 1 : 0);
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

    // ========== Step 1: Reset Indirect Args Buffer ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));

        TShaderMapRef<FGrassResetIndirectArgsCS> ResetCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassResetIndirectArgsCS::FParameters ResetParams;
        ResetParams.OutIndirectArgs = IndirectArgsBufferUAV;
        ResetParams.IndexCountPerInstance = NumIndices;
        
        FComputeShaderUtils::Dispatch(RHICmdList, ResetCS, ResetParams, FIntVector(1, 1, 1));
    }

    // ========== Step 2: Execute Frustum Culling ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));

        TShaderMapRef<FGrassFrustumCullingCS> CullingCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassFrustumCullingCS::FParameters CullingParams;
        
        CullingParams.InPositions = PositionBufferSRV;
        CullingParams.OutVisiblePositions = VisiblePositionBufferUAV;
        CullingParams.OutIndirectArgs = IndirectArgsBufferUAV;
        CullingParams.TotalInstanceCount = TotalInstanceCount;
        CullingParams.IndexCountPerInstance = NumIndices;
        
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
    RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
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

    // ========== Step 1: 重置 Indirect Args Buffer ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::UAVCompute));

        TShaderMapRef<FGrassResetIndirectArgsCS> ResetCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassResetIndirectArgsCS::FParameters ResetParams;
        ResetParams.OutIndirectArgs = IndirectArgsBufferUAV;
        ResetParams.IndexCountPerInstance = NumIndices;
        
        FComputeShaderUtils::Dispatch(RHICmdList, ResetCS, ResetParams, FIntVector(1, 1, 1));
    }

    // ========== Step 2: 执行 Frustum Culling ==========
    {
        RHICmdList.Transition(FRHITransitionInfo(VisiblePositionBuffer, ERHIAccess::SRVMask, ERHIAccess::UAVCompute));

        TShaderMapRef<FGrassFrustumCullingCS> CullingCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FGrassFrustumCullingCS::FParameters CullingParams;
        
        CullingParams.InPositions = PositionBufferSRV;
        CullingParams.OutVisiblePositions = VisiblePositionBufferUAV;
        CullingParams.OutIndirectArgs = IndirectArgsBufferUAV;
        CullingParams.TotalInstanceCount = TotalInstanceCount;
        CullingParams.IndexCountPerInstance = NumIndices;
        
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
    RHICmdList.Transition(FRHITransitionInfo(IndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
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
}

