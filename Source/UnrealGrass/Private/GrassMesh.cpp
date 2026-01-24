// GrassMesh.cpp
// 草叶网格生成器实现 - 参考《对马岛之魂》草地渲染教程

#include "GrassMesh.h"

AGrassMesh::AGrassMesh()
{
	PrimaryActorTick.bCanEverTick = false;

	// 创建程序化网格组件
	ProceduralMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProceduralMesh"));
	RootComponent = ProceduralMesh;

	// 设置默认材质为双面渲染
	ProceduralMesh->bUseAsyncCooking = true;
}

void AGrassMesh::BeginPlay()
{
	Super::BeginPlay();
	UpdateMesh();
}

void AGrassMesh::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	
	if (bAutoUpdate)
	{
		UpdateMesh();
	}
}

FGrassBladeMeshData AGrassMesh::CreateHighLODMesh()
{
	// 使用Unity教程中的原始顶点数据
	// 这是一个7段的草叶网格，从底部到顶部逐渐变窄
	
	FGrassBladeMeshData MeshData;

	// 顶点数据 - 直接从Unity教程复制并转换坐标系
	// Unity使用Y向上，UE使用Z向上，所以需要交换Y和Z
	// Unity: (X, Y, Z) -> UE: (X, Z, Y) * 100（厘米单位转换）
	const float Scale = 100.0f; // Unity米 -> UE厘米
	
	MeshData.Vertices = {
		// 第0段（底部）
		FVector(0.0f, 0.03445f * Scale, 0.15599f * Scale),   // 左
		FVector(0.0f, -0.03444f * Scale, 0.0f),               // 右
		FVector(0.0f, 0.03444f * Scale, 0.0f),                // 左底
		// 第1段
		FVector(0.0f, -0.03445f * Scale, 0.15599f * Scale),  // 右
		FVector(0.0f, -0.03193f * Scale, 0.27249f * Scale),  // 右
		FVector(0.0f, 0.03193f * Scale, 0.27249f * Scale),   // 左
		// 第2段
		FVector(0.0f, -0.02942f * Scale, 0.38111f * Scale),
		FVector(0.0f, 0.02942f * Scale, 0.38111f * Scale),
		// 第3段
		FVector(0.0f, -0.02620f * Scale, 0.47325f * Scale),
		FVector(0.0f, 0.02620f * Scale, 0.47325f * Scale),
		// 第4段
		FVector(0.0f, -0.02338f * Scale, 0.55531f * Scale),
		FVector(0.0f, 0.02338f * Scale, 0.55531f * Scale),
		// 第5段
		FVector(0.0f, -0.01728f * Scale, 0.63064f * Scale),
		FVector(0.0f, 0.01728f * Scale, 0.63064f * Scale),
		// 第6段（顶部）
		FVector(0.0f, 0.0f * Scale, 0.70819f * Scale),       // 顶点
	};

	// 重新组织顶点为正确的草叶形状（左右交替）
	MeshData.Vertices.Empty();
	
	// 按照教程的精确顶点数据重建（从图片中提取）
	// 格式: {高度Y, 半宽X}
	// new Vector3(0.00000f, Y, X) 中 Y是高度，X是半宽
	TArray<TPair<float, float>> SegmentData = {
		{0.0f,       0.03444f},      // 0 - 底部 (0.00000f, 0.03444f)
		{0.15599f,   0.03445f},      // 1 (0.15599f, 0.03445f) / (0.15599f, -0.03444f)
		{0.27249f,   0.03193f},      // 2 (0.27249f, -0.03193f) / (0.27249f, 0.03193f)
		{0.38111f,   0.02942f},      // 3 (0.38111f, -0.02942f) / (0.38111f, 0.02942f)
		{0.47325f,   0.02620f},      // 4 (0.47325f, -0.02620f) / (0.47325f, 0.02620f)
		{0.55531f,   0.02338f},      // 5 (0.55531f, -0.02338f) / (0.55531f, 0.02338f)
		{0.63064f,   0.01728f},      // 6 (0.63064f, -0.01728f) / (0.63064f, 0.01728f)
		{0.70819f,   0.0f},          // 7 - 顶部 (0.70819f, 0.00000f)
	};

	MeshData.Vertices.Empty();
	
	// 生成顶点（每层左右两个顶点，除了顶部只有一个）
	for (int32 i = 0; i < SegmentData.Num(); i++)
	{
		float Height = SegmentData[i].Key * Scale;
		float HalfWidth = SegmentData[i].Value * Scale;
		
		if (i < SegmentData.Num() - 1) // 不是顶部
		{
			// 左顶点
			MeshData.Vertices.Add(FVector(0.0f, -HalfWidth, Height));
			// 右顶点
			MeshData.Vertices.Add(FVector(0.0f, HalfWidth, Height));
		}
		else // 顶部只有一个顶点
		{
			MeshData.Vertices.Add(FVector(0.0f, 0.0f, Height));
		}
	}

	// 生成三角形索引
	// 每个段由2个三角形组成（四边形）
	MeshData.Triangles.Empty();
	
	int32 NumSegments = SegmentData.Num() - 2; // 除去顶部的三角形
	for (int32 i = 0; i < NumSegments; i++)
	{
		int32 BaseIdx = i * 2;
		
		// 第一个三角形
		MeshData.Triangles.Add(BaseIdx);      // 左下
		MeshData.Triangles.Add(BaseIdx + 2);  // 左上
		MeshData.Triangles.Add(BaseIdx + 1);  // 右下
		
		// 第二个三角形
		MeshData.Triangles.Add(BaseIdx + 1);  // 右下
		MeshData.Triangles.Add(BaseIdx + 2);  // 左上
		MeshData.Triangles.Add(BaseIdx + 3);  // 右上
	}
	
	// 顶部三角形（收尾到一个点）
	int32 TopBaseIdx = NumSegments * 2;
	int32 TopVertexIdx = MeshData.Vertices.Num() - 1;
	
	MeshData.Triangles.Add(TopBaseIdx);      // 左
	MeshData.Triangles.Add(TopVertexIdx);    // 顶
	MeshData.Triangles.Add(TopBaseIdx + 1);  // 右

	// 生成法线（草叶朝向X正方向）
	MeshData.Normals.Empty();
	for (int32 i = 0; i < MeshData.Vertices.Num(); i++)
	{
		MeshData.Normals.Add(FVector(1.0f, 0.0f, 0.0f));
	}

	// 生成UV坐标
	// U: 0-1 从左到右
	// V: 0-1 从底到顶
	MeshData.UVs.Empty();
	for (int32 i = 0; i < SegmentData.Num() - 1; i++)
	{
		float V = SegmentData[i].Key / SegmentData.Last().Key; // 归一化高度
		MeshData.UVs.Add(FVector2D(0.0f, V)); // 左
		MeshData.UVs.Add(FVector2D(1.0f, V)); // 右
	}
	// 顶部顶点
	MeshData.UVs.Add(FVector2D(0.5f, 1.0f));

	// 生成顶点颜色（用于存储高度信息，可用于着色器）
	MeshData.VertexColors.Empty();
	for (int32 i = 0; i < SegmentData.Num() - 1; i++)
	{
		float HeightRatio = SegmentData[i].Key / SegmentData.Last().Key;
		uint8 HeightValue = FMath::Clamp(static_cast<int32>(HeightRatio * 255.0f), 0, 255);
		FColor Color(HeightValue, HeightValue, HeightValue, 255);
		MeshData.VertexColors.Add(Color); // 左
		MeshData.VertexColors.Add(Color); // 右
	}
	// 顶部顶点
	MeshData.VertexColors.Add(FColor::White);

	return MeshData;
}

