// GrassComponent.h
// GPU 草地：Compute Shader 生成位置 + GPU Frustum Culling + Indirect Draw

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GrassComponent.generated.h"

class UStaticMesh;
class ALandscapeProxy;
class ULandscapeComponent;

// ============================================================================
// 草丛簇实例数据结构体 (GPU Buffer 格式)
// 每个簇的属性，由 Compute Shader 在初始化时生成
// ============================================================================
struct FGrassClumpData
{
    FVector2f Centre;        // 簇中心位置 (UV空间, 0-1)
    FVector2f Direction;     // 簇内草叶的统一朝向 (已归一化)
    float HeightScale;       // 这个簇的高度缩放因子
    float WidthScale;        // 这个簇的宽度缩放因子
    float WindPhase;         // 风动画的相位偏移 (用于让不同簇的风效果有差异)
    float Padding;           // 对齐到 32 字节 (float4 * 2)
};

// ============================================================================
// 草丛簇类型参数结构体 (每种簇类型的完整参数配置)
// 用户可以为每种簇类型单独设置草叶形态参数
// ============================================================================
USTRUCT(BlueprintType)
struct FClumpTypeParameters
{
    GENERATED_BODY()

    // ======== 聚集行为参数 ========
    
    /** 控制草叶朝向簇中心的程度，值越高草叶越聚集在簇中心 (0=均匀分布, 1=完全聚集) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PullToCentre = 0.3f;

    /** 控制簇内草叶朝向的一致性，值越高簇内草叶朝向越统一 (0=随机朝向, 1=完全统一) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float PointInSameDirection = 0.5f;

    // ======== 草叶形态参数 ========
    
    /** 草叶的基础高度 (厘米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "1.0", ClampMax = "200.0"))
    float BaseHeight = 50.0f;

    /** 草叶高度的随机变化范围 (厘米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "100.0"))
    float HeightRandom = 20.0f;

    /** 草叶的基础宽度 (厘米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.1", ClampMax = "20.0"))
    float BaseWidth = 5.0f;

    /** 草叶宽度的随机变化范围 (厘米) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "10.0"))
    float WidthRandom = 2.0f;

    /** 草叶的基础倾斜度，控制顶端偏离垂直方向的程度 (0=直立, 1=完全倾斜) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float BaseTilt = 0.2f;

    /** 草叶倾斜度的随机变化范围 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float TiltRandom = 0.1f;

    /** 草叶的基础弯曲度，控制整体曲线形状 (0=直线, 1=强弯曲) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float BaseBend = 0.3f;

    /** 草叶弯曲度的随机变化范围 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "0.5"))
    float BendRandom = 0.15f;
};

// ============================================================================
// 全局草叶渲染参数 (所有簇类型共享的参数)
// ============================================================================
USTRUCT(BlueprintType)
struct FGrassRenderParameters
{
    GENERATED_BODY()

    /** 草叶尖端收缩程度 (0=不收缩保持宽度, 1=完全收缩成尖点) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TaperAmount = 0.8f;

    /** 草叶法线弯曲程度，让草叶边缘的法线向外弯曲，产生更柔和的光照效果 (0=平面法线, 1=完全弯曲) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float CurvedNormalAmount = 0.5f;

    /* 视角依赖旋转强度 
     *  当从侧面观看草叶时，草叶会轻微旋转朝向相机，让草地看起来更饱满*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blade", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ViewRotationAmount = 0.3f;
};

// ============================================================================
// 最大簇类型数量常量
// ============================================================================
constexpr int32 MAX_CLUMP_TYPES = 5;

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
    float MaxVisibleDistance = 5000.0f;

    /** 草叶的包围盒半径（用于扩展剔除检测）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Culling", meta = (ClampMin = "1.0"))
    float GrassBoundingRadius = 50.0f;

    /** 是否启用 Hi-Z 遮挡剔除（需要 GPU Culling 开启）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Culling")
    bool bEnableOcclusionCulling = true;

    // ======== LOD 设置 ========
    
    /** 是否启用 LOD 系统 */
    UPROPERTY(EditAnywhere, Category = "Grass|LOD")
    bool bEnableLOD = true;

    /** LOD 0 到 LOD 1 的切换距离（厘米）*/
    UPROPERTY(EditAnywhere, Category = "Grass|LOD", meta = (ClampMin = "100.0", EditCondition = "bEnableLOD"))
    float LOD0Distance = 1000.0f;

    // ======== 全局渲染参数 ========
    
    /** 全局草叶渲染参数（所有簇类型共享）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Rendering")
    FGrassRenderParameters RenderParameters;

    // ======== 地形高度图设置 ========
    
    /** 是否启用 Landscape Heightmap，自动从所在的 Landscape Component 获取高度数据 */
    UPROPERTY(EditAnywhere, Category = "Grass|Heightmap")
    bool bUseLandscapeHeightmap = false;

    // ======== 风场扰动噪声设置 ========

    /** 风场扰动噪声纹理 */
    UPROPERTY(EditAnywhere, Category = "Grass|Wind")
    UTexture2D* WindNoiseTexture = nullptr;

