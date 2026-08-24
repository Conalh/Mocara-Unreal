#include "MocaraBvhImporter.h"
#include "MocaraSettings.h"
#include "Misc/PackageName.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Curves/RichCurve.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneWeights.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Math/RotationMatrix.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "ReferenceSkeleton.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshTypes.h"
#include "StaticMeshAttributes.h"
#include "StaticToSkeletalMeshConverter.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/SkeletalMeshModel.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogMocaraImport, Log, All);

namespace
{
	FString NextToken(const FString& Line, int32& Index)
	{
		while (Index < Line.Len() && FChar::IsWhitespace(Line[Index]))
		{
			++Index;
		}
		const int32 Start = Index;
		while (Index < Line.Len() && !FChar::IsWhitespace(Line[Index]))
		{
			++Index;
		}
		return Line.Mid(Start, Index - Start);
	}

	UObject* CreateAsset(UClass* Class, const FString& PackagePath, const FString& AssetName)
	{
		const FString LongName = PackagePath / AssetName;
		UPackage* Package = CreatePackage(*LongName);
		Package->FullyLoad();
		UObject* Asset = NewObject<UObject>(Package, Class, *AssetName, RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}

	/**
	 * Un-publish a partially built asset. Anything left in the content browser after a
	 * failed import is a live crash risk -- a skeletal mesh with no LODs, for instance,
	 * fatally asserts as soon as a thumbnail or preview binds it to a component.
	 */
	void DiscardAsset(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		UPackage* Package = Asset->GetOutermost();
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Asset->MarkAsGarbage();
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}
	}

	/** UE needs exactly one root and unique bone names; a malformed hierarchy builds an unusable mesh. */
	bool ValidateHierarchy(const TArray<FMocaraBvhJoint>& Joints, FString& OutError)
	{
		TSet<FName> Seen;
		for (int32 Index = 0; Index < Joints.Num(); ++Index)
		{
			const FMocaraBvhJoint& Joint = Joints[Index];
			if (Joint.Name.IsNone())
			{
				OutError = FString::Printf(TEXT("BVH joint %d has no name."), Index);
				return false;
			}
			bool bAlreadySeen = false;
			Seen.Add(Joint.Name, &bAlreadySeen);
			if (bAlreadySeen)
			{
				OutError = FString::Printf(TEXT("BVH has duplicate joint name '%s'."), *Joint.Name.ToString());
				return false;
			}
			if (Index == 0)
			{
				if (Joint.ParentIndex != INDEX_NONE)
				{
					OutError = TEXT("BVH root joint has a parent.");
					return false;
				}
				continue;
			}
			if (Joint.ParentIndex == INDEX_NONE)
			{
				OutError = FString::Printf(
					TEXT("BVH has more than one root joint ('%s'); Unreal skeletons need exactly one."),
					*Joint.Name.ToString());
				return false;
			}
			if (Joint.ParentIndex >= Index)
			{
				OutError = FString::Printf(
					TEXT("BVH joint '%s' is declared before its parent."), *Joint.Name.ToString());
				return false;
			}
		}
		return true;
	}
}

namespace
{
	FQuat MakeRotationFromXY(const FVector& InX, const FVector& InY)
	{
		const FVector X = InX.GetSafeNormal();
		const FVector Y = InY.GetSafeNormal();
		if (X.IsNearlyZero() || Y.IsNearlyZero())
		{
			return FQuat::Identity;
		}
		return FRotationMatrix::MakeFromXY(X, Y).ToQuat().GetNormalized();
	}
}

/**
 * Re-express a rotation in the Unreal basis: R' = M * R * M^-1, where M = YUpToZUp.
 *
 * M is a handedness-flipping map (right-handed Y-up -> left-handed Z-up, det = -1), so it is
 * NOT enough to convert the rotation's X and Y axes and rebuild -- the conjugation permutes
 * which source axis lands on which destination axis. With
 *     M(ex) = -ey,  M(ey) = ez,  M(ez) = ex     =>  M^-1(ex) = ez, M^-1(ey) = -ex, M^-1(ez) = ey
 * the converted rotation's axes are
 *     R'(ex) = M(R(M^-1(ex))) = M(R(ez)) =  M(Qz)
 *     R'(ey) = M(R(M^-1(ey))) = M(R(-ex)) = -M(Qx)
 *
 * Verified against Kimodo's own reader (scipy intrinsic "ZYX"): with this and the channel
 * order below, imported joint positions match the reference to 0.0 cm over a full clip.
 */
