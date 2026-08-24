param(
    [Parameter(Mandatory = $true)]
    [string]$CsvPath,

    [double]$MaxTargetDriftDegrees = 10.0,

    [double]$MinTargetForwardToSideRatio = 1.25,

    [double]$MaxArmSegmentDirectionErrorDegrees = 5.0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Numerics

$resolvedCsv = (Resolve-Path -LiteralPath $CsvPath).Path
$rows = @(Import-Csv -LiteralPath $resolvedCsv)
if ($rows.Count -eq 0) {
    throw "Pose CSV is empty: $resolvedCsv"
}

$poseLookup = @{}
foreach ($row in $rows) {
    $poseLookup["$($row.skeleton)|$($row.frame)|$($row.bone)"] = $row
}

function Get-PoseVector {
    param(
        [object[]]$PoseRows,
        [string]$Skeleton,
        [int]$Frame,
        [string]$Bone
    )

    $key = "$Skeleton|$Frame|$Bone"
    if (-not $poseLookup.ContainsKey($key)) {
        throw "Missing $Skeleton/$Frame/$Bone row in $resolvedCsv."
    }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    return [System.Numerics.Vector3]::new(
        [single]::Parse($poseLookup[$key].x, $culture),
        [single]::Parse($poseLookup[$key].y, $culture),
        [single]::Parse($poseLookup[$key].z, $culture))
}

function Get-PlanarAngleDegrees {
    param(
        [System.Numerics.Vector3]$A,
        [System.Numerics.Vector3]$B
    )

    $aLength = [Math]::Sqrt($A.X * $A.X + $A.Y * $A.Y)
    $bLength = [Math]::Sqrt($B.X * $B.X + $B.Y * $B.Y)
    if ($aLength -lt 0.001 -or $bLength -lt 0.001) {
        throw 'Cannot measure heading from a near-zero planar vector.'
    }

    $dot = ($A.X * $B.X + $A.Y * $B.Y) / ($aLength * $bLength)
    $dot = [Math]::Max(-1.0, [Math]::Min(1.0, $dot))
    return [Math]::Acos($dot) * 180.0 / [Math]::PI
}

function Get-SpatialAngleDegrees {
    param(
        [System.Numerics.Vector3]$A,
        [System.Numerics.Vector3]$B
    )

    if ($A.LengthSquared() -lt 0.0001 -or $B.LengthSquared() -lt 0.0001) {
        throw 'Cannot measure direction from a near-zero segment.'
    }
    $dot = [System.Numerics.Vector3]::Dot(
        [System.Numerics.Vector3]::Normalize($A),
        [System.Numerics.Vector3]::Normalize($B))
    $dot = [Math]::Max(-1.0, [Math]::Min(1.0, $dot))
    return [Math]::Acos($dot) * 180.0 / [Math]::PI
}

function Measure-SkeletonHeading {
    param(
        [object[]]$PoseRows,
        [string]$Skeleton,
        [string]$RootBone,
        [string]$SpineBone,
        [string]$LeftHipBone,
        [string]$RightHipBone,
        [string]$LeftFootBone,
        [string]$RightFootBone
    )

    $frames = @($PoseRows |
        Where-Object { $_.skeleton -eq $Skeleton } |
        ForEach-Object { [int]$_.frame } |
        Sort-Object -Unique)
    if ($frames.Count -lt 2) {
        throw "Skeleton '$Skeleton' does not contain at least two frames."
    }

    $firstFrame = $frames[0]
    $lastFrame = $frames[-1]

    $root = Get-PoseVector $PoseRows $Skeleton $firstFrame $RootBone
    $spine = Get-PoseVector $PoseRows $Skeleton $firstFrame $SpineBone
    $leftHip = Get-PoseVector $PoseRows $Skeleton $firstFrame $LeftHipBone
    $rightHip = Get-PoseVector $PoseRows $Skeleton $firstFrame $RightHipBone

    # Right x Up is the skeleton's forward normal. This deliberately measures facing
    # independently of root translation, which is the mismatch the regression guards.
    $right = $rightHip - $leftHip
    $up = $spine - $root
    $bodyForward = [System.Numerics.Vector3]::Cross($right, $up)

    $firstFeet = ((Get-PoseVector $PoseRows $Skeleton $firstFrame $LeftFootBone) +
        (Get-PoseVector $PoseRows $Skeleton $firstFrame $RightFootBone)) / 2.0
    $lastFeet = ((Get-PoseVector $PoseRows $Skeleton $lastFrame $LeftFootBone) +
        (Get-PoseVector $PoseRows $Skeleton $lastFrame $RightFootBone)) / 2.0
    $footTravel = $lastFeet - $firstFeet

    return Get-PlanarAngleDegrees $bodyForward $footTravel
}

function Measure-LimbSwing {
    param(
        [object[]]$PoseRows,
        [string]$Skeleton,
        [string]$RootBone,
        [string]$SpineBone,
        [string]$LeftHipBone,
        [string]$RightHipBone,
        [string[]]$LimbBones
    )

    $frames = @($PoseRows |
        Where-Object { $_.skeleton -eq $Skeleton } |
        ForEach-Object { [int]$_.frame } |
        Sort-Object -Unique)

    $measurements = @()
    foreach ($limbBone in $LimbBones) {
        $forwardSamples = @()
        $sideSamples = @()

        foreach ($frame in $frames) {
            $root = Get-PoseVector $PoseRows $Skeleton $frame $RootBone
            $up = (Get-PoseVector $PoseRows $Skeleton $frame $SpineBone) - $root
            $right = (Get-PoseVector $PoseRows $Skeleton $frame $RightHipBone) -
                (Get-PoseVector $PoseRows $Skeleton $frame $LeftHipBone)

            $forward = [System.Numerics.Vector3]::Cross($right, $up)
            $forward.Z = 0.0
            if ($forward.LengthSquared() -lt 0.0001) {
                throw "Cannot measure $Skeleton/$limbBone swing from a degenerate torso basis at frame $frame."
            }
            $forward = [System.Numerics.Vector3]::Normalize($forward)
            $side = [System.Numerics.Vector3]::new(-$forward.Y, $forward.X, 0.0)
            $relativeLimb = (Get-PoseVector $PoseRows $Skeleton $frame $limbBone) - $root

            $forwardSamples += [double][System.Numerics.Vector3]::Dot($relativeLimb, $forward)
            $sideSamples += [double][System.Numerics.Vector3]::Dot($relativeLimb, $side)
        }

        $forwardSpan = ($forwardSamples | Measure-Object -Maximum).Maximum -
            ($forwardSamples | Measure-Object -Minimum).Minimum
        $sideSpan = ($sideSamples | Measure-Object -Maximum).Maximum -
            ($sideSamples | Measure-Object -Minimum).Minimum
        $ratio = $forwardSpan / [Math]::Max($sideSpan, 0.001)

        $measurements += [pscustomobject]@{
            Bone = $limbBone
            ForwardSpan = $forwardSpan
            SideSpan = $sideSpan
            ForwardToSideRatio = $ratio
        }
    }

    return $measurements
}

function Measure-ArmSegmentDirection {
    param(
        [object[]]$PoseRows,
        [string]$SourceBone,
        [string]$SourceChildBone,
        [string]$TargetBone,
        [string]$TargetChildBone
    )

    $frames = @($PoseRows |
        Where-Object { $_.skeleton -eq 'soma' } |
        ForEach-Object { [int]$_.frame } |
        Sort-Object -Unique)
    $errors = @()
    foreach ($frame in $frames) {
        $sourceDirection = (Get-PoseVector $PoseRows 'soma' $frame $SourceChildBone) -
            (Get-PoseVector $PoseRows 'soma' $frame $SourceBone)
        # UE5 mannequin motion is +90 degrees around Z from the imported SOMA basis.
        $expectedTargetDirection = [System.Numerics.Vector3]::new(
            -$sourceDirection.Y, $sourceDirection.X, $sourceDirection.Z)
        $targetDirection = (Get-PoseVector $PoseRows 'manny' $frame $TargetChildBone) -
            (Get-PoseVector $PoseRows 'manny' $frame $TargetBone)
        $errors += Get-SpatialAngleDegrees $expectedTargetDirection $targetDirection
    }

    return [pscustomobject]@{
        Segment = "$TargetBone->$TargetChildBone"
        MeanError = ($errors | Measure-Object -Average).Average
        MaxError = ($errors | Measure-Object -Maximum).Maximum
    }
}

$sourceError = Measure-SkeletonHeading $rows 'soma' 'Hips' 'Chest' `
    'LeftLeg' 'RightLeg' 'LeftFoot' 'RightFoot'
$targetError = Measure-SkeletonHeading $rows 'manny' 'pelvis' 'spine_05' `
    'thigh_l' 'thigh_r' 'foot_l' 'foot_r'
$targetDrift = [Math]::Max(0.0, $targetError - $sourceError)
$sourceSwing = @(Measure-LimbSwing $rows 'soma' 'Hips' 'Chest' `
    'LeftLeg' 'RightLeg' @('LeftFoot', 'RightFoot', 'LeftHand', 'RightHand'))
$targetSwing = @(Measure-LimbSwing $rows 'manny' 'pelvis' 'spine_05' `
    'thigh_l' 'thigh_r' @('foot_l', 'foot_r', 'hand_l', 'hand_r'))
$armSegments = @(
    Measure-ArmSegmentDirection $rows 'LeftArm' 'LeftForeArm' 'upperarm_l' 'lowerarm_l'
    Measure-ArmSegmentDirection $rows 'LeftForeArm' 'LeftHand' 'lowerarm_l' 'hand_l'
    Measure-ArmSegmentDirection $rows 'RightArm' 'RightForeArm' 'upperarm_r' 'lowerarm_r'
    Measure-ArmSegmentDirection $rows 'RightForeArm' 'RightHand' 'lowerarm_r' 'hand_r'
)

Write-Output ('source_heading_error_deg={0:F2}' -f $sourceError)
Write-Output ('target_heading_error_deg={0:F2}' -f $targetError)
Write-Output ('target_added_drift_deg={0:F2}' -f $targetDrift)
for ($index = 0; $index -lt $targetSwing.Count; ++$index) {
    Write-Output ('limb_swing {0}->{1} source_forward_to_side={2:F2} target_forward_to_side={3:F2}' -f `
        $sourceSwing[$index].Bone,
        $targetSwing[$index].Bone,
        $sourceSwing[$index].ForwardToSideRatio,
        $targetSwing[$index].ForwardToSideRatio)
}
foreach ($segment in $armSegments) {
    Write-Output ('arm_segment {0} mean_direction_error_deg={1:F2} max_direction_error_deg={2:F2}' -f `
        $segment.Segment, $segment.MeanError, $segment.MaxError)
}

if ($targetDrift -gt $MaxTargetDriftDegrees) {
    throw ('Retarget added {0:F2} degrees of heading error; maximum allowed is {1:F2}.' -f `
        $targetDrift, $MaxTargetDriftDegrees)
}

$sidewaysLimbs = @($targetSwing | Where-Object {
    $_.ForwardToSideRatio -lt $MinTargetForwardToSideRatio
})
if ($sidewaysLimbs.Count -gt 0) {
    $details = ($sidewaysLimbs | ForEach-Object {
        '{0}={1:F2}' -f $_.Bone, $_.ForwardToSideRatio
    }) -join ', '
    throw ('Retargeted limbs swing sideways instead of forward ({0}); minimum forward/side ratio is {1:F2}.' -f `
        $details, $MinTargetForwardToSideRatio)
}

$misdirectedArmSegments = @($armSegments | Where-Object {
    $_.MeanError -gt $MaxArmSegmentDirectionErrorDegrees
})
if ($misdirectedArmSegments.Count -gt 0) {
    $details = ($misdirectedArmSegments | ForEach-Object {
        '{0}={1:F2}deg' -f $_.Segment, $_.MeanError
    }) -join ', '
    throw ('Retargeted arm segments do not follow the source bend plane ({0}); maximum mean error is {1:F2} degrees.' -f `
        $details, $MaxArmSegmentDirectionErrorDegrees)
}
