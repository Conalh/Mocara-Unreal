#include "SMocaraTimeline.h"
#include "MocaraPoseEditing.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

bool SMocaraTimeline::KeyBelongsToLane(const FMocaraPoseKey& Key, const FLane& Lane)
{
	if (Lane.bClipBar)
	{
		return false;
	}
	if (Lane.BoneFilter != NAME_None)
	{
		return Key.BoneName == Lane.BoneFilter;
	}
	if (Lane.Label == TEXT("Root Path"))
	{
		return FMocaraPoseEditing::ClassifyBoneLane(Key.BoneName) == EMocaraPoseLane::RootPath;
	}
	if (Lane.Label == TEXT("Left Hand"))
	{
		return FMocaraPoseEditing::ClassifyBoneLane(Key.BoneName) == EMocaraPoseLane::LeftHand;
	}
	if (Lane.Label == TEXT("Right Hand"))
	{
		return FMocaraPoseEditing::ClassifyBoneLane(Key.BoneName) == EMocaraPoseLane::RightHand;
	}
	if (Lane.Label == TEXT("Left Foot"))
	{
		return FMocaraPoseEditing::ClassifyBoneLane(Key.BoneName) == EMocaraPoseLane::LeftFoot;
	}
	if (Lane.Label == TEXT("Right Foot"))
	{
		return FMocaraPoseEditing::ClassifyBoneLane(Key.BoneName) == EMocaraPoseLane::RightFoot;
	}
	if (Lane.bFullBody)
	{
		return FMocaraPoseEditing::ClassifyBoneLane(Key.BoneName) == EMocaraPoseLane::FullBody;
	}
	return false;
}

void SMocaraTimeline::Construct(const FArguments& InArgs)
{
	Keys = InArgs._Keys;
	NumFrames = InArgs._NumFrames;
	Playhead = InArgs._Playhead;
	SelectedKey = InArgs._SelectedKey;
	OnPlayheadChanged = InArgs._OnPlayheadChanged;
	OnKeySelected = InArgs._OnKeySelected;
	OnKeyMoved = InArgs._OnKeyMoved;
	OnKeyResized = InArgs._OnKeyResized;
	OnKeyMoveStarted = InArgs._OnKeyMoveStarted;
	OnKeyMoveEnded = InArgs._OnKeyMoveEnded;
}

TArray<SMocaraTimeline::FLane> SMocaraTimeline::BuildLanes() const
{
	TArray<FLane> Lanes;
	Lanes.Add({TEXT("Animation"), NAME_None, true, false});
	Lanes.Add({TEXT("Root Path"), NAME_None, false, false});
	Lanes.Add({TEXT("Full-Body"), NAME_None, false, true});

	TSet<FName> Extra;
	if (Keys)
	{
		for (const FMocaraPoseKey& Key : *Keys)
		{
			if (FMocaraPoseEditing::ClassifyBoneLane(Key.BoneName) == EMocaraPoseLane::FullBody)
			{
				Extra.Add(Key.BoneName);
			}
		}
	}
	TArray<FName> ExtraBones = Extra.Array();
	ExtraBones.Sort([](FName Left, FName Right)
	{
		const FString LeftLabel = FMocaraPoseEditing::DisplayLabel(Left);
		const FString RightLabel = FMocaraPoseEditing::DisplayLabel(Right);
		const int32 LabelOrder = LeftLabel.Compare(RightLabel, ESearchCase::IgnoreCase);
		return LabelOrder == 0
			? Left.LexicalLess(Right)
			: LabelOrder < 0;
	});
	for (const FName Bone : ExtraBones)
	{
		Lanes.Add({FMocaraPoseEditing::DisplayLabel(Bone), Bone, false, false});
	}

	Lanes.Add({TEXT("Left Hand"), NAME_None, false, false});
	Lanes.Add({TEXT("Right Hand"), NAME_None, false, false});
	Lanes.Add({TEXT("Left Foot"), NAME_None, false, false});
	Lanes.Add({TEXT("Right Foot"), NAME_None, false, false});
	return Lanes;
}

FVector2D SMocaraTimeline::ComputeDesiredSize(float) const
{
	return FVector2D(480.0, RulerHeight + BuildLanes().Num() * LaneHeight + 4.f);
}