FQuat FMocaraBvhImporter::ConvertQuatYUpToZUp(const FQuat& Q)
{
	return MakeRotationFromXY(YUpToZUp(Q.GetAxisZ()), -YUpToZUp(Q.GetAxisX()));
}

/**
 * Inverse of the above: R' = N * R * N^-1, where N = ZUpToYUp = M^-1.
 *     N(ex) = ez, N(ey) = -ex, N(ez) = ey   =>  N^-1(ex) = -ey, N^-1(ey) = ez
 *     R'(ex) = N(R(-ey)) = -N(Qy)
 *     R'(ey) = N(R(ez))  =  N(Qz)
 */
FQuat FMocaraBvhImporter::ConvertQuatZUpToYUp(const FQuat& Q)
{
	return MakeRotationFromXY(-ZUpToYUp(Q.GetAxisY()), ZUpToYUp(Q.GetAxisZ()));
}

bool FMocaraBvhImporter::SaveGeneratedAsset(UObject* Asset)
{
	if (!Asset)
	{
		return false;
	}
	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		return false;
	}

	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
	{
		UE_LOG(LogMocaraImport, Warning, TEXT("No filename for package %s"), *Package->GetName());
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, Asset, *Filename, SaveArgs);
	if (!bSaved)
	{
		UE_LOG(LogMocaraImport, Warning, TEXT("Failed to save %s"), *Filename);
	}
	return bSaved;
}

bool FMocaraBvhFile::Load(const FString& Filename, FString& OutError)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *Filename))
	{
		OutError = FString::Printf(TEXT("Could not read BVH: %s"), *Filename);
		return false;
	}

	Joints.Reset();
	FrameValues.Reset();
	TArray<int32> Stack;
	int32 ChannelCursor = 0;
	bool bInMotion = false;
	int32 MotionLine = 0;

	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty())
		{
			continue;
		}

		if (!bInMotion)
		{
			if (Line.StartsWith(TEXT("ROOT ")) || Line.StartsWith(TEXT("JOINT ")))
			{
				FMocaraBvhJoint Joint;
				Joint.Name = FName(*Line.Mid(Line.Find(TEXT(" ")) + 1));
				Joint.ParentIndex = Stack.Num() ? Stack.Last() : INDEX_NONE;
				Joint.ChannelOffset = ChannelCursor;
				Joints.Add(Joint);
				Stack.Add(Joints.Num() - 1);
			}
			else if (Line.StartsWith(TEXT("End Site")))
			{
				Stack.Add(INDEX_NONE);
			}
			else if (Line.StartsWith(TEXT("OFFSET")))
			{
				int32 Idx = 6;
				const float X = FCString::Atof(*NextToken(Line, Idx));
				const float Y = FCString::Atof(*NextToken(Line, Idx));
				const float Z = FCString::Atof(*NextToken(Line, Idx));
				if (Stack.Num() && Stack.Last() != INDEX_NONE)
				{
					Joints[Stack.Last()].OffsetYUp = FVector(X, Y, Z);
				}
			}
			else if (Line.StartsWith(TEXT("CHANNELS")))
			{
				int32 Idx = 8;
				const int32 Count = FCString::Atoi(*NextToken(Line, Idx));
				if (Stack.Num() && Stack.Last() != INDEX_NONE)
				{
					FMocaraBvhJoint& Joint = Joints[Stack.Last()];
					Joint.ChannelOffset = ChannelCursor;
					for (int32 C = 0; C < Count; ++C)
					{
						Joint.Channels.Add(NextToken(Line, Idx));
					}
					ChannelCursor += Count;
				}
			}
			else if (Line.StartsWith(TEXT("}")))
			{
				if (Stack.Num())
				{
					Stack.Pop();
				}
			}
			else if (Line.StartsWith(TEXT("MOTION")))
			{
				bInMotion = true;
			}
		}
		else
		{
			if (Line.StartsWith(TEXT("Frames:")))
			{
				NumFrames = FCString::Atoi(*Line.Mid(7).TrimStartAndEnd());
			}
			else if (Line.StartsWith(TEXT("Frame Time:")))
			{
				const float Parsed = FCString::Atof(*Line.Mid(11).TrimStartAndEnd());
				if (Parsed > UE_KINDA_SMALL_NUMBER)
				{
					FrameTime = Parsed;
				}
			}
			else
			{
				TArray<float> Values;
				int32 Idx = 0;
				while (Idx < Line.Len())
				{
					const FString Tok = NextToken(Line, Idx);
					if (Tok.IsEmpty())
					{
						break;
					}
					Values.Add(FCString::Atof(*Tok));
				}
				if (Values.Num())
				{
					FrameValues.Add(MoveTemp(Values));
				}
				++MotionLine;
			}
		}
	}

	if (!Joints.Num() || !FrameValues.Num())
	{
		OutError = TEXT("BVH file is missing hierarchy or motion data.");
		return false;
	}
	NumFrames = FrameValues.Num();
	return true;
}

