#include "MocaraPreviewCharacterResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"

namespace
{
	bool UsesTargetSkeleton(const USkeletalMesh* CandidateMesh, const USkeletalMesh* TargetMesh)
	{
		return CandidateMesh && TargetMesh && CandidateMesh->GetSkeleton()
			&& CandidateMesh->GetSkeleton() == TargetMesh->GetSkeleton();
	}

	int32 BodyNameScore(FName ComponentName)
	{
		const FString Name = ComponentName.ToString();
		if (Name.Equals(TEXT("Body"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Body_GEN_VARIABLE"), ESearchCase::IgnoreCase))
		{
			return 100;
		}
		return Name.Contains(TEXT("Body"), ESearchCase::IgnoreCase) ? 50 : 0;
	}

	int32 PreviewAssetScore(const FAssetData& Asset)
	{
		const FString AssetName = Asset.AssetName.ToString();
		FString ParentFolder;
		FString PackagePath = Asset.PackagePath.ToString();
		PackagePath.Split(TEXT("/"), nullptr, &ParentFolder, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		int32 Score = AssetName.StartsWith(TEXT("BP_")) ? 100 : 0;
		if (AssetName == FString(TEXT("BP_")) + ParentFolder)
		{
			Score += 1000;
		}
		return Score;
	}

	bool FindBodyTemplate(UClass* CandidateClass, USkeletalMesh* TargetMesh, FName& OutComponentName)
	{
		int32 BestScore = INDEX_NONE;
		FName BestName = NAME_None;
		for (UClass* Class = CandidateClass; Class; Class = Class->GetSuperClass())
		{
			const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{
				continue;
			}
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				const USkeletalMeshComponent* Template = Node
					? Cast<USkeletalMeshComponent>(Node->ComponentTemplate)
					: nullptr;
				if (!Template || !UsesTargetSkeleton(Template->GetSkeletalMeshAsset(), TargetMesh))
				{
					continue;
				}
				const FName CandidateName = Node->GetVariableName();
				const int32 Score = BodyNameScore(CandidateName);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestName = CandidateName;
				}
			}
		}
		OutComponentName = BestName;
		return !BestName.IsNone();
	}
}

UClass* FMocaraPreviewCharacterResolver::FindCompatibleClass(
	USkeletalMesh* TargetMesh,
	UClass* PreferredClass,
	FName& OutBodyComponentName)
{
	OutBodyComponentName = NAME_None;
	if (!TargetMesh || !TargetMesh->GetSkeleton())
	{
		return nullptr;
	}

	if (PreferredClass && PreferredClass->IsChildOf(AActor::StaticClass())
		&& FindBodyTemplate(PreferredClass, TargetMesh, OutBodyComponentName))
	{
		return PreferredClass;
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game/MetaHumans"));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
	{
		const int32 LeftScore = PreviewAssetScore(Left);
		const int32 RightScore = PreviewAssetScore(Right);
		if (LeftScore != RightScore)
		{
			return LeftScore > RightScore;
		}
		return Left.GetObjectPathString() < Right.GetObjectPathString();
	});

	for (const FAssetData& Asset : Assets)
	{
		const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
		UClass* CandidateClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
		if (!CandidateClass || !CandidateClass->IsChildOf(AActor::StaticClass()))
		{
			continue;
		}
		FName BodyComponentName;
		if (FindBodyTemplate(CandidateClass, TargetMesh, BodyComponentName))
		{
			OutBodyComponentName = BodyComponentName;
			return CandidateClass;
		}
	}
	return nullptr;
}

USkeletalMeshComponent* FMocaraPreviewCharacterResolver::FindCompatibleBodyComponent(
	AActor* Actor,
	USkeletalMesh* TargetMesh,
	FName PreferredComponentName)
{
	if (!Actor || !TargetMesh)
	{
		return nullptr;
	}

	TInlineComponentArray<USkeletalMeshComponent*> Components(Actor);
	USkeletalMeshComponent* BestComponent = nullptr;
	int32 BestScore = INDEX_NONE;
	for (USkeletalMeshComponent* Component : Components)
	{
		if (!Component || !UsesTargetSkeleton(Component->GetSkeletalMeshAsset(), TargetMesh))
		{
			continue;
		}
		int32 Score = BodyNameScore(Component->GetFName());
		if (!PreferredComponentName.IsNone()
			&& (Component->GetFName() == PreferredComponentName
				|| Component->GetName().StartsWith(PreferredComponentName.ToString())))
		{
			Score += 200;
		}
		if (Score > BestScore)
		{
			BestScore = Score;
			BestComponent = Component;
		}
	}
	return BestComponent;
}