int32 SMocaraTimeline::FrameFromX(const FGeometry& Geo, float LocalX) const
{
	const int32 Frames = FMath::Max(1, NumFrames.Get());
	const float TrackW = FMath::Max(1.f, Geo.GetLocalSize().X - LabelWidth);
	const float T = FMath::Clamp((LocalX - LabelWidth) / TrackW, 0.f, 1.f);
	return FMath::Clamp(FMath::RoundToInt(T * (Frames - 1)), 0, Frames - 1);
}

float SMocaraTimeline::XFromFrame(const FGeometry& Geo, int32 Frame) const
{
	const int32 Frames = FMath::Max(1, NumFrames.Get());
	const float TrackW = FMath::Max(1.f, Geo.GetLocalSize().X - LabelWidth);
	const int32 ClampedFrame = FMath::Clamp(Frame, 0, Frames - 1);
	const float T = Frames <= 1 ? 0.f : static_cast<float>(ClampedFrame) / static_cast<float>(Frames - 1);
	return LabelWidth + T * TrackW;
}

SMocaraTimeline::FKeyHit SMocaraTimeline::HitKey(const FGeometry& Geo, const FVector2D& Local) const
{
	if (!Keys)
	{
		return {};
	}
	const TArray<FLane> Lanes = BuildLanes();
	const int32 LaneIndex = FMath::FloorToInt((Local.Y - RulerHeight) / LaneHeight);
	if (!Lanes.IsValidIndex(LaneIndex))
	{
		return {};
	}
	const float LaneY = RulerHeight + LaneIndex * LaneHeight;
	for (int32 Index = Keys->Num() - 1; Index >= 0; --Index)
	{
		const FMocaraPoseKey& Key = (*Keys)[Index];
		if (!KeyBelongsToLane(Key, Lanes[LaneIndex]))
		{
			continue;
		}
		const float EaseInX = XFromFrame(Geo, Key.Frame - FMath::Max(0, Key.EaseInFrames));
		const float HoldStartX = XFromFrame(Geo, Key.Frame);
		const float HoldEndX = XFromFrame(Geo, Key.Frame + FMath::Max(1, Key.HoldFrames));
		const float EaseOutX = XFromFrame(
			Geo, Key.Frame + FMath::Max(1, Key.HoldFrames) + FMath::Max(0, Key.EaseOutFrames));
		const bool bNearLowerHandles = FMath::Abs(Local.Y - (LaneY + 16.f)) <= 6.f;
		if (bNearLowerHandles)
		{
			const float EaseInDistance = FMath::Abs(Local.X - EaseInX);
			const float EaseOutDistance = FMath::Abs(Local.X - EaseOutX);
			if (FMath::Min(EaseInDistance, EaseOutDistance) <= 6.f)
			{
				return {Index, EaseInDistance <= EaseOutDistance
					? EMocaraPoseIntervalHandle::EaseInStart
					: EMocaraPoseIntervalHandle::EaseOutEnd};
			}
		}
		if (FMath::Abs(Local.Y - (LaneY + 5.f)) <= 6.f && FMath::Abs(Local.X - HoldEndX) <= 6.f)
		{
			return {Index, EMocaraPoseIntervalHandle::HoldEnd};
		}
		if (FMath::Abs(Local.Y - (LaneY + 10.f)) <= 8.f
			&& Local.X >= FMath::Min(EaseInX, EaseOutX) - 3.f
			&& Local.X <= FMath::Max(EaseInX, EaseOutX) + 3.f)
		{
			return {Index, EMocaraPoseIntervalHandle::Move};
		}
	}
	return {};
}

void SMocaraTimeline::ApplyClick(const FGeometry& Geo, const FVector2D& Local)
{
	const int32 KeyIndex = HitKey(Geo, Local).KeyIndex;
	if (KeyIndex != INDEX_NONE)
	{
		OnKeySelected.ExecuteIfBound(KeyIndex);
		if (Keys && Keys->IsValidIndex(KeyIndex))
		{
			OnPlayheadChanged.ExecuteIfBound((*Keys)[KeyIndex].Frame);
		}
		return;
	}
	OnPlayheadChanged.ExecuteIfBound(FrameFromX(Geo, Local.X));
	OnKeySelected.ExecuteIfBound(INDEX_NONE);
}

