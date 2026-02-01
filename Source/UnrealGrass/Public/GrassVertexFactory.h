// GrassVertexFactory.h
// 自定义 Vertex Factory，支持从 StructuredBuffer 读取每个实例的位置偏移

#pragma once

#include "CoreMinimal.h"
#include "LocalVertexFactory.h"
#include "ShaderParameters.h"
#include "RenderResource.h"

/**
 * 草地 Vertex Factory
 * 扩展 LocalVertexFactory，添加实例位置缓冲区支持
 */
class FGrassVertexFactory : public FLocalVertexFactory
{
    DECLARE_VERTEX_FACTORY_TYPE(FGrassVertexFactory);

public:
    FGrassVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName);

    // 设置实例位置缓冲区 SRV 和实例数量
    void SetInstancePositionSRV(FRHIShaderResourceView* InSRV, uint32 InNumInstances);
    
    // 设置草叶数据缓冲区 SRV
    void SetGrassDataSRV(FRHIShaderResourceView* InData0SRV, FRHIShaderResourceView* InData1SRV, FRHIShaderResourceView* InData2SRV);

    FRHIShaderResourceView* GetInstancePositionSRV() const { return InstancePositionSRV; }
    FRHIShaderResourceView* GetGrassData0SRV() const { return GrassData0SRV; }
    FRHIShaderResourceView* GetGrassData1SRV() const { return GrassData1SRV; }
    FRHIShaderResourceView* GetGrassData2SRV() const { return GrassData2SRV; }
    uint32 GetNumInstances() const { return NumInstances; }

    static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
    static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

private:
    FRHIShaderResourceView* InstancePositionSRV = nullptr;
    FRHIShaderResourceView* GrassData0SRV = nullptr;  // Height, Width, Tilt, Bend
    FRHIShaderResourceView* GrassData1SRV = nullptr;  // TaperAmount, FacingDir.x, FacingDir.y, P1Offset
    FRHIShaderResourceView* GrassData2SRV = nullptr;  // P2Offset
    uint32 NumInstances = 0;
};

/**
 * Shader 参数结构 - 负责将 SRV 绑定到 Shader
 */
class FGrassVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
    DECLARE_TYPE_LAYOUT(FGrassVertexFactoryShaderParameters, NonVirtual);

public:
    void Bind(const FShaderParameterMap& ParameterMap);
    
    void GetElementShaderBindings(
        const class FSceneInterface* Scene,
        const class FSceneView* View,
        const class FMeshMaterialShader* Shader,
        const EVertexInputStreamType InputStreamType,
        ERHIFeatureLevel::Type FeatureLevel,
        const class FVertexFactory* VertexFactory,
        const struct FMeshBatchElement& BatchElement,
        class FMeshDrawSingleShaderBindings& ShaderBindings,
        FVertexInputStreamArray& VertexStreams
    ) const;

    LAYOUT_FIELD(FShaderResourceParameter, InstancePositionBuffer);
    LAYOUT_FIELD(FShaderResourceParameter, GrassData0Buffer);  // Height, Width, Tilt, Bend
    LAYOUT_FIELD(FShaderResourceParameter, GrassData1Buffer);  // TaperAmount, FacingDir.x, FacingDir.y, P1Offset
    LAYOUT_FIELD(FShaderResourceParameter, GrassData2Buffer);  // P2Offset
};