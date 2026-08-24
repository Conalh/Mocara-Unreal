#pragma once

#include "CoreMinimal.h"
#include "MocaraTypes.h"
#include "Widgets/SLeafWidget.h"

DECLARE_DELEGATE_OneParam(FMocaraFrameChanged, int32);
DECLARE_DELEGATE_OneParam(FMocaraKeySelected, int32);
DECLARE_DELEGATE_RetVal_TwoParams(int32, FMocaraKeyMoved, int32, int32);
DECLARE_DELEGATE_RetVal_ThreeParams(
	int32, FMocaraKeyResized, int32, EMocaraPoseIntervalHandle, int32);

class SMocaraTimeline : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SMocaraTimeline) {}
		SLATE_ARGUMENT(const TArray<FMocaraPoseKey>*, Keys)
		SLATE_ATTRIBUTE(int32, NumFrames)
		SLATE_ATTRIBUTE(int32, Playhead)
		SLATE_ATTRIBUTE(int32, SelectedKey)
		SLATE_EVENT(FMocaraFrameChanged, OnPlayheadChanged)
		SLATE_EVENT(FMocaraKeySelected, OnKeySelected)
		SLATE_EVENT(FMocaraKeyMoved, OnKeyMoved)
		SLATE_EVENT(FMocaraKeyResized, OnKeyResized)
		SLATE_EVENT(FSimpleDelegate, OnKeyMoveStarted)
		SLATE_EVENT(FSimpleDelegate, OnKeyMoveEnded)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;

private:
	struct FLane
	{
		FString Label;
		FName BoneFilter = NAME_None;
		bool bClipBar = false;
		bool bFullBody = false;
	};
	struct FKeyHit
	{
		int32 KeyIndex = INDEX_NONE;
		EMocaraPoseIntervalHandle Handle = EMocaraPoseIntervalHandle::Move;
	};

	TArray<FLane> BuildLanes() const;
	static bool KeyBelongsToLane(const FMocaraPoseKey& Key, const FLane& Lane);
	int32 FrameFromX(const FGeometry& Geo, float LocalX) const;
	float XFromFrame(const FGeometry& Geo, int32 Frame) const;
	FKeyHit HitKey(const FGeometry& Geo, const FVector2D& Local) const;
	void ApplyClick(const FGeometry& Geo, const FVector2D& Local);
	void UpdateHoveredHit(const FKeyHit& Hit);
	void FinishDrag();

	const TArray<FMocaraPoseKey>* Keys = nullptr;
	TAttribute<int32> NumFrames;
	TAttribute<int32> Playhead;
	TAttribute<int32> SelectedKey;
	FMocaraFrameChanged OnPlayheadChanged;
	FMocaraKeySelected OnKeySelected;
	FMocaraKeyMoved OnKeyMoved;
	FMocaraKeyResized OnKeyResized;
	FSimpleDelegate OnKeyMoveStarted;
	FSimpleDelegate OnKeyMoveEnded;
	bool bDragging = false;
	bool bDragTransactionStarted = false;
	int32 DraggedKeyIndex = INDEX_NONE;
	EMocaraPoseIntervalHandle DraggedHandle = EMocaraPoseIntervalHandle::Move;
	int32 DragFrameOffset = 0;
	int32 HoveredKeyIndex = INDEX_NONE;
	EMocaraPoseIntervalHandle HoveredHandle = EMocaraPoseIntervalHandle::Move;

	static constexpr float LabelWidth = 92.f;
	static constexpr float LaneHeight = 22.f;
	static constexpr float RulerHeight = 18.f;
};