void SMocaraTimeline::UpdateHoveredHit(const FKeyHit& Hit)
{
	if (HoveredKeyIndex == Hit.KeyIndex && HoveredHandle == Hit.Handle)
	{
		return;
	}
	HoveredKeyIndex = Hit.KeyIndex;
	HoveredHandle = Hit.Handle;
	Invalidate(EInvalidateWidgetReason::Paint);
}

void SMocaraTimeline::FinishDrag()
{
	if (bDragTransactionStarted)
	{
		OnKeyMoveEnded.ExecuteIfBound();
	}
	bDragging = false;
	bDragTransactionStarted = false;
	DraggedKeyIndex = INDEX_NONE;
	DraggedHandle = EMocaraPoseIntervalHandle::Move;
	DragFrameOffset = 0;
}

int32 SMocaraTimeline::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FSlateBrush* White = FAppStyle::GetBrush("WhiteBrush");
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 8);
	const TArray<FLane> Lanes = BuildLanes();
	const int32 Frames = FMath::Max(1, NumFrames.Get());
	const FVector2f Size = AllottedGeometry.GetLocalSize();

	FSlateDrawElement::MakeBox(
		OutDrawElements,
		LayerId,
		AllottedGeometry.ToPaintGeometry(),
		White,
		ESlateDrawEffect::None,
		FLinearColor(0.04f, 0.04f, 0.04f, 1.f));

	const int32 TickEvery = Frames > 120 ? 10 : (Frames > 60 ? 5 : 1);
	for (int32 Frame = 0; Frame < Frames; Frame += TickEvery)
	{
		const float X = XFromFrame(AllottedGeometry, Frame);
		TArray<FVector2D> Line = {FVector2D(X, 0.f), FVector2D(X, RulerHeight)};
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(), Line, ESlateDrawEffect::None, FLinearColor(0.25f, 0.25f, 0.25f), false, 1.f);
		if (Frame % (TickEvery * 2) == 0)
		{
			FSlateDrawElement::MakeText(
				OutDrawElements,
				LayerId + 2,
				AllottedGeometry.ToPaintGeometry(FVector2f(28.f, 14.f), FSlateLayoutTransform(FVector2f(X + 2.f, 2.f))),
				FString::FromInt(Frame),
				Font,
				ESlateDrawEffect::None,
				FLinearColor(0.7f, 0.7f, 0.7f));
		}
	}

	for (int32 LaneIndex = 0; LaneIndex < Lanes.Num(); ++LaneIndex)
	{
		const float Y = RulerHeight + LaneIndex * LaneHeight;
		const FLinearColor Row = (LaneIndex % 2) ? FLinearColor(0.07f, 0.07f, 0.07f) : FLinearColor(0.09f, 0.09f, 0.09f);
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			LayerId + 1,
			AllottedGeometry.ToPaintGeometry(FVector2f(Size.X, LaneHeight - 1.f), FSlateLayoutTransform(FVector2f(0.f, Y))),
			White,
			ESlateDrawEffect::None,
			Row);

		FSlateDrawElement::MakeText(
			OutDrawElements,
			LayerId + 2,
			AllottedGeometry.ToPaintGeometry(FVector2f(LabelWidth - 4.f, LaneHeight), FSlateLayoutTransform(FVector2f(4.f, Y + 4.f))),
			Lanes[LaneIndex].Label,
			Font,
			ESlateDrawEffect::None,
			FLinearColor(0.85f, 0.85f, 0.85f));

		if (Lanes[LaneIndex].bClipBar)
		{
			const float X0 = XFromFrame(AllottedGeometry, 0);
			const float X1 = XFromFrame(AllottedGeometry, Frames - 1);
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId + 2,
				AllottedGeometry.ToPaintGeometry(FVector2f(FMath::Max(8.f, X1 - X0), 10.f), FSlateLayoutTransform(FVector2f(X0, Y + 6.f))),
				White,
				ESlateDrawEffect::None,
				FLinearColor(0.15f, 0.55f, 0.22f));
		}

		if (Keys)
		{
			for (int32 Index = 0; Index < Keys->Num(); ++Index)
			{
				const FMocaraPoseKey& Key = (*Keys)[Index];
				if (!KeyBelongsToLane(Key, Lanes[LaneIndex]))
				{
					continue;
				}
				const int32 HoldBoundaryFrame = Key.Frame + FMath::Max(1, Key.HoldFrames);
				const int32 EaseOutEndFrame = HoldBoundaryFrame + FMath::Max(0, Key.EaseOutFrames);
				const float EaseInX = XFromFrame(AllottedGeometry, Key.Frame - FMath::Max(0, Key.EaseInFrames));
				const float HoldStartX = XFromFrame(AllottedGeometry, Key.Frame);
				const float HoldEndX = XFromFrame(AllottedGeometry, HoldBoundaryFrame);
				const float EaseOutX = XFromFrame(AllottedGeometry, EaseOutEndFrame);
				const bool bSel = SelectedKey.Get() == Index;
				const bool bHovered = HoveredKeyIndex == Index;
				const FLinearColor EaseColor = bSel
					? FLinearColor(0.95f, 0.82f, 0.2f, 0.38f)
					: FLinearColor(0.2f, 0.85f, 0.35f, 0.32f);
				const FLinearColor HoldColor = bSel
					? FLinearColor(0.95f, 0.82f, 0.2f, 0.95f)
					: FLinearColor(0.2f, 0.85f, 0.35f, 0.9f);
				if (Key.EaseInFrames > 0)
				{
					FSlateDrawElement::MakeBox(
						OutDrawElements,
						LayerId + 3,
						AllottedGeometry.ToPaintGeometry(
							FVector2f(FMath::Max(1.f, HoldStartX - EaseInX), 6.f),
							FSlateLayoutTransform(FVector2f(EaseInX, Y + 8.f))),
						White,
						ESlateDrawEffect::None,
						EaseColor);
				}
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					LayerId + 3,
					AllottedGeometry.ToPaintGeometry(
						FVector2f(FMath::Max(6.f, HoldEndX - HoldStartX), 10.f),
						FSlateLayoutTransform(FVector2f(HoldStartX, Y + 6.f))),
					White,
					ESlateDrawEffect::None,
					HoldColor);
				if (Key.EaseOutFrames > 0)
				{
					FSlateDrawElement::MakeBox(
						OutDrawElements,
						LayerId + 3,
						AllottedGeometry.ToPaintGeometry(
							FVector2f(FMath::Max(1.f, EaseOutX - HoldEndX), 6.f),
							FSlateLayoutTransform(FVector2f(HoldEndX, Y + 8.f))),
						White,
						ESlateDrawEffect::None,
						EaseColor);
				}
				TArray<FVector2D> KeyMarker = {
					FVector2D(HoldStartX, Y + 4.f), FVector2D(HoldStartX, Y + LaneHeight - 5.f)};
				FSlateDrawElement::MakeLines(
					OutDrawElements, LayerId + 4, AllottedGeometry.ToPaintGeometry(), KeyMarker,
					ESlateDrawEffect::None, HoldColor, false, bSel ? 2.f : 1.f);
				const FLinearColor HandleColor = bSel || bHovered
					? FLinearColor(1.f, 0.9f, 0.35f)
					: FLinearColor(0.72f, 0.95f, 0.78f);
				for (const TPair<FVector2f, EMocaraPoseIntervalHandle>& Handle : {
					TPair<FVector2f, EMocaraPoseIntervalHandle>(FVector2f(EaseInX - 4.f, Y + 12.f), EMocaraPoseIntervalHandle::EaseInStart),
					TPair<FVector2f, EMocaraPoseIntervalHandle>(FVector2f(HoldEndX - 4.f, Y + 1.f), EMocaraPoseIntervalHandle::HoldEnd),
					TPair<FVector2f, EMocaraPoseIntervalHandle>(FVector2f(EaseOutX - 4.f, Y + 12.f), EMocaraPoseIntervalHandle::EaseOutEnd)})
				{
					const bool bHoveredHandle = bHovered && HoveredHandle == Handle.Value;
					FSlateDrawElement::MakeBox(
						OutDrawElements,
						LayerId + 5,
						AllottedGeometry.ToPaintGeometry(
							FVector2f(8.f, 8.f), FSlateLayoutTransform(Handle.Key)),
						White,
						ESlateDrawEffect::None,
						bHoveredHandle ? FLinearColor::White : HandleColor);
				}
			}
		}
	}

	const float PlayX = XFromFrame(AllottedGeometry, Playhead.Get());
	TArray<FVector2D> Play = {FVector2D(PlayX, 0.f), FVector2D(PlayX, Size.Y)};
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 4, AllottedGeometry.ToPaintGeometry(), Play, ESlateDrawEffect::None, FLinearColor(0.9f, 0.15f, 0.12f), false, 1.5f);
	return LayerId + 6;
}