FGrassBladeMeshData AGrassMesh::CreateGrassBladeMesh(int32 Segments, float Width, float Height)
{
	FGrassBladeMeshData MeshData;
	
	const float Scale = 100.0f; // 转换为厘米
	
	// 使用 Unity 教程中的精确顶点比例
	// 原始数据: 宽度 = 0.03444*2, 高度 = 0.70819
	// 按比例缩放到用户指定的 Width 和 Height
	const float OriginalWidth = 0.03444f * 2.0f;  // 原始总宽度
	const float OriginalHeight = 0.70819f;         // 原始高度
	
	float WidthScale = (Width * Scale) / (OriginalWidth * Scale);
	float HeightScale = (Height * Scale) / (OriginalHeight * Scale);
	
	// 直接使用 Unity 教程的顶点数据（已转换为 UE 坐标系）
	// 格式: FVector(X, Y, Z) 其中 Y 是宽度，Z 是高度
	MeshData.Vertices = {
		// 第0段（底部）
		FVector(0, -0.03444f * Scale * WidthScale, 0.0f),                                    // 0: 底部左
		FVector(0,  0.03444f * Scale * WidthScale, 0.0f),                                    // 1: 底部右
		// 第1段
		FVector(0, -0.03445f * Scale * WidthScale, 0.15599f * Scale * HeightScale),         // 2
		FVector(0,  0.03445f * Scale * WidthScale, 0.15599f * Scale * HeightScale),         // 3
		// 第2段
		FVector(0, -0.03193f * Scale * WidthScale, 0.27249f * Scale * HeightScale),         // 4
		FVector(0,  0.03193f * Scale * WidthScale, 0.27249f * Scale * HeightScale),         // 5
		// 第3段
		FVector(0, -0.02942f * Scale * WidthScale, 0.38111f * Scale * HeightScale),         // 6
		FVector(0,  0.02942f * Scale * WidthScale, 0.38111f * Scale * HeightScale),         // 7
		// 第4段
		FVector(0, -0.02620f * Scale * WidthScale, 0.47325f * Scale * HeightScale),         // 8
		FVector(0,  0.02620f * Scale * WidthScale, 0.47325f * Scale * HeightScale),         // 9
		// 第5段
		FVector(0, -0.02338f * Scale * WidthScale, 0.55531f * Scale * HeightScale),         // 10
		FVector(0,  0.02338f * Scale * WidthScale, 0.55531f * Scale * HeightScale),         // 11
		// 第6段
		FVector(0, -0.01728f * Scale * WidthScale, 0.63064f * Scale * HeightScale),         // 12
		FVector(0,  0.01728f * Scale * WidthScale, 0.63064f * Scale * HeightScale),         // 13
		// 第7段（顶部）
		FVector(0,  0.0f,                          0.70819f * Scale * HeightScale),         // 14: 顶点
	};
	
	// 三角形索引（逆时针缠绕）
	MeshData.Triangles = {
		0, 2, 1,    1, 2, 3,    // 第1段
		2, 4, 3,    3, 4, 5,    // 第2段
		4, 6, 5,    5, 6, 7,    // 第3段
		6, 8, 7,    7, 8, 9,    // 第4段
		8, 10, 9,   9, 10, 11,  // 第5段
		10, 12, 11, 11, 12, 13, // 第6段
		12, 14, 13,             // 顶部三角形
	};
	
	// 法线（朝向+X）
	for (int32 i = 0; i < MeshData.Vertices.Num(); i++)
	{
		MeshData.Normals.Add(FVector(1, 0, 0));
	}
	
	// UV坐标（V 值使用精确的高度比例）
	MeshData.UVs = {
		FVector2D(0, 0.0f),                              FVector2D(1, 0.0f),
		FVector2D(0, 0.15599f / 0.70819f),               FVector2D(1, 0.15599f / 0.70819f),
		FVector2D(0, 0.27249f / 0.70819f),               FVector2D(1, 0.27249f / 0.70819f),
		FVector2D(0, 0.38111f / 0.70819f),               FVector2D(1, 0.38111f / 0.70819f),
		FVector2D(0, 0.47325f / 0.70819f),               FVector2D(1, 0.47325f / 0.70819f),
		FVector2D(0, 0.55531f / 0.70819f),               FVector2D(1, 0.55531f / 0.70819f),
		FVector2D(0, 0.63064f / 0.70819f),               FVector2D(1, 0.63064f / 0.70819f),
		FVector2D(0.5f, 1.0f),
	};
	
	// 顶点颜色（高度值，用于着色器）
	const TArray<float> Heights = {0.0f, 0.15599f, 0.27249f, 0.38111f, 0.47325f, 0.55531f, 0.63064f, 0.70819f};
	for (int32 i = 0; i < Heights.Num(); i++)
	{
		float HeightRatio = Heights[i] / 0.70819f;
		uint8 HeightValue = FMath::Clamp(static_cast<int32>(HeightRatio * 255.0f), 0, 255);
		FColor Color(HeightValue, HeightValue, HeightValue, 255);
		
		if (i < Heights.Num() - 1)
		{
			MeshData.VertexColors.Add(Color); // 左
			MeshData.VertexColors.Add(Color); // 右
		}
		else
		{
			MeshData.VertexColors.Add(Color); // 顶点
		}
	}
	
	return MeshData;
}