static FQuat ChannelsToQuat(const FMocaraBvhJoint& Joint, const TArray<float>& Values)
{
	FQuat Q = FQuat::Identity;
	int32 Cursor = Joint.ChannelOffset;
	for (const FString& Channel : Joint.Channels)
	{
		if (!Values.IsValidIndex(Cursor))
		{
			break;
		}
		const float Degrees = Values[Cursor++];
		if (Channel.Contains(TEXT("position")))
		{
			continue;
		}
		FVector Axis = FVector::ZeroVector;
		if (Channel.StartsWith(TEXT("Xrotation")))
		{
			Axis = FVector::XAxisVector;
		}
		else if (Channel.StartsWith(TEXT("Yrotation")))
		{
			Axis = FVector::YAxisVector;
		}
		else if (Channel.StartsWith(TEXT("Zrotation")))
		{
			Axis = FVector::ZAxisVector;
		}
		else
		{
			continue;
		}

		// BVH channel order is intrinsic: "Zrotation Yrotation Xrotation" means
		// R = Rz * Ry * Rx, i.e. the LAST-listed channel is applied first. UE's
		// A * B applies B then A, so accumulate on the right, not the left.
		Q = Q * FQuat(Axis, FMath::DegreesToRadians(Degrees));
	}
	return Q.GetNormalized();
}

static FVector ChannelsToPos(const FMocaraBvhJoint& Joint, const TArray<float>& Values, bool bHasPos)
{
	FVector Pos = Joint.OffsetYUp;
	if (!bHasPos)
	{
		return Pos;
	}
	int32 Cursor = Joint.ChannelOffset;
	for (const FString& Channel : Joint.Channels)
	{
		if (!Values.IsValidIndex(Cursor))
		{
			break;
		}
		const float V = Values[Cursor++];
		if (Channel == TEXT("Xposition"))
		{
			Pos.X = V;
		}
		else if (Channel == TEXT("Yposition"))
		{
			Pos.Y = V;
		}
		else if (Channel == TEXT("Zposition"))
		{
			Pos.Z = V;
		}
	}
	return Pos;
}

/**
 * Write per-foot speed curves (cm/s) onto the imported clip.
 *
 * UE's Speed Planting retarget op decides when a foot is planted by reading a float curve
 * on the SOURCE animation. A BVH carries no curves, so without this the op has nothing to
 * work with and the retargeted feet skate along the floor.
 */
