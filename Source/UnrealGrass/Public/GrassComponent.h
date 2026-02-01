// GrassComponent.h
// GPU 草地：Compute Shader 生成位置 + GPU Frustum Culling + Indirect Draw

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GrassComponent.generated.h"

class UStaticMesh;

// ============================================================================
// 草叶实例数据结构体 (GPU Buffer 格式)
// 每个草叶实例的属性，用于 Vertex Shader 中的贝塞尔曲线变形
// ============================================================================
struct FGrassInstanceData
{
    FVector3f Position;      // 草叶根部位置
    float Height;            // 草叶高度
    float Width;             // 草叶宽度
    float Tilt;              // 倾斜角度 (弧度)
    float Bend;              // 弯曲程度
    float TaperAmount;       // 尖端收缩程度 (0=不收缩, 1=完全收缩成点)
    FVector2f FacingDirection; // 草叶朝向 (XY平面上的方向)
    float P1Offset;          // 贝塞尔控制点1偏移
    float P2Offset;          // 贝塞尔控制点2偏移
};

// ============================================================================
// 草丛簇参数结构体
// ============================================================================
USTRUCT(BlueprintType)
struct FClumpParameters
{
    GENERATED_BODY()

    /** 控制草叶朝向族中心的程度，值越高草叶越集中在族中心 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PullToCentre = 0.3f;

    /** 控制族内草叶朝向的一致性，值越高族内草叶朝向越统一 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PointInSameDirection = 0.5f;

    /** 草叶的基础高度 (厘米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "1.0"))
    float BaseHeight = 50.0f;

    /** 草叶高度的随机变化范围 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0"))
    float HeightRandom = 20.0f;

    /** 草叶的基础宽度 (厘米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.1"))
    float BaseWidth = 5.0f;

    /** 草叶宽度的随机变化范围 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0"))
    float WidthRandom = 2.0f;

    /** 草叶的基础倾斜度，控制顶端偏离垂直方向的程度 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float BaseTilt = 0.2f;

    /** 草叶倾斜度的随机变化范围 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float TiltRandom = 0.1f;

    /** 草叶的基础弯曲度，控制整体曲线形状 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float BaseBend = 0.3f;

    /** 草叶弯曲度的随机变化范围 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float BendRandom = 0.15f;

    /** 草叶尖端收缩程度 (0=不收缩, 1=完全收缩) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TaperAmount = 0.8f;
};

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

    // ======== 丛簇参数 ========
    
    /** 草丛簇参数 */
    UPROPERTY(EditAnywhere, Category = "Grass|Clumping")
    FClumpParameters ClumpParameters;

    /** 丛簇数量 */
    UPROPERTY(EditAnywhere, Category = "Grass|Clumping", meta = (ClampMin = "1", ClampMax = "100"))
    int32 NumClumps = 50;

    /** 丛簇类型数量（用于颜色/属性变化）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Clumping", meta = (ClampMin = "1", ClampMax = "40"))
    int32 NumClumpTypes = 5;

    /** 草叶模型，如果为空则使用默认高质量草叶 */
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

    // 草叶实例数据 Buffer（包含 Height, Width, Tilt, Bend 等属性）
    FBufferRHIRef GrassDataBuffer;      // GrassData0: Height, Width, Tilt, Bend
    FShaderResourceViewRHIRef GrassDataBufferSRV;
    FBufferRHIRef GrassData1Buffer;     // GrassData1: TaperAmount, FacingDir.x, FacingDir.y, P1Offset
    FShaderResourceViewRHIRef GrassData1BufferSRV;
    FBufferRHIRef GrassData2Buffer;     // GrassData2: P2Offset
    FShaderResourceViewRHIRef GrassData2BufferSRV;

    // 可见实例的位置 Buffer（剔除后输出）
    FBufferRHIRef VisiblePositionBuffer;
    FShaderResourceViewRHIRef VisiblePositionBufferSRV;
    FUnorderedAccessViewRHIRef VisiblePositionBufferUAV;

    // 可见实例的数据 Buffer（剔除后输出）
    FBufferRHIRef VisibleGrassData0Buffer;
    FShaderResourceViewRHIRef VisibleGrassData0BufferSRV;
    FUnorderedAccessViewRHIRef VisibleGrassData0BufferUAV;
    
    FBufferRHIRef VisibleGrassData1Buffer;
    FShaderResourceViewRHIRef VisibleGrassData1BufferSRV;
    FUnorderedAccessViewRHIRef VisibleGrassData1BufferUAV;
    
    FBufferRHIRef VisibleGrassData2Buffer;
    FShaderResourceViewRHIRef VisibleGrassData2BufferSRV;
    FUnorderedAccessViewRHIRef VisibleGrassData2BufferUAV;

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

