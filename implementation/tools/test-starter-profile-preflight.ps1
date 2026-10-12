# Synthetic CLI fixtures only. Invalid policies must fail before SQL/Steam startup.
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workspace = 'F:\HawkenServer'
$executable = Join-Path $workspace 'build\windows-msvc\hawken-server.exe'
$profileSource = Join-Path $workspace 'profiles\local-starter-catalogue.json'
$absolute = [IO.Path]::GetFullPath($EvidenceDirectory)
if (-not [IO.Path]::IsPathFullyQualified($EvidenceDirectory) -or
    -not $absolute.StartsWith((Join-Path $workspace 'implementation\evidence\'),
        [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $absolute)) {
    throw 'Fresh workspace evidence directory required.'
}
$null = [IO.Directory]::CreateDirectory($absolute)
$source = [IO.File]::ReadAllText($profileSource)
$cases = @(
    @{ name='empty-schema'; content='{}' },
    @{ name='zero-damage'; content=$source.Replace('"InstantHitDamage": 8,','"InstantHitDamage": 0,') },
    @{ name='string-number'; content=$source.Replace('"HeatPerShot": 2,','"HeatPerShot": "2",') },
    @{ name='duplicate-type'; content=$source.Replace('10000000-0000-0000-0000-000000000002','10000000-0000-0000-0000-000000000001') },
    @{ name='offer-collision'; content=$source.Replace('20000000-0000-0000-0000-000000000001','10000000-0000-0000-0000-000000000001') },
    @{ name='invalid-version'; content=$source.Replace('starter.local2','private invalid version') },
    @{ name='missing-projectile'; content=$source.Replace('"SecondaryProjectile":','"MissingProjectile":') },
    @{ name='projectile-zero-speed'; content=$source.Replace('"Speed": 2500,','"Speed": 0,') },
    @{ name='projectile-string-speed'; content=$source.Replace('"Speed": 2500,','"Speed": "2500",') },
    @{ name='projectile-max-speed'; content=$source.Replace('"MaxSpeed": 2500,','"MaxSpeed": 2499,') },
    @{ name='projectile-radius'; content=$source.Replace('"InnerDamageRadius": 100,','"InnerDamageRadius": 301,') },
    @{ name='projectile-min-damage'; content=$source.Replace('"MinDamage": 20','"MinDamage": 81') },
    @{ name='projectile-negative-acceleration'; content=$source.Replace('"AccelRate": 0,','"AccelRate": -1,') },
    @{ name='projectile-unknown-field'; content=$source.Replace('"Speed": 2500,','"Extra": 1, "Speed": 2500,') },
    @{ name='unknown-field'; content=$source.Replace('"Version":','"UnknownPolicy": true, "Version":') },
    @{ name='oversized'; content=$source + (' ' * 16385) },
    @{ name='without-steam'; content=$source },
    @{ name='relative-path'; content=$source },
    @{ name='duplicate-option'; content=$source },
    @{ name='original-game-path'; content=$source }
)
$results = @()
foreach ($case in $cases) {
    $directory = Join-Path $absolute $case.name
    $null = [IO.Directory]::CreateDirectory($directory)
    $profile = Join-Path $directory 'policy.json'
    $database = Join-Path $directory 'accounts.sqlite'
    $diagnostics = Join-Path $directory 'diagnostics.log'
    $stopFile = Join-Path $directory 'stop.request'
    [IO.File]::WriteAllText($profile, $case.content, [Text.UTF8Encoding]::new($false))
    $arguments = @('--steam-local','--database',$database,'--server-id','profile-fixture',
        '--diagnostics',$diagnostics,'--stop-file',$stopFile,'--starter-profile',$profile)
    $expected = 'Invalid starter profile.'
    switch ($case.name) {
        'without-steam' { $arguments = @('--starter-profile',$profile); $expected = 'Invalid arguments. Use --help.' }
        'relative-path' { $arguments[-1] = 'policy.json'; $expected = 'Invalid arguments. Use --help.' }
        'duplicate-option' { $arguments += @('--starter-profile',$profile); $expected = 'Invalid arguments. Use --help.' }
        'original-game-path' { $arguments[-1] = 'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\StormConsumerSettings.ini'; $expected = 'Invalid arguments. Use --help.' }
    }
    $stdout = Join-Path $directory 'stdout.log'
    $stderr = Join-Path $directory 'stderr.log'
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $workspace `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    if (-not $process.WaitForExit(10000)) { $process.Kill(); $process.WaitForExit(); throw 'CLI fixture did not reject promptly.' }
    $exitCode = $process.ExitCode
    $process.Dispose()
    $errorText = [IO.File]::ReadAllText($stderr).Trim()
    $unchanged = -not (Test-Path -LiteralPath $database) -and
        -not (Test-Path -LiteralPath $diagnostics) -and -not (Test-Path -LiteralPath $stopFile) -and
        [IO.File]::ReadAllText($profile) -ceq $case.content
    $results += [ordered]@{ name=$case.name; exitCode=$exitCode; expectedError=$expected;
        actualError=$errorText; noDataSideEffects=$unchanged;
        passed=($exitCode -eq 2 -and $errorText -ceq $expected -and $unchanged) }
}
$failed = @($results | Where-Object { -not $_.passed }).Count
[ordered]@{ schema='hawken.starter-profile-preflight.v1'; cases=$results.Count; failed=$failed;
    executableSha256=(Get-FileHash -LiteralPath $executable).Hash.ToLowerInvariant();
    profileSha256=(Get-FileHash -LiteralPath $profileSource).Hash.ToLowerInvariant(); results=$results;
    scope='Invalid synthetic policies/CLI shapes only; no legitimate Steam authentication or game execution' } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $absolute 'summary.json') -Encoding utf8NoBOM
Write-Output "Starter profile preflight: $($results.Count) cases, $failed failed."
if ($failed) { exit 1 }
exit 0