static void AddFootSpeedCurves(
	IAnimationDataController& Controller,
	const FMocaraBvhFile& File,
	const TArray<TArray<FTransform>>& LocalByJoint,
	float Fps)
{
	if (File.NumFrames < 2 || Fps <= 0.f)
	{
		return;
	}

	// Component-space FK. BVH hierarchies are depth-first, so a parent always precedes its
	// child and a single forward pass is enough.
	const int32 NumJoints = File.Joints.Num();
	TArray<TArray<FVector>> ComponentPos;
	ComponentPos.SetNum(NumJoints);
	for (TArray<FVector>& Track : ComponentPos)
	{
		Track.SetNum(File.NumFrames);
	}

	TArray<FTransform> Comp;
	Comp.SetNum(NumJoints);
	for (int32 Frame = 0; Frame < File.NumFrames; ++Frame)
	{
		for (int32 Index = 0; Index < NumJoints; ++Index)
		{
			if (!LocalByJoint[Index].IsValidIndex(Frame))
			{
				return;
			}
			const int32 Parent = File.Joints[Index].ParentIndex;
			Comp[Index] = (Parent == INDEX_NONE)
				? LocalByJoint[Index][Frame]
				: LocalByJoint[Index][Frame] * Comp[Parent];
			ComponentPos[Index][Frame] = Comp[Index].GetLocation();
		}
	}

	for (const FName FootBone : { FName(TEXT("LeftFoot")), FName(TEXT("RightFoot")) })
	{
		const int32 Index = File.Joints.IndexOfByPredicate(
			[FootBone](const FMocaraBvhJoint& Joint) { return Joint.Name == FootBone; });
		if (Index == INDEX_NONE)
		{
			continue;
		}

		const TArray<FVector>& Track = ComponentPos[Index];
		TArray<FRichCurveKey> Keys;
		Keys.Reserve(File.NumFrames);
		for (int32 Frame = 0; Frame < File.NumFrames; ++Frame)
		{
			// Backward difference; frame 0 borrows frame 1 so the clip does not start
			// looking planted when it is not.
			const int32 A = FMath::Max(Frame, 1);
			const float Speed = static_cast<float>(FVector::Dist(Track[A], Track[A - 1])) * Fps;
			Keys.Emplace(static_cast<float>(Frame) / Fps, Speed);
		}

		const FAnimationCurveIdentifier CurveId(
			FName(*(FootBone.ToString() + TEXT("_speed"))), ERawCurveTrackTypes::RCT_Float);
		Controller.AddCurve(CurveId, AACF_Editable, false);
		Controller.SetCurveKeys(CurveId, Keys, false);

		UE_LOG(LogMocaraImport, Display, TEXT("  speed curve %s_speed: %d keys, peak %.1f cm/s"),
			*FootBone.ToString(), Keys.Num(),
			Keys.Num() ? FMath::Max<float>(0.f, Algo::MaxElement(Keys,
				[](const FRichCurveKey& L, const FRichCurveKey& R){ return L.Value < R.Value; })->Value) : 0.f);
	}
}

