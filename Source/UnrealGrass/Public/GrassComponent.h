// GrassComponent.h
// GPU 草地：Compute Shader 生成位置 + GPU Frustum Culling + Indirect Draw

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GrassComponent.generated.h"

class UStaticMesh;

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
// 草叶实例数据结构体 (文档用途, 实际数据分散存储在多个 Buffer 中)
// 每个草叶实例的属性，用于 Vertex Shader 中的贝塞尔曲线变形
// ============================================================================
/*
* struct FGrassInstanceData
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
*/
// ============================================================================

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

    /** 视角依赖旋转强度 (对马岛之魂风格)
     *  当从侧面观看草叶时，草叶会轻微旋转朝向相机，让草地看起来更饱满
     *  (0=无旋转, 1=完全旋转朝向相机) */
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
    
    /** 是否启用高度图，让草叶适应地形 */
    UPROPERTY(EditAnywhere, Category = "Grass|Heightmap")
    bool bUseHeightmap = false;

    /** 高度图纹理（R通道存储归一化高度值 0-1）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Heightmap", meta = (EditCondition = "bUseHeightmap"))
    UTexture2D* HeightmapTexture = nullptr;

    /** 高度图覆盖的世界空间大小 (X, Y) 厘米 */
    UPROPERTY(EditAnywhere, Category = "Grass|Heightmap", meta = (EditCondition = "bUseHeightmap"))
    FVector2D HeightmapWorldSize = FVector2D(10000.0, 10000.0);

    /** 高度图世界空间偏移（用于对齐地形）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Heightmap", meta = (EditCondition = "bUseHeightmap"))
    FVector2D HeightmapWorldOffset = FVector2D(0.0, 0.0);

    /** 高度图缩放因子（高度图值乘以此值得到实际高度）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Heightmap", meta = (EditCondition = "bUseHeightmap"))
    float HeightmapScale = 1000.0f;

    /** 高度偏移值（添加到最终高度）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Heightmap", meta = (EditCondition = "bUseHeightmap"))
    float HeightmapOffset = 0.0f;

    // ======== 丛簇类型参数 ========
    
    /** 草丛簇类型参数数组，每种类型可以有不同的草叶形态 (最多5种) */
    UPROPERTY(EditAnywhere, Category = "Grass|Clump Types", meta = (TitleProperty = "BaseHeight"))
    TArray<FClumpTypeParameters> ClumpTypes;

    /** 丛簇数量 */
    UPROPERTY(EditAnywhere, Category = "Grass|Clumping", meta = (ClampMin = "1", ClampMax = "256"))
    int32 NumClumps = 50;

    /** Voronoi Texture 分辨率（越高精度越高，但内存占用也越大）*/
    UPROPERTY(EditAnywhere, Category = "Grass|Clumping", meta = (ClampMin = "64", ClampMax = "1024"))
    int32 VoronoiTextureSize = 256;

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

    // ======== Voronoi Texture 数据 ========
    // 预计算的 Voronoi 查找纹理，用于 O(1) 复杂度获取最近 Clump
    // R = ClumpIndex (归一化), G = CentreX, B = CentreY, A = Distance
    FTextureRHIRef VoronoiTexture;
    FShaderResourceViewRHIRef VoronoiTextureSRV;
    FUnorderedAccessViewRHIRef VoronoiTextureUAV;

    // ======== 高度图 Texture 数据 ========
    // 用于地形高度采样的纹理 SRV（从 UTexture2D 获取）
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