#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "GrassMesh.generated.h"

/**
 * 草叶网格数据结构
 * 包含顶点、三角形、UV等网格数据
 */
USTRUCT(BlueprintType)
struct FGrassBladeMeshData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	TArray<FVector> Vertices;

	UPROPERTY(BlueprintReadOnly)
	TArray<int32> Triangles;

	UPROPERTY(BlueprintReadOnly)
	TArray<FVector> Normals;

	UPROPERTY(BlueprintReadOnly)
	TArray<FVector2D> UVs;

	UPROPERTY(BlueprintReadOnly)
	TArray<FColor> VertexColors;
};

/**
 * 草叶网格生成器Actor
 * 用于生成和预览草叶网格
 */
UCLASS()
class UNREALGRASS_API AGrassMesh : public AActor
{
	GENERATED_BODY()
	
public:	
	AGrassMesh();

	// 程序化网格组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grass")
	UProceduralMeshComponent* ProceduralMesh;

	// 草叶段数（LOD级别控制）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grass|Settings", meta = (ClampMin = "2", ClampMax = "10"))
	int32 BladeSegments = 7;

	// 草叶宽度（底部）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grass|Settings", meta = (ClampMin = "0.01"))
	float BladeWidth = 0.07f;

	// 草叶高度
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grass|Settings", meta = (ClampMin = "0.01"))
	float BladeHeight = 0.7f;

	// 是否在编辑器中自动更新预览
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grass|Settings")
	bool bAutoUpdate = true;

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

public:	
	/**
	 * 创建高精度LOD网格数据
	 * 模拟Unity教程中的CreateHighLODMesh函数
	 * @return 草叶网格数据
	 */
	UFUNCTION(BlueprintCallable, Category = "Grass")
	static FGrassBladeMeshData CreateHighLODMesh();

	/**
	 * 创建可配置的草叶网格数据
	 * @param Segments 草叶段数
	 * @param Width 草叶底部宽度
	 * @param Height 草叶高度
	 * @return 草叶网格数据
	 */
	UFUNCTION(BlueprintCallable, Category = "Grass")
	static FGrassBladeMeshData CreateGrassBladeMesh(int32 Segments, float Width, float Height);

	/**
	 * 更新程序化网格显示
	 */
	UFUNCTION(BlueprintCallable, Category = "Grass")
	void UpdateMesh();

	/**
	 * 将网格数据导出为静态网格资产（编辑器功能）
	 */
	UFUNCTION(BlueprintCallable, Category = "Grass", meta = (CallInEditor = "true"))
	void ExportToStaticMesh();
};