bool FMocaraBvhImporter::ImportFile(const FString& Filename, const FString& DestinationPath, const FString& AssetBaseName, bool bInPlace, FMocaraImportedClip& OutClip, FString& OutError, bool bSaveAssets)
{
	OutClip = FMocaraImportedClip();
	OutError.Reset();

	FMocaraBvhFile File;
	if (!File.Load(Filename, OutError))
	{
		return false;
	}

	if (!ValidateHierarchy(File.Joints, OutError))
	{
		return false;
	}

	FReferenceSkeleton RefSkel;
	{
		FReferenceSkeletonModifier Modifier(RefSkel, nullptr);
		for (int32 Index = 0; Index < File.Joints.Num(); ++Index)
		{
			const FMocaraBvhJoint& Joint = File.Joints[Index];
			FMeshBoneInfo Info(Joint.Name, Joint.Name.ToString(), Joint.ParentIndex);
			const FTransform Local(FQuat::Identity, YUpToZUp(Joint.OffsetYUp));
			Modifier.Add(Info, Local);
		}
	}

	// The reference skeleton silently drops malformed bones; if it did, the skin weights
	// below would point at bones that do not exist and the mesh build would produce nothing.
	if (RefSkel.GetRawBoneNum() != File.Joints.Num())
	{
		OutError = FString::Printf(
			TEXT("BVH hierarchy is not valid for Unreal: %d of %d joints were rejected."),
			File.Joints.Num() - RefSkel.GetRawBoneNum(),
			File.Joints.Num());
		return false;
	}

	USkeletalMesh* Mesh = Cast<USkeletalMesh>(CreateAsset(USkeletalMesh::StaticClass(), DestinationPath, AssetBaseName + TEXT("_Mesh")));
	if (!Mesh)
	{
		OutError = TEXT("Could not create the SOMA skeletal mesh asset.");
		return false;
	}

	FMeshDescription MeshDesc;
	FSkeletalMeshAttributes Attrs(MeshDesc);
	Attrs.Register();
	Attrs.RegisterSkinWeightAttribute(NAME_None);
	FSkinWeightsVertexAttributesRef Skin = Attrs.GetVertexSkinWeights();
	Attrs.ReserveNewBones(File.Joints.Num());

	TArray<FTransform> ComponentPose;
	ComponentPose.SetNum(File.Joints.Num());
	for (int32 Index = 0; Index < File.Joints.Num(); ++Index)
	{
		const FMocaraBvhJoint& Joint = File.Joints[Index];
		FBoneID BoneId = Attrs.CreateBone();
		Attrs.GetBoneNames()[BoneId] = Joint.Name;
		Attrs.GetBoneParentIndices()[BoneId] = Joint.ParentIndex;
		const FTransform Local(FQuat::Identity, YUpToZUp(Joint.OffsetYUp));
		FTransform Component = Local;
		if (Joint.ParentIndex != INDEX_NONE)
		{
			Component = Local * ComponentPose[Joint.ParentIndex];
		}
		ComponentPose[Index] = Component;
		Attrs.GetBonePoses()[BoneId] = Component;
	}

	FPolygonGroupID Group = MeshDesc.CreatePolygonGroup();
	Attrs.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Default");
	TVertexInstanceAttributesRef<FVector2f> UVs = Attrs.GetVertexInstanceUVs();
	if (UVs.GetNumChannels() == 0)
	{
		UVs.SetNumChannels(1);
	}
	for (int32 Index = 0; Index < File.Joints.Num(); ++Index)
	{
		const FVector Center = ComponentPose[Index].GetLocation();
		const FVector A = Center + FVector(1.5f, 0.f, 0.f);
		const FVector B = Center + FVector(-0.75f, 1.3f, 0.f);
		const FVector C = Center + FVector(-0.75f, -1.3f, 0.f);
		const FVertexID V0 = MeshDesc.CreateVertex();
		const FVertexID V1 = MeshDesc.CreateVertex();
		const FVertexID V2 = MeshDesc.CreateVertex();
		MeshDesc.GetVertexPositions()[V0] = FVector3f(A);
		MeshDesc.GetVertexPositions()[V1] = FVector3f(B);
		MeshDesc.GetVertexPositions()[V2] = FVector3f(C);
		const FVertexInstanceID I0 = MeshDesc.CreateVertexInstance(V0);
		const FVertexInstanceID I1 = MeshDesc.CreateVertexInstance(V1);
		const FVertexInstanceID I2 = MeshDesc.CreateVertexInstance(V2);
		UVs.Set(I0, 0, FVector2f(0.f, 0.f));
		UVs.Set(I1, 0, FVector2f(1.f, 0.f));
		UVs.Set(I2, 0, FVector2f(0.f, 1.f));
		TArray<FVertexInstanceID> Loop = {I0, I1, I2};
		MeshDesc.CreatePolygon(Group, Loop);

		TArray<UE::AnimationCore::FBoneWeight> Weights;
		Weights.Add(UE::AnimationCore::FBoneWeight(static_cast<FBoneIndexType>(Index), 1.0f));
		Skin.Set(V0, Weights);
		Skin.Set(V1, Weights);
		Skin.Set(V2, Weights);
	}

	TArray<FSkeletalMaterial> Materials;
	Materials.Add(FSkeletalMaterial(UMaterial::GetDefaultMaterial(MD_Surface), FName(TEXT("Default")), FName(TEXT("Default"))));
	FStaticToSkeletalMeshConverter::FInitializationParams InitParams;
	InitParams.Materials = Materials;
	InitParams.bRecomputeNormals = true;
	InitParams.bRecomputeTangents = true;
	TArray<const FMeshDescription*> Descriptions;
	Descriptions.Add(&MeshDesc);
	if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(Mesh, Descriptions, RefSkel, InitParams))
	{
		OutError = TEXT("Failed to build SOMA skeletal mesh from BVH.");
		DiscardAsset(Mesh);
		return false;
	}

	// A skeletal mesh with no LOD/source model is fatal, not cosmetic: the first
	// USkeletalMeshComponent to reference it (content browser thumbnail, IK rig
	// preview, the retarget batch op) indexes LODModels[0] and takes the editor down
	// with "Array index out of bounds: 0 into an array of size 0". Fail the import
	// and remove the asset instead of handing it downstream.
	const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
	UE_LOG(LogMocaraImport, Display,
		TEXT("BVH '%s': %d joints, %d frames @ %.4fs, refskel bones=%d, verts=%d, tris=%d -> LODNum=%d, LODModels=%d"),
		*FPaths::GetCleanFilename(Filename),
		File.Joints.Num(),
		File.NumFrames,
		File.FrameTime,
		RefSkel.GetRawBoneNum(),
		MeshDesc.Vertices().Num(),
		MeshDesc.Triangles().Num(),
		Mesh->GetLODNum(),
		ImportedModel ? ImportedModel->LODModels.Num() : -1);

	if (Mesh->GetLODNum() <= 0 || ImportedModel == nullptr || ImportedModel->LODModels.Num() == 0)
	{
		UE_LOG(LogMocaraImport, Error,
			TEXT("Skeletal mesh build produced no LODs. Discarding '%s' rather than letting the editor assert on it."),
			*Mesh->GetPathName());

		OutError = TEXT("BVH import produced a skeletal mesh with no LODs; discarding it so the editor stays stable.");
		DiscardAsset(Mesh);
		return false;
	}

	USkeleton* Skeleton = Cast<USkeleton>(CreateAsset(USkeleton::StaticClass(), DestinationPath, AssetBaseName + TEXT("_Skeleton")));
	if (!Skeleton || !Skeleton->MergeAllBonesToBoneTree(Mesh))
	{
		OutError = TEXT("Could not build a skeleton from the BVH hierarchy.");
		DiscardAsset(Skeleton);
		DiscardAsset(Mesh);
		return false;
	}
	Mesh->SetSkeleton(Skeleton);
	Skeleton->SetPreviewMesh(Mesh);

	UAnimSequence* Sequence = Cast<UAnimSequence>(CreateAsset(UAnimSequence::StaticClass(), DestinationPath, AssetBaseName));
	if (!Sequence)
	{
		OutError = TEXT("Could not create the animation sequence asset.");
		DiscardAsset(Skeleton);
		DiscardAsset(Mesh);
		return false;
	}
	Sequence->SetSkeleton(Skeleton);
	Sequence->SetPreviewMesh(Mesh);

	IAnimationDataController& Controller = Sequence->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Import Kimodo BVH")), false);
	Controller.InitializeModel();
	const float SafeFrameTime = (File.FrameTime > UE_KINDA_SMALL_NUMBER) ? File.FrameTime : (1.0f / 30.0f);
	const int32 Fps = FMath::Clamp(FMath::RoundToInt(1.0f / SafeFrameTime), 1, 1000);
	Controller.SetFrameRate(FFrameRate(Fps, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(File.NumFrames), false);

	// Keep every local transform we key, so we can run FK below for the foot-speed curves.
	TArray<TArray<FTransform>> LocalByJoint;
	LocalByJoint.SetNum(File.Joints.Num());

	for (int32 JointIndex = 0; JointIndex < File.Joints.Num(); ++JointIndex)
	{
		const FMocaraBvhJoint& Joint = File.Joints[JointIndex];
		Controller.AddBoneCurve(Joint.Name, false);
		TArray<FVector> PosKeys;
		TArray<FQuat> RotKeys;
		TArray<FVector> ScaleKeys;
		PosKeys.Reserve(File.NumFrames);
		RotKeys.Reserve(File.NumFrames);
		ScaleKeys.Reserve(File.NumFrames);
		const bool bHasPos = Joint.Channels.Contains(TEXT("Xposition"));
		for (int32 Frame = 0; Frame < File.NumFrames; ++Frame)
		{
			FVector PosYUp = ChannelsToPos(Joint, File.FrameValues[Frame], bHasPos);
			if (bInPlace && bHasPos)
			{
				PosYUp.X = Joint.OffsetYUp.X;
				PosYUp.Z = Joint.OffsetYUp.Z;
			}
			PosKeys.Add(YUpToZUp(PosYUp));
			RotKeys.Add(ConvertQuatYUpToZUp(ChannelsToQuat(Joint, File.FrameValues[Frame])));
			ScaleKeys.Add(FVector::OneVector);
		}
		Controller.SetBoneTrackKeys(Joint.Name, PosKeys, RotKeys, ScaleKeys, false);

		LocalByJoint[JointIndex].Reserve(File.NumFrames);
		for (int32 Frame = 0; Frame < File.NumFrames; ++Frame)
		{
			LocalByJoint[JointIndex].Emplace(RotKeys[Frame], PosKeys[Frame]);
		}
	}

	AddFootSpeedCurves(Controller, File, LocalByJoint, static_cast<float>(Fps));

	Controller.NotifyPopulated();
	Controller.CloseBracket(false);
	Sequence->PostEditChange();

	OutClip.Skeleton = Skeleton;
	OutClip.Mesh = Mesh;
	OutClip.Sequence = Sequence;

	// Diagnostics pass bSaveAssets=false so Mocara.VerifyBVH and friends do not litter
	// the content browser with throwaway assets every time they run.
	bool bAutoSave = bSaveAssets;
	if (bAutoSave)
	if (const UMocaraSettings* SaveSettings = GetDefault<UMocaraSettings>())
	{
		bAutoSave = SaveSettings->bAutoSaveGenerated;
	}
	if (bAutoSave)
	{
		// Use &= deliberately so every package gets a save attempt even if an earlier
		// one failed. The caller can keep using the valid in-memory assets and warn.
		OutClip.bAllAssetsSaved &= SaveGeneratedAsset(Mesh);
		OutClip.bAllAssetsSaved &= SaveGeneratedAsset(Skeleton);
		OutClip.bAllAssetsSaved &= SaveGeneratedAsset(Sequence);
	}
	return true;
}

