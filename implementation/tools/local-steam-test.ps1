# Ensayo temporal de destino. No inicia Steam ni el juego y no captura sus logs.
# Ejecutar elevado; iniciar Hawken normalmente desde Steam tras ready.json.
param(
    [ValidatePattern('^local-steam-routing-[0-9]{8}-[0-9]{6}$')]
    [string]$SessionName = ('local-steam-routing-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')),
    [switch]$SteamLocalAuth,
    [switch]$StarterInventory,
    [switch]$ShortFtuxTest,
    [switch]$UdpTrial,
    [switch]$WreckageTrial,
    [ValidateRange(1, 65535)]
    [int]$UdpPort = 7777,
    [ValidatePattern('^[A-Za-z0-9_.-]{1,64}$')]
    [string]$UdpRegion = 'EU'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workspace = 'F:\HawkenServer'
$gameDirectory = 'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64'
$gameExecutable = Join-Path $gameDirectory 'HawkenGame-Win64-Shipping.exe'
$masterExecutable = Join-Path $workspace 'build\windows-msvc\hawken-master.exe'
$serverExecutable = Join-Path $workspace 'build\windows-msvc\hawken-server.exe'
$bridgeSource = Join-Path $workspace 'build\windows-msvc\client-bridge\dinput8.dll'
$configSource = Join-Path $workspace 'profiles\hawken-client.toml'
$starterProfileSource = Join-Path $workspace 'profiles\local-starter-catalogue.json'
$starterProfileHash = '528c9a7685ff6686a23c07cb4e62bb4cc7389ae2bd64a8ea1bf5dcd945e996fe'
if ($ShortFtuxTest) {
    $starterProfileSource = Join-Path $workspace 'profiles\local-short-ftux-catalogue.json'
    $starterProfileHash = 'c6811a6c6d7220e8c139e655466ae44064c52d7b6ff30b0bb21e31cfc7947117'
}
$sessionDirectory = Join-Path $workspace ('implementation\evidence\' + $SessionName)
$ruleName = 'HawkenLocalOnly-' + [Guid]::NewGuid().ToString('N')
$remoteAddresses = @('0.0.0.0-126.255.255.255', '128.0.0.0-255.255.255.255', '::/1', '8000::/1')
$addedFiles = @()
$sessionCreated = $false
$ruleCreated = $false
$masterProcess = $null
$serverProcess = $null
$serverLogName = if ($SteamLocalAuth) { 'server.diagnostics.log' } else { 'server.stdout.log' }
$serverStopFile = Join-Path $sessionDirectory 'server.stop'
$udpServerGuid = if ($UdpTrial) { [Guid]::NewGuid().ToString('D') } else { $null }
$phase = 'preflight'
$result = [ordered]@{
    schema = 'hawken.local-steam-routing.v2'
    status = 'NOT_STARTED'
    steamStartedByHelper = $false
    gameStartedByHelper = $false
    gameObserved = $false
    bridgeModuleObserved = $false
    clientLoopbackConnectionObserved = $false
    steamLoginRequestObserved = $false
    credentialsOrClientLogsCaptured = $false
    authenticationVerified = $false
    localSteamAuthRequested = $SteamLocalAuth.IsPresent
    starterInventoryRequested = $StarterInventory.IsPresent
    shortFtuxTestRequested = $ShortFtuxTest.IsPresent
    udpTrialRequested = $UdpTrial.IsPresent
    wreckageTrialRequested = $WreckageTrial.IsPresent
    udpLoopbackBound = $false
    serverGracefulStop = $false
    serverForcedStop = $false
    gameplayVerified = $false
    cleanupComplete = $false
}
$originals = @(
    @{ name = 'HawkenGame-Win64-Shipping.exe'; hash = '4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2' },
    @{ name = 'Meteor.Sdk.Cpp.Win32.Client64.dll'; hash = 'f63fef02ffb517ca7844a769d03412ad82bd22198397302ee3c090c63a970c57' },
    @{ name = 'StormConsumerSettings.ini'; hash = '08eda2cb20539996b7c1a91480c1ee2f64e02743c9bbee0bfc4f3543d8fec3d9' },
    @{ name = 'steam_api64.dll'; hash = '38f6a1dd1e738e142160322638dae7b88dabcc30eea3f15c58eb08c5341f9b29' },
    @{ name = 'steam_appid.txt'; hash = '7f8af64056cc012f81841069a6bb93ad471e5f738a82e05a4981030a6da34233' }
)

function Assert-Hash([string]$Path, [string]$Expected) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {
        throw 'File precondition failed.'
    }
}