    /** 噪声纹理缩放（控制噪声在世界中的频率）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Wind")
    FVector2D WindNoiseScale = FVector2D(0.001f, 0.001f);

    /** 噪声对风向/弯曲的影响强度 */
    UPROPERTY(EditAnywhere, Category = "Grass|Wind", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float WindNoiseStrength = 0.5f;

    /** 噪声滚动速度 */
    UPROPERTY(EditAnywhere, Category = "Grass|Wind", meta = (ClampMin = "0.0"))
    float WindNoiseSpeed = 0.1f;

    // ======== 正弦波风参数 (《对马岛之魂》风格) ========

    /** 风波动速度（正弦波频率）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Wind", meta = (ClampMin = "0.0", ClampMax = "10.0"))
    float WindWaveSpeed = 2.0f;

    /** 风波动振幅（草叶摆动幅度）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Wind", meta = (ClampMin = "0.0", ClampMax = "5.0"))
    float WindWaveAmplitude = 1.0f;

    /** 正弦偏移范围（让每个草叶有不同的相位，产生波浪效果）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Wind", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float WindSinOffsetRange = 0.5f;

    /** 尖端前推量（让草叶顶端在风中向前倾斜）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Wind", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float WindPushTipForward = 0.0f;

    // ======== 局部风方向旋转 (对马岛之魂风格) ========

    /** 局部风方向旋转强度
     *  Noise 纹理被映射为局部风方向角度，投影到草叶侧面方向上旋转草叶朝向
     *  这使得每棵草在风中的倾倒方向不同，而不是所有草都沿同一个风向倒
     *  0 = 无旋转, 1 = 最大旋转 */
    UPROPERTY(EditAnywhere, Category = "Grass|Wind", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float LocalWindRotateAmount = 0.5f;

    // ======== 丛簇类型参数 ========
    
    /** 草丛簇类型参数数组，每种类型可以有不同的草叶形态 (最多5种) */
    UPROPERTY(EditAnywhere, Category = "Grass|Clump Types", meta = (TitleProperty = "BaseHeight"))
    TArray<FClumpTypeParameters> ClumpTypes;

    /** 丛簇数量 */
    UPROPERTY(EditAnywhere, Category = "Grass|Clumping", meta = (ClampMin = "1", ClampMax = "256"))
    int32 NumClumps = 50;

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
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

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

    // LOD 1 的 Indirect Draw 参数 Buffer (用于简化版草叶)
    FBufferRHIRef IndirectArgsBufferLOD1;
    FUnorderedAccessViewRHIRef IndirectArgsBufferLOD1UAV;

    // ======== LOD 1 独立的 Visible Buffers ========
    // LOD 1 使用独立的 buffer，避免与 LOD 0 数据冲突
    FBufferRHIRef VisiblePositionBufferLOD1;
    FShaderResourceViewRHIRef VisiblePositionBufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisiblePositionBufferLOD1UAV;
    
    FBufferRHIRef VisibleGrassData0BufferLOD1;
    FShaderResourceViewRHIRef VisibleGrassData0BufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData0BufferLOD1UAV;
    
    FBufferRHIRef VisibleGrassData1BufferLOD1;
    FShaderResourceViewRHIRef VisibleGrassData1BufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData1BufferLOD1UAV;
    
    FBufferRHIRef VisibleGrassData2BufferLOD1;
    FShaderResourceViewRHIRef VisibleGrassData2BufferLOD1SRV;
    FUnorderedAccessViewRHIRef VisibleGrassData2BufferLOD1UAV;

    // 用于传递给 SceneProxy 的 Mesh 信息
    int32 NumIndices = 0;
    int32 NumVertices = 0;

    // ======== Clump Buffer 数据 ========
    // 簇数据 Buffer，存储每个簇的中心位置、朝向、缩放等属性
    FBufferRHIRef ClumpBuffer;            // ClumpData0: Centre.xy, Direction.xy
    FShaderResourceViewRHIRef ClumpBufferSRV;
    FBufferRHIRef ClumpData1Buffer;       // ClumpData1: HeightScale, WidthScale, WindPhase, Padding
    FShaderResourceViewRHIRef ClumpData1BufferSRV;

    // ======== Clump Type 参数 Buffer ========
    // 存储每种簇类型的参数，供 GPU 读取
    FBufferRHIRef ClumpTypeParamsBuffer;
    FShaderResourceViewRHIRef ClumpTypeParamsBufferSRV;

    // ======== Landscape 高度图 Texture 数据 ========
    // 从 Landscape Component 自动获取的高度图 SRV
    FShaderResourceViewRHIRef HeightmapTextureSRV;

    // ======== 辅助方法 ========
    
    /** 获取有效的簇类型数量（确保至少有1个，最多MAX_CLUMP_TYPES个）*/
    int32 GetNumClumpTypes() const { return FMath::Clamp(ClumpTypes.Num(), 1, MAX_CLUMP_TYPES); }
    
    /** 确保 ClumpTypes 数组有效（至少有一个默认类型）*/
    void EnsureValidClumpTypes();

#if WITH_EDITOR
    /** 编辑器中属性变化时自动重新生成草地 */
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

#if WITH_EDITORONLY_DATA
    /** 是否启用实时预览（属性修改后自动更新）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Editor")
    bool bEnableRealtimePreview = true;
#endif
};