/**
 * Console hook so the importer can be exercised without the sidecar or the Slate UI:
 *   Mocara.ImportBVH <bvh-path> [/Game/Dest/Path] [AssetBaseName]
 * Useful for reproducing import failures against a known BVH.
 */
static void MocaraImportBvhCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogMocaraImport, Error, TEXT("Usage: Mocara.ImportBVH <bvh-path> [/Game/Dest/Path] [AssetBaseName]"));
		return;
	}

	const FString Filename = Args[0];
	const FString DestPath = Args.IsValidIndex(1) ? Args[1] : TEXT("/Game/Mocara/Generated");
	const FString BaseName = Args.IsValidIndex(2)
		? Args[2]
		: (TEXT("AS_Test_") + FPaths::GetBaseFilename(Filename));

	UE_LOG(LogMocaraImport, Display, TEXT("Mocara.ImportBVH '%s' -> %s/%s"), *Filename, *DestPath, *BaseName);

	FMocaraImportedClip Clip;
	FString Error;
	const bool bOk = FMocaraBvhImporter::ImportFile(Filename, DestPath, BaseName, /*bInPlace=*/false, Clip, Error);

	if (bOk)
	{
		if (Clip.bAllAssetsSaved)
		{
			UE_LOG(LogMocaraImport, Display, TEXT("Mocara.ImportBVH SUCCEEDED: mesh=%s skeleton=%s sequence=%s"),
				*GetNameSafe(Clip.Mesh), *GetNameSafe(Clip.Skeleton), *GetNameSafe(Clip.Sequence));
		}
		else
		{
			UE_LOG(LogMocaraImport, Warning,
				TEXT("Mocara.ImportBVH created the assets, but one or more packages could not be saved: mesh=%s skeleton=%s sequence=%s"),
				*GetNameSafe(Clip.Mesh), *GetNameSafe(Clip.Skeleton), *GetNameSafe(Clip.Sequence));
		}
	}
	else
	{
		UE_LOG(LogMocaraImport, Error, TEXT("Mocara.ImportBVH FAILED: %s"), *Error);
	}
}

