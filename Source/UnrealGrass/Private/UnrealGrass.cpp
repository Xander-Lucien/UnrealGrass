// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGrass.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FUnrealGrassModule"

void FUnrealGrassModule::StartupModule()
{
	// 注册插件的着色器目录
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("UnrealGrass"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/UnrealGrass"), PluginShaderDir);
}

void FUnrealGrassModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUnrealGrassModule, UnrealGrass)