function Write-Evidence([string]$Name, $Value) {
    $text = $Value | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText((Join-Path $sessionDirectory $Name), $text, [Text.UTF8Encoding]::new($false))
}

function Get-GameProcesses {
    @(Get-Process -Name 'HawkenGame-Win64-Shipping' -ErrorAction SilentlyContinue)
}

function Add-TestFile([string]$Source, [string]$Name, [string]$Expected) {
    $destination = Join-Path $gameDirectory $Name
    Assert-Hash $Source $Expected
    [IO.File]::Copy($Source, $destination, $false)
    $script:addedFiles += @{ path = $destination; hash = $Expected }
    Assert-Hash $destination $Expected
}

try {
    if ($WreckageTrial -and -not $UdpTrial) {
        throw 'Wreckage trial requires UdpTrial.'
    }
    if (($UdpTrial -and -not $SteamLocalAuth) -or
        (-not $UdpTrial -and ($PSBoundParameters.ContainsKey('UdpPort') -or $PSBoundParameters.ContainsKey('UdpRegion')))) {
        throw 'UDP trial requires local Steam authentication; UDP options require UdpTrial.'
    }
    if ($ShortFtuxTest -and -not ($SteamLocalAuth -and $StarterInventory)) {
        throw 'Short FTUX test requires local Steam authentication and starter inventory.'
    }
    if ($StarterInventory -and -not $SteamLocalAuth) {
        throw 'Starter inventory requires local Steam authentication.'
    }
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator token required.'
    }
    if (@(Get-GameProcesses).Count -ne 0 -or (Test-Path -LiteralPath $sessionDirectory)) {
        throw 'Session precondition failed.'
    }
    foreach ($original in $originals) {
        Assert-Hash (Join-Path $gameDirectory $original.name) $original.hash
    }
    Assert-Hash $masterExecutable 'a54f5163d16f0309ad2f0b829990c046a203fe6d399da9a04eb9251e85e6ffa5'
    Assert-Hash $serverExecutable '437e69de9f99e47820aa2c1c3d76b6d31527bfb5e3c0a382caf94217b83bcbc8'
    if ($StarterInventory) {
        Assert-Hash $starterProfileSource $starterProfileHash
    }
    foreach ($name in @('dinput8.dll', 'hawken-client.toml')) {
        if (Test-Path -LiteralPath (Join-Path $gameDirectory $name)) { throw 'Destination already exists.' }
    }
    $profiles = @(Get-NetFirewallProfile -PolicyStore ActiveStore)
    if ($profiles.Count -ne 3 -or @($profiles | Where-Object { $_.Enabled -ne 'True' }).Count -ne 0 -or
        (Get-Service -Name MpsSvc).Status -ne 'Running') {
        throw 'Active firewall precondition failed.'
    }
    if (@(Get-NetTCPConnection -State Listen -LocalPort 80,18080 -ErrorAction SilentlyContinue).Count -ne 0) {
        throw 'A local test port is already in use.'
    }
    if ($UdpTrial -and @(Get-NetUDPEndpoint -LocalPort $UdpPort -ErrorAction SilentlyContinue).Count -ne 0) {
        throw 'The UDP trial port is already in use.'
    }
    $null = New-Item -ItemType Directory -Path $sessionDirectory
    $sessionCreated = $true
    Write-Evidence 'guard.json' @{ ruleName = $ruleName; gameExecutable = $gameExecutable; remoteAddresses = $remoteAddresses }

    $phase = 'firewall-rule-create'
    $null = New-NetFirewallRule -Name $ruleName -DisplayName $ruleName -Direction Outbound -Action Block `
        -Program $gameExecutable -Protocol Any -RemoteAddress $remoteAddresses -Profile Any -Enabled True
    $ruleCreated = $true
    $phase = 'firewall-rule-query'
    $rule = @(Get-NetFirewallRule -Name $ruleName -PolicyStore ActiveStore)
    if ($rule.Count -ne 1 -or $rule[0].Enabled -ne 'True' -or $rule[0].Direction -ne 'Outbound' -or $rule[0].Action -ne 'Block') {
        throw 'Firewall rule verification failed.'
    }
    $application = $rule[0] | Get-NetFirewallApplicationFilter
    $phase = 'firewall-address-query'
    $address = $rule[0] | Get-NetFirewallAddressFilter
    if ($application.Program -ne $gameExecutable -or
        @(Compare-Object ($remoteAddresses | Sort-Object) ($address.RemoteAddress | Sort-Object)).Count -ne 0) {
        throw 'Firewall filter verification failed.'
    }
    Write-Evidence 'guard-active.json' @{ verified = $true; ruleName = $ruleName; program = $gameExecutable }

    $phase = 'temporary-files'
    Add-TestFile $bridgeSource 'dinput8.dll' '7d90edf5ed672e6c498954f73420c94c4cf324f9ffe7dc9ad98de9cc924263c7'
    Add-TestFile $configSource 'hawken-client.toml' '7f476d02e9c93627b43c5dde3bed7f12646252e71333944a66869498ddb4e4c3'

    $phase = 'master-startup'
    $masterProcess = Start-Process -FilePath $masterExecutable -ArgumentList @('--port', '18080') `
        -WorkingDirectory $workspace -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $sessionDirectory 'master.stdout.log') `
        -RedirectStandardError (Join-Path $sessionDirectory 'master.stderr.log')
    $null = $masterProcess.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        if ($masterProcess.HasExited) { throw 'Master exited before readiness.' }
        $listeners = @(Get-NetTCPConnection -State Listen -OwningProcess $masterProcess.Id -ErrorAction SilentlyContinue)
        if ($listeners.Count -eq 1 -and $listeners[0].LocalAddress -eq '127.0.0.1' -and $listeners[0].LocalPort -eq 18080) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($listeners.Count -ne 1 -or $listeners[0].LocalAddress -ne '127.0.0.1' -or $listeners[0].LocalPort -ne 18080) {
        throw 'Master loopback listener not observed.'
    }
    $phase = 'server-startup'
    $serverArguments = @('--port', '80')
    if ($SteamLocalAuth) {
        $dataDirectory = Join-Path $workspace 'implementation\local-runtime'
        $null = [IO.Directory]::CreateDirectory($dataDirectory)
        if ((Get-Item -LiteralPath $dataDirectory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Local account directory precondition failed.'
        }
        $databasePath = Join-Path $dataDirectory 'accounts.sqlite'
        if ((Test-Path -LiteralPath $databasePath) -and
            ((Get-Item -LiteralPath $databasePath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Local account database precondition failed.'
        }
        $serverArguments += @('--steam-local', '--database', $databasePath,
            '--server-id', 'local-steam-first', '--diagnostics',
            (Join-Path $sessionDirectory $serverLogName), '--stop-file', $serverStopFile)
        if ($StarterInventory) { $serverArguments += @('--starter-profile', $starterProfileSource) }
        if ($UdpTrial) {
            $serverArguments += @('--udp-port', [string]$UdpPort,
                '--udp-server-guid', $udpServerGuid, '--udp-region', $UdpRegion)
            if ($WreckageTrial) {
                $serverArguments += @('--udp-world-root', '"F:\SteamLibrary\steamapps\common\Hawken"')
            }
        }
    }
    $serverProcess = Start-Process -FilePath $serverExecutable -ArgumentList $serverArguments `
        -WorkingDirectory $workspace -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $sessionDirectory 'server.stdout.log') `
        -RedirectStandardError (Join-Path $sessionDirectory 'server.stderr.log')
    $null = $serverProcess.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds($(if ($SteamLocalAuth) { 45 } else { 10 }))
    do {
        if ($serverProcess.HasExited) { throw 'Server exited before readiness.' }
        # Steam also opens an owned loopback listener. Readiness concerns the
        # backend port specifically; the independent listener is not HTTP/80.
        $listeners = @(Get-NetTCPConnection -State Listen -LocalPort 80 -OwningProcess $serverProcess.Id -ErrorAction SilentlyContinue)
        if ($listeners.Count -eq 1 -and $listeners[0].LocalAddress -eq '127.0.0.1' -and $listeners[0].LocalPort -eq 80) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($listeners.Count -ne 1 -or $listeners[0].LocalAddress -ne '127.0.0.1' -or $listeners[0].LocalPort -ne 80) {
        throw 'Server loopback listener not observed.'
    }
    if ($UdpTrial) {
        $udpListeners = @(Get-NetUDPEndpoint -LocalPort $UdpPort -OwningProcess $serverProcess.Id -ErrorAction SilentlyContinue)
        if ($udpListeners.Count -ne 1 -or $udpListeners[0].LocalAddress -ne '127.0.0.1') {
            throw 'UDP trial loopback listener not observed.'
        }
        $result.udpLoopbackBound = $true
        Write-Evidence 'udp-ready.json' @{ port = $UdpPort; region = $UdpRegion; serverGuid = $udpServerGuid; gameplay = $false }
    }
    $phase = 'awaiting-steam-client'
    $result.status = 'READY_FOR_MANUAL_STEAM_START'
    Write-Evidence 'ready.json' @{ status = $result.status; masterPid = $masterProcess.Id; masterPort = 18080; serverPid = $serverProcess.Id; serverPort = 80; ruleName = $ruleName }
    $launchDeadline = [DateTime]::UtcNow.AddMinutes(10)
    while ($true) {
        $games = @(Get-GameProcesses)
        if ($games.Count -gt 0) {
            $result.gameObserved = $true
            foreach ($game in $games) {
                if ($game.HasExited) { continue }
                if ($game.Path -ne $gameExecutable) {
                    if ($game.HasExited) { continue }
                    throw 'Unexpected game path.'
                }
                if (-not $result.clientLoopbackConnectionObserved) {
                    $connections = @(Get-NetTCPConnection -State Established -OwningProcess $game.Id -RemoteAddress '127.0.0.1' -RemotePort 80 -ErrorAction SilentlyContinue)
                    if ($connections.Count -gt 0) {
                        $result.clientLoopbackConnectionObserved = $true
                        Write-Evidence 'client-connection.json' @{ pid = $game.Id; remoteAddress = '127.0.0.1'; remotePort = 80; state = 'Established' }
                    }
                }
                if (-not $result.bridgeModuleObserved) {
                    try {
                        $bridge = @($game.Modules | Where-Object { $_.ModuleName -eq 'dinput8.dll' -and $_.FileName -eq (Join-Path $gameDirectory 'dinput8.dll') })
                        if ($bridge.Count -eq 1) {
                            $result.bridgeModuleObserved = $true
                            Write-Evidence 'client-module.json' @{ pid = $game.Id; image = $gameExecutable; bridge = $bridge[0].FileName }
                        }
                    } catch { # An exiting process can disappear while enumerating its modules.
                    }
                }
            }
        } elseif ($result.gameObserved) {
            $result.status = 'CLIENT_CLOSED'
            break
        }
        if (Test-Path -LiteralPath (Join-Path $sessionDirectory 'stop.request')) {
            $result.status = 'STOP_REQUESTED'
            break
        }
        if (-not $result.gameObserved -and [DateTime]::UtcNow -gt $launchDeadline) {
            $result.status = 'NO_CLIENT_STARTED'
            break
        }
        if ($masterProcess.HasExited) { throw 'Master stopped during test.' }
        if ($serverProcess.HasExited) { throw 'Server stopped during test.' }
        Start-Sleep -Milliseconds 500
    }
} catch {
    $result.status = 'FAILED'
    $result.failurePhase = $phase
    $result.failureType = $_.Exception.GetType().Name
    $result.failureId = $_.FullyQualifiedErrorId
    $result.failureHResult = $_.Exception.HResult
    $result.ruleCreatedBeforeFailure = $ruleCreated
    # These operations receive only the fixed local rule, never account data.
    if ($phase -like 'firewall-*') { $result.firewallDiagnostic = $_.Exception.Message }
} finally {
    # Also find our exact rule after a partially successful creation call.
    if ($sessionCreated -and -not $ruleCreated) {
        $ruleCreated = @(Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue).Count -gt 0
    }
    # Keep the network guard and added files while a Hawken process is alive.
    while ($ruleCreated -and @(Get-GameProcesses).Count -gt 0) { Start-Sleep -Seconds 1 }
    $cleanupOk = $true
    if ($null -ne $serverProcess) {
        if ($SteamLocalAuth -and -not $serverProcess.HasExited) {
            [IO.File]::WriteAllText($serverStopFile, "stop`n", [Text.UTF8Encoding]::new($false))
            $result.serverGracefulStop = $serverProcess.WaitForExit(15000)
        }
        if (-not $serverProcess.HasExited) {
            $serverProcess.Kill()
            $serverProcess.WaitForExit()
            $result.serverStoppedByTestCleanup = $true
            $result.serverForcedStop = $true
        }
        $result.serverExitCode = $serverProcess.ExitCode
        $serverProcess.Dispose()
    }
    if ($null -ne $masterProcess) {
        if (-not $masterProcess.HasExited) {
            $masterProcess.Kill()
            $masterProcess.WaitForExit()
            $result.masterStoppedByTestCleanup = $true
        }
        $masterProcess.Dispose()
    }
    foreach ($serviceName in @('master', 'server')) {
        $logName = if ($serviceName -eq 'server') { $serverLogName } else { 'master.stdout.log' }
        $log = Join-Path $sessionDirectory $logName
        if ($sessionCreated -and (Test-Path -LiteralPath $log)) {
            $categories = @([IO.File]::ReadAllLines($log) | Where-Object { $_ -match '^http status=[0-9]+ category=[a-z]+(?:-[a-z]+)* method=(GET|POST|HEAD|other)$' })
            $result[$serviceName + 'HttpStatusCategories'] = $categories
            if ($serviceName -eq 'server') {
                $result.steamLoginRequestObserved = @($categories | Where-Object { $_ -match ' category=steam-login method=POST$' }).Count -gt 0
            }
        }
    }
    if ($SteamLocalAuth -and $sessionCreated -and
        (Test-Path -LiteralPath (Join-Path $sessionDirectory $serverLogName))) {
        # These records are written by our typed observer only. Native DLL
        # stdout/stderr are discarded before loading Steam in the backend.
        $diagnostics = @([IO.File]::ReadAllLines((Join-Path $sessionDirectory $serverLogName)))
        $beginOk = @($diagnostics | Where-Object { $_ -match '^steam phase=begin .* begin_ok=1 ' }).Count -gt 0
        $callbackOk = @($diagnostics | Where-Object {
            $_ -match '^steam phase=callback143 .* packed20=1 identity_match=1 response_ok=1 '
        }).Count -gt 0
        $licenseOk = @($diagnostics | Where-Object { $_ -match '^steam phase=license .* license_ok=1 ' }).Count -gt 0
        $loginOk = @($diagnostics | Where-Object { $_ -eq 'http status=200 category=steam-login method=POST' }).Count -gt 0
        $shutdownOk = @($diagnostics | Where-Object {
            $_ -match '^steam phase=shutdown .* gs_shutdown=1 gs_pipe_zero=1$'
        }).Count -gt 0
        $result.localSteamObserved = @{
            beginAccepted = $beginOk; callback143PackedMatchedOK = $callbackOk
            licenseApproved = $licenseOk; loginHTTP200 = $loginOk
            ownedContextsShutdownClean = $shutdownOk; privateTicketPersistedOrLogged = $false
        }
        $result.authenticationVerified = $beginOk -and $callbackOk -and $licenseOk -and $loginOk
    }
    foreach ($file in $addedFiles) {
        try {
            $resolved = [IO.Path]::GetFullPath($file.path)
            if ([IO.Path]::GetDirectoryName($resolved) -ne $gameDirectory) { throw 'Cleanup path rejected.' }
            Assert-Hash $resolved $file.hash
            Remove-Item -LiteralPath $resolved
        } catch {
            # Preserve a file whose contents changed after this helper added it.
            $cleanupOk = $false
        }
    }
    if ($ruleCreated) {
        try { Remove-NetFirewallRule -Name $ruleName }
        catch { $cleanupOk = $false }
    }
    if ($sessionCreated) {
        $result.originalsUnchanged = $true
        foreach ($original in $originals) {
            try { Assert-Hash (Join-Path $gameDirectory $original.name) $original.hash }
            catch { $result.originalsUnchanged = $false }
        }
        $result.cleanupComplete = $cleanupOk
        if (-not $cleanupOk -or -not $result.originalsUnchanged) { $result.status = 'FAILED' }
        Write-Evidence 'result.json' $result
    }
}
if ($result.status -eq 'FAILED') { exit 1 }
exit 0
