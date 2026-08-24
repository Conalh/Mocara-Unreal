#pragma once

#include "CoreMinimal.h"

namespace UE::Mocara::Viewport
{
	inline constexpr float FollowDiscontinuityCm = 60.f;

	inline bool ShouldReanchorFollowCamera(const FVector& Delta)
	{
		return Delta.SizeSquared() > FMath::Square(FollowDiscontinuityCm);
	}
}
