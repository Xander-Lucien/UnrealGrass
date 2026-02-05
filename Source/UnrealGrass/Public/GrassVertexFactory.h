// GrassVertexFactory.h
// 自定义 Vertex Factory，支持从 StructuredBuffer 读取每个实例的位置偏移

#pragma once

#include "CoreMinimal.h"
#include "LocalVertexFactory.h"
#include "ShaderParameters.h"
#include "RenderResource.h"
#include "RHIResources.h"

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

    // 设置 LOD 级别 (0 = 高质量, 1 = 简化版)
    void SetLODLevel(uint32 InLODLevel) { LODLevel = InLODLevel; }
    uint32 GetLODLevel() const { return LODLevel; }

    // 设置弯曲法线程度
    void SetCurvedNormalAmount(float InAmount) { CurvedNormalAmount = InAmount; }
    float GetCurvedNormalAmount() const { return CurvedNormalAmount; }

    // 设置视角依赖旋转强度 (对马岛之魂风格)
    // 当从侧面观看草叶时，草叶会轻微旋转朝向相机，让草地看起来更饱满
    void SetViewRotationAmount(float InAmount) { ViewRotationAmount = InAmount; }
    float GetViewRotationAmount() const { return ViewRotationAmount; }

    // 设置风场扰动噪声参数
    void SetWindNoiseParameters(FTextureRHIRef InTexture, const FVector2f& InScale, float InStrength, float InSpeed)
    {
        WindNoiseTexture = InTexture;
        WindNoiseScale = InScale;
        WindNoiseStrength = InStrength;
        WindNoiseSpeed = InSpeed;
    }
    FTextureRHIRef GetWindNoiseTexture() const { return WindNoiseTexture; }
    FVector2f GetWindNoiseScale() const { return WindNoiseScale; }
    float GetWindNoiseStrength() const { return WindNoiseStrength; }
    float GetWindNoiseSpeed() const { return WindNoiseSpeed; }

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
    uint32 LODLevel = 0;  // LOD 级别: 0 = LOD0 高质量, 1 = LOD1 简化版
    float CurvedNormalAmount = 0.5f;  // 弯曲法线程度
    float ViewRotationAmount = 0.3f;  // 视角依赖旋转强度 (0 = 无, 1 = 最大)
    FTextureRHIRef WindNoiseTexture;
    FVector2f WindNoiseScale = FVector2f(0.001f, 0.001f);
    float WindNoiseStrength = 0.0f;
    float WindNoiseSpeed = 0.0f;
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
    LAYOUT_FIELD(FShaderParameter, GrassLODLevel);  // LOD 级别参数
    LAYOUT_FIELD(FShaderParameter, GrassCurvedNormalAmount);  // 弯曲法线程度参数
    LAYOUT_FIELD(FShaderParameter, GrassViewRotationAmount);  // 视角依赖旋转强度参数
    LAYOUT_FIELD(FShaderParameter, GrassWindDirection);
    LAYOUT_FIELD(FShaderParameter, GrassWindStrength);
    LAYOUT_FIELD(FShaderResourceParameter, GrassWindNoiseTexture);
    LAYOUT_FIELD(FShaderResourceParameter, GrassWindNoiseSampler);
    LAYOUT_FIELD(FShaderParameter, GrassWindNoiseScale);
    LAYOUT_FIELD(FShaderParameter, GrassWindNoiseStrength);
    LAYOUT_FIELD(FShaderParameter, GrassWindNoiseSpeed);
};