FReply SMocaraTimeline::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	bDragging = true;
	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const FKeyHit Hit = HitKey(MyGeometry, Local);
	UpdateHoveredHit(Hit);
	DraggedKeyIndex = Hit.KeyIndex;
	DraggedHandle = Hit.Handle;
	DragFrameOffset = DraggedKeyIndex != INDEX_NONE
		&& DraggedHandle == EMocaraPoseIntervalHandle::Move
		&& Keys && Keys->IsValidIndex(DraggedKeyIndex)
		? FrameFromX(MyGeometry, Local.X) - (*Keys)[DraggedKeyIndex].Frame
		: 0;
	ApplyClick(MyGeometry, Local);
	return FReply::Handled()
		.CaptureMouse(SharedThis(this))
		.SetUserFocus(SharedThis(this), EFocusCause::Mouse);
}

FReply SMocaraTimeline::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	if (!bDragging || !HasMouseCapture())
	{
		UpdateHoveredHit(HitKey(MyGeometry, Local));
		return FReply::Unhandled();
	}
	const int32 Frame = FrameFromX(MyGeometry, Local.X);
	if (DraggedKeyIndex != INDEX_NONE && DraggedHandle == EMocaraPoseIntervalHandle::Move && OnKeyMoved.IsBound())
	{
		const int32 NewFrame = FMath::Clamp(Frame - DragFrameOffset, 0, FMath::Max(0, NumFrames.Get() - 1));
		if (Keys && Keys->IsValidIndex(DraggedKeyIndex) && (*Keys)[DraggedKeyIndex].Frame != NewFrame)
		{
			if (!bDragTransactionStarted)
			{
				OnKeyMoveStarted.ExecuteIfBound();
				bDragTransactionStarted = true;
			}
			DraggedKeyIndex = OnKeyMoved.Execute(DraggedKeyIndex, NewFrame);
		}
	}
	else if (DraggedKeyIndex != INDEX_NONE && OnKeyResized.IsBound())
	{
		bool bWouldChange = false;
		if (Keys && Keys->IsValidIndex(DraggedKeyIndex))
		{
			FMocaraPoseKey Resized = (*Keys)[DraggedKeyIndex];
			FMocaraPoseEditing::ResizeInterval(Resized, DraggedHandle, Frame);
			const FMocaraPoseKey& Current = (*Keys)[DraggedKeyIndex];
			bWouldChange = Resized.EaseInFrames != Current.EaseInFrames
				|| Resized.HoldFrames != Current.HoldFrames
				|| Resized.EaseOutFrames != Current.EaseOutFrames;
		}
		if (bWouldChange)
		{
			if (!bDragTransactionStarted)
			{
				OnKeyMoveStarted.ExecuteIfBound();
				bDragTransactionStarted = true;
			}
			DraggedKeyIndex = OnKeyResized.Execute(DraggedKeyIndex, DraggedHandle, Frame);
		}
	}
	else
	{
		OnPlayheadChanged.ExecuteIfBound(Frame);
	}
	return FReply::Handled();
}

FReply SMocaraTimeline::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	FinishDrag();
	return FReply::Handled().ReleaseMouseCapture();
}

void SMocaraTimeline::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	FinishDrag();
	SLeafWidget::OnMouseCaptureLost(CaptureLostEvent);
}

void SMocaraTimeline::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	if (!bDragging)
	{
		UpdateHoveredHit({});
	}
	SLeafWidget::OnMouseLeave(MouseEvent);
}

FCursorReply SMocaraTimeline::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
	const FKeyHit Hit = HitKey(
		MyGeometry, MyGeometry.AbsoluteToLocal(CursorEvent.GetScreenSpacePosition()));
	if (Hit.KeyIndex == INDEX_NONE)
	{
		return FCursorReply::Unhandled();
	}
	return FCursorReply::Cursor(Hit.Handle == EMocaraPoseIntervalHandle::Move
		? EMouseCursor::GrabHand
		: EMouseCursor::ResizeLeftRight);
}
