# usage (PowerShell): Scripts/vram.ps1 -Label high -Sg 2   (Sg: scalability 0-3); writes a CSV to %TEMP%
param([string]$Label, [int]$Sg, [string]$Plaza = "A1-G01", [int]$Delay = 90)
# Per-process GPU memory of one -game run: dedicated and shared, sampled every 2 s.
$ue = "X:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
$gameArgs = @("X:/Projects/toll-sim-unreal/TollSimUE.uproject", "-game", "-RenderOffscreen", "-ResX=1920", "-ResY=1080", "-TollReplay",
  "-TollCam=plaza:$Plaza", "-TollTime=15", "-ExecCmds=`"scalability $Sg`"", "-TollShot=$env:TEMP/toll-vram_$Label.png", "-TollShotDelay=$Delay", "-unattended")
$p = Start-Process -FilePath $ue -ArgumentList $gameArgs -PassThru
$t0 = Get-Date
$rows = @()
while (-not $p.HasExited) {
  try {
    $s = Get-Counter -Counter "\GPU Process Memory(pid_$($p.Id)_*)\Dedicated Usage", "\GPU Process Memory(pid_$($p.Id)_*)\Shared Usage" -ErrorAction Stop
    $ded = ($s.CounterSamples | Where-Object Path -like "*dedicated usage" | Measure-Object CookedValue -Sum).Sum / 1MB
    $sh = ($s.CounterSamples | Where-Object Path -like "*shared usage" | Measure-Object CookedValue -Sum).Sum / 1MB
    $rows += [pscustomobject]@{ t = [int]((Get-Date) - $t0).TotalSeconds; ded = [int]$ded; shared = [int]$sh }
  } catch {}
  Start-Sleep -Seconds 2
}
$rows | Export-Csv "C:\tmp\hero\vram2_$Label.csv" -NoTypeInformation
$late = $rows | Where-Object { $_.t -gt 60 }
"{0} sg={1}: peak dedicated {2} MiB, peak shared {3} MiB; after 60 s dedicated {4}-{5} MiB, shared {6}-{7} MiB" -f $Label, $Sg,
  ($rows | Measure-Object ded -Maximum).Maximum, ($rows | Measure-Object shared -Maximum).Maximum,
  ($late | Measure-Object ded -Minimum).Minimum, ($late | Measure-Object ded -Maximum).Maximum,
  ($late | Measure-Object shared -Minimum).Minimum, ($late | Measure-Object shared -Maximum).Maximum