void AGrassMesh::UpdateMesh()
{
	if (!ProceduralMesh)
	{
		return;
	}
	
	// 清除现有网格
	ProceduralMesh->ClearAllMeshSections();
	
	// 创建网格数据
	FGrassBladeMeshData MeshData = CreateGrassBladeMesh(BladeSegments, BladeWidth, BladeHeight);
	
	// 转换颜色为LinearColor
	TArray<FLinearColor> LinearColors;
	for (const FColor& Color : MeshData.VertexColors)
	{
		LinearColors.Add(FLinearColor(Color));
	}
	
	// 创建程序化网格
	ProceduralMesh->CreateMeshSection_LinearColor(
		0,                          // Section索引
		MeshData.Vertices,          // 顶点
		MeshData.Triangles,         // 三角形
		MeshData.Normals,           // 法线
		MeshData.UVs,               // UV
		LinearColors,               // 顶点颜色
		TArray<FProcMeshTangent>(), // 切线（自动计算）
		true                        // 创建碰撞
	);
	
	// 应用材质（如果已设置）
	if (GrassMaterial)
	{
		ProceduralMesh->SetMaterial(0, GrassMaterial);
	}
}

void AGrassMesh::ExportToStaticMesh()
{
	// 这个功能需要在编辑器模块中实现
	// 用于将程序化网格导出为静态网格资产
	UE_LOG(LogTemp, Warning, TEXT("导出静态网格功能需要在编辑器模块中实现"));
}
