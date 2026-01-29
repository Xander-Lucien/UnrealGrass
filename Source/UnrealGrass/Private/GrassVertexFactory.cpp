// GrassVertexFactory.cpp
// 自定义 Vertex Factory 实现 - 支持从 StructuredBuffer 读取实例位置偏移

#include "GrassVertexFactory.h"
#include "MeshMaterialShader.h"
#include "MeshDrawShaderBindings.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Vertex Factory 类型注册
// ============================================================================

IMPLEMENT_VERTEX_FACTORY_TYPE(FGrassVertexFactory, "/Plugin/UnrealGrass/Private/GrassVertexFactory.ush",
    EVertexFactoryFlags::UsedWithMaterials |
    EVertexFactoryFlags::SupportsDynamicLighting |
    EVertexFactoryFlags::SupportsPositionOnly
);

// ============================================================================
// Vertex Factory 实现
// ============================================================================

FGrassVertexFactory::FGrassVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName)
    : FLocalVertexFactory(InFeatureLevel, InDebugName)
{
}

void FGrassVertexFactory::SetInstancePositionSRV(FRHIShaderResourceView* InSRV, uint32 InNumInstances)
{
    InstancePositionSRV = InSRV;
    NumInstances = InNumInstances;
}

bool FGrassVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
    // 只为 SM5 及以上编译
    return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGrassVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
    // 不调用父类的 ModifyCompilationEnvironment，因为我们使用完全自定义的 shader
    
    // 启用草地实例化宏
    OutEnvironment.SetDefine(TEXT("USE_GRASS_INSTANCING"), 1);
}

// ============================================================================
// Shader Parameters 实现
// ============================================================================

IMPLEMENT_TYPE_LAYOUT(FGrassVertexFactoryShaderParameters);

void FGrassVertexFactoryShaderParameters::Bind(const FShaderParameterMap& ParameterMap)
{
    // 绑定 Shader 中的 GrassInstancePositions 参数
    InstancePositionBuffer.Bind(ParameterMap, TEXT("GrassInstancePositions"));
}

void FGrassVertexFactoryShaderParameters::GetElementShaderBindings(
    const FSceneInterface* Scene,
    const FSceneView* View,
    const FMeshMaterialShader* Shader,
    const EVertexInputStreamType InputStreamType,
    ERHIFeatureLevel::Type FeatureLevel,
    const FVertexFactory* VertexFactory,
    const FMeshBatchElement& BatchElement,
    FMeshDrawSingleShaderBindings& ShaderBindings,
    FVertexInputStreamArray& VertexStreams) const
{
    const FGrassVertexFactory* GrassVF = static_cast<const FGrassVertexFactory*>(VertexFactory);

    // 将实例位置缓冲区 SRV 绑定到 Shader
    if (InstancePositionBuffer.IsBound())
    {
        FRHIShaderResourceView* SRV = GrassVF->GetInstancePositionSRV();
        if (SRV)
        {
            ShaderBindings.Add(InstancePositionBuffer, SRV);
        }
    }
}

// 注册参数绑定 - 顶点着色器和像素着色器都需要
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGrassVertexFactory, SF_Vertex, FGrassVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGrassVertexFactory, SF_Pixel, FGrassVertexFactoryShaderParameters);