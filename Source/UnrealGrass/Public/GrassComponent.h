// GrassComponent.h
// GPU 草地：Compute Shader 生成位置 + GPU Frustum Culling + Indirect Draw

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GrassComponent.generated.h"

class UStaticMesh;

UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class UNREALGRASS_API UGrassComponent : public UPrimitiveComponent
{
    GENERATED_BODY()

public:
    UGrassComponent();

    UPROPERTY(EditAnywhere, Category = "Grass", meta = (ClampMin = "2", ClampMax = "1000"))
    int32 GridSize = 10;

    UPROPERTY(EditAnywhere, Category = "Grass")
    float Spacing = 100.0f;

    UPROPERTY(EditAnywhere, Category = "Grass", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float JitterStrength = 0.5f;

    /** 是否使用 Indirect Draw（GPU 驱动的绘制调用）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Rendering")
    bool bUseIndirectDraw = true;

    /** 是否启用 GPU Frustum Culling */
    UPROPERTY(EditAnywhere, Category = "Grass|Culling")
    bool bEnableFrustumCulling = true;

    /** 是否启用距离剔除 */
    UPROPERTY(EditAnywhere, Category = "Grass|Culling")
    bool bEnableDistanceCulling = true;

    /** 草叶可见的最大距离（距离剔除）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Culling", meta = (ClampMin = "100.0", EditCondition = "bEnableDistanceCulling"))
    float MaxVisibleDistance = 10000.0f;

    /** 草叶的包围盒半径（用于扩展剔除检测）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Culling", meta = (ClampMin = "1.0"))
    float GrassBoundingRadius = 50.0f;

    /** 草叶模型，如果为空则使用默认三角形 */
    UPROPERTY(EditAnywhere, Category = "Grass")
    UStaticMesh* GrassMesh;

    UPROPERTY(EditAnywhere, Category = "Grass")
    UMaterialInterface* GrassMaterial;

    UFUNCTION(CallInEditor, Category = "Grass")
    void GenerateGrass();

    // 生命周期函数
    virtual void BeginPlay() override;
    virtual void OnRegister() override;

    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    
    virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

    // ======== GPU Buffer 数据 ========
    
    // 所有实例的位置 Buffer（输入）
    FBufferRHIRef PositionBuffer;
    FShaderResourceViewRHIRef PositionBufferSRV;
    int32 InstanceCount = 0;

    // 可见实例的位置 Buffer（剔除后输出）
    FBufferRHIRef VisiblePositionBuffer;
    FShaderResourceViewRHIRef VisiblePositionBufferSRV;
    FUnorderedAccessViewRHIRef VisiblePositionBufferUAV;

    // Indirect Draw 参数 Buffer
    // 存储 DrawIndexedInstancedIndirect 的参数:
    // [0] IndexCountPerInstance
    // [1] InstanceCount
    // [2] StartIndexLocation
    // [3] BaseVertexLocation
    // [4] StartInstanceLocation
    FBufferRHIRef IndirectArgsBuffer;
    FUnorderedAccessViewRHIRef IndirectArgsBufferUAV;

    // 用于传递给 SceneProxy 的 Mesh 信息
    int32 NumIndices = 0;
    int32 NumVertices = 0;
};

