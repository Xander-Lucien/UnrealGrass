// GrassVertexFactory.cpp
// 自定义 VertexFactory 实现

#include "GrassVertexFactory.h"
#include "MeshMaterialShader.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"
#include "PipelineStateCache.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FGrassVertexFactoryParameters, "GrassVF");

// ============================================================================
// Shader Parameters
// ============================================================================
class FGrassVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
    DECLARE_TYPE_LAYOUT(FGrassVertexFactoryShaderParameters, NonVirtual);

public:
    void Bind(const FShaderParameterMap& ParameterMap)
    {
        InstancePositionBufferParam.Bind(ParameterMap, TEXT("InstancePositionBuffer"));
    }

    void GetElementShaderBindings(
        const class FSceneInterface* Scene,
        const class FSceneView* View,
        const class FMeshMaterialShader* Shader,
        const EVertexInputStreamType InputStreamType,
        ERHIFeatureLevel::Type FeatureLevel,
        const class FVertexFactory* VertexFactory,
        const struct FMeshBatchElement& BatchElement,
        class FMeshDrawSingleShaderBindings& ShaderBindings,
        FVertexInputStreamArray& VertexStreams) const
    {
        const FGrassVertexFactory* GrassVF = static_cast<const FGrassVertexFactory*>(VertexFactory);
        
        if (InstancePositionBufferParam.IsBound() && GrassVF->GetInstancePositionSRV())
        {
            ShaderBindings.Add(InstancePositionBufferParam, GrassVF->GetInstancePositionSRV());
        }
    }

private:
    LAYOUT_FIELD(FShaderResourceParameter, InstancePositionBufferParam);
};

IMPLEMENT_TYPE_LAYOUT(FGrassVertexFactoryShaderParameters);

// ============================================================================
// FGrassVertexFactory 实现
// ============================================================================
IMPLEMENT_VERTEX_FACTORY_TYPE(FGrassVertexFactory, "/Plugin/UnrealGrass/Private/GrassVertexFactory.ush",
    EVertexFactoryFlags::UsedWithMaterials |
    EVertexFactoryFlags::SupportsDynamicLighting |
    EVertexFactoryFlags::SupportsPositionOnly
);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGrassVertexFactory, SF_Vertex, FGrassVertexFactoryShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FGrassVertexFactory, SF_Pixel, FGrassVertexFactoryShaderParameters);

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
    return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
           FLocalVertexFactory::ShouldCompilePermutation(Parameters);
}

void FGrassVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
    FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    OutEnvironment.SetDefine(TEXT("USE_GRASS_INSTANCING"), 1);
}
