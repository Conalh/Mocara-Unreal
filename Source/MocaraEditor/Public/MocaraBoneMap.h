#pragma once

#include "CoreMinimal.h"

struct FMocaraBoneMap
{
	static const TArray<TPair<FName, FName>>& MannyToSoma()
	{
		static const TArray<TPair<FName, FName>> Map = {
			{TEXT("pelvis"), TEXT("Hips")},
			{TEXT("spine_01"), TEXT("Spine1")},
			{TEXT("spine_02"), TEXT("Spine2")},
			{TEXT("spine_03"), TEXT("Chest")},
			{TEXT("spine_04"), TEXT("Chest")},
			{TEXT("spine_05"), TEXT("Chest")},
			{TEXT("neck_01"), TEXT("Neck1")},
			{TEXT("neck_02"), TEXT("Neck2")},
			{TEXT("head"), TEXT("Head")},
			{TEXT("clavicle_l"), TEXT("LeftShoulder")},
			{TEXT("upperarm_l"), TEXT("LeftArm")},
			{TEXT("lowerarm_l"), TEXT("LeftForeArm")},
			{TEXT("hand_l"), TEXT("LeftHand")},
			{TEXT("clavicle_r"), TEXT("RightShoulder")},
			{TEXT("upperarm_r"), TEXT("RightArm")},
			{TEXT("lowerarm_r"), TEXT("RightForeArm")},
			{TEXT("hand_r"), TEXT("RightHand")},
			{TEXT("thigh_l"), TEXT("LeftLeg")},
			{TEXT("calf_l"), TEXT("LeftShin")},
			{TEXT("foot_l"), TEXT("LeftFoot")},
			{TEXT("ball_l"), TEXT("LeftToeBase")},
			{TEXT("thigh_r"), TEXT("RightLeg")},
			{TEXT("calf_r"), TEXT("RightShin")},
			{TEXT("foot_r"), TEXT("RightFoot")},
			{TEXT("ball_r"), TEXT("RightToeBase")},
		};
		return Map;
	}

	static FName ToSoma(FName MannyBone)
	{
		for (const TPair<FName, FName>& Pair : MannyToSoma())
		{
			if (Pair.Key == MannyBone)
			{
				return Pair.Value;
			}
		}
		return MannyBone;
	}

	static FName FindExisting(const TArray<FName>& Names, const TArray<FName>& Candidates)
	{
		for (const FName Candidate : Candidates)
		{
			if (Names.Contains(Candidate))
			{
				return Candidate;
			}
		}
		return NAME_None;
	}
};
