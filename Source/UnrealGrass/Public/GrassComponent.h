// GrassComponent.h
// 最简单的 GPU 草地：Compute Shader 生成位置 + DrawIndexedPrimitive 绘制

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GrassComponent.generated.h"

UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class UNREALGRASS_API UGrassComponent : public UPrimitiveComponent
{
    GENERATED_BODY()

public:
    UGrassComponent();

    UPROPERTY(EditAnywhere, Category = "Grass", meta = (ClampMin = "2", ClampMax = "100"))
    int32 GridSize = 10;

    UPROPERTY(EditAnywhere, Category = "Grass")
    float Spacing = 100.0f;

    UPROPERTY(EditAnywhere, Category = "Grass")
    UMaterialInterface* GrassMaterial;

    UFUNCTION(CallInEditor, Category = "Grass")
    void GenerateGrass();

    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    
    virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

    // GPU Buffer 数据
    FBufferRHIRef PositionBuffer;
    FShaderResourceViewRHIRef PositionBufferSRV;
    int32 InstanceCount = 0;
};