static FAutoConsoleCommand GMocaraImportBvhCommand(
	TEXT("Mocara.ImportBVH"),
	TEXT("Import a BVH through the Mocara importer: Mocara.ImportBVH <path> [/Game/Dest] [AssetBaseName]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&MocaraImportBvhCommand));

/**
 * Import a BVH and print component-space positions for a few landmark bones, so the
 * coordinate conversion can be checked against an external reference.
 *   Mocara.VerifyBVH <bvh-path> [frame]
 */
static void MocaraVerifyBvhCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogMocaraImport, Error, TEXT("Usage: Mocara.VerifyBVH <bvh-path> [frame]"));
		return;
	}
	const int32 Frame = Args.IsValidIndex(1) ? FCString::Atoi(*Args[1]) : 0;

	FMocaraImportedClip Clip;
	FString Error;
	if (!FMocaraBvhImporter::ImportFile(Args[0], TEXT("/Game/Mocara/Verify"), TEXT("AS_Verify"), false, Clip, Error, /*bSaveAssets=*/false))
	{
		UE_LOG(LogMocaraImport, Error, TEXT("Mocara.VerifyBVH import failed: %s"), *Error);
		return;
	}

	const FReferenceSkeleton& Ref = Clip.Mesh->GetRefSkeleton();
	const IAnimationDataModel* Model = Clip.Sequence->GetDataModel();
	if (!Model)
	{
		UE_LOG(LogMocaraImport, Error, TEXT("Mocara.VerifyBVH: sequence has no data model."));
		return;
	}

	TArray<FTransform> Component;
	Component.SetNum(Ref.GetNum());
	for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
	{
		const FName Bone = Ref.GetBoneName(Index);
		FTransform Local = Ref.GetRefBonePose()[Index];
		if (Model->IsValidBoneTrackName(Bone))
		{
			Local = Model->EvaluateBoneTrackTransform(Bone, FFrameTime(Frame), EAnimInterpolationType::Step);
		}
		const int32 Parent = Ref.GetParentIndex(Index);
		Component[Index] = (Parent == INDEX_NONE) ? Local : Local * Component[Parent];
	}

	static const TArray<FName> Landmarks = {
		TEXT("Hips"), TEXT("Chest"), TEXT("Head"),
		TEXT("LeftHand"), TEXT("RightHand"), TEXT("LeftFoot"), TEXT("RightFoot")
	};

	// Curve readback: the Speed Planting retarget op reads these off the source clip.
	if (const IAnimationDataModel* CurveModel = Clip.Sequence->GetDataModel())
	{
		const TArray<FFloatCurve>& Floats = CurveModel->GetFloatCurves();
		UE_LOG(LogMocaraImport, Display, TEXT("float curves on imported clip: %d"), Floats.Num());
		for (const FFloatCurve& Curve : Floats)
		{
			float MinV = TNumericLimits<float>::Max();
			float MaxV = -MinV;
			for (const FRichCurveKey& Key : Curve.FloatCurve.GetConstRefOfKeys())
			{
				MinV = FMath::Min(MinV, Key.Value);
				MaxV = FMath::Max(MaxV, Key.Value);
			}
			UE_LOG(LogMocaraImport, Display, TEXT("    curve '%s': %d keys, range %.1f..%.1f"),
				*Curve.GetName().ToString(), Curve.FloatCurve.GetNumKeys(), MinV, MaxV);
		}
	}

	UE_LOG(LogMocaraImport, Display, TEXT("VerifyBVH '%s' frame %d (component space, cm):"),
		*FPaths::GetCleanFilename(Args[0]), Frame);
	for (const FName Bone : Landmarks)
	{
		const int32 Index = Ref.FindBoneIndex(Bone);
		if (Index == INDEX_NONE)
		{
			continue;
		}
		const FVector P = Component[Index].GetLocation();
		UE_LOG(LogMocaraImport, Display, TEXT("    %-12s = (%8.2f, %8.2f, %8.2f)"), *Bone.ToString(), P.X, P.Y, P.Z);
	}
}

static FAutoConsoleCommand GMocaraVerifyBvhCommand(
	TEXT("Mocara.VerifyBVH"),
	TEXT("Import a BVH and print component-space landmark bone positions: Mocara.VerifyBVH <path> [frame]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&MocaraVerifyBvhCommand));
