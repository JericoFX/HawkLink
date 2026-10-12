# Diagnostic of the rejected IPv6 /0. The single rule stays disabled throughout.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$output = 'F:\HawkenServer\implementation\evidence\local-firewall-prefix-probe'
$program = 'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe'
$ruleName = 'HawkenPrefixProbe-' + [Guid]::NewGuid().ToString('N')
$addresses = @('0.0.0.0-126.255.255.255', '128.0.0.0-255.255.255.255', '::/1', '8000::/1')
$result = [ordered]@{ schema = 'hawken.firewall-prefix-probe.v1'; ruleName = $ruleName; requestedAddresses = $addresses; accepted = $false; cleanupVerified = $false }
$ownedOutput = $false
try {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator token required.' }
    if (Test-Path -LiteralPath $output) { throw 'Output already exists.' }
    $null = New-Item -ItemType Directory -Path $output
    $ownedOutput = $true
    $null = New-NetFirewallRule -Name $ruleName -DisplayName $ruleName -Direction Outbound -Action Block `
        -Program $program -Protocol Any -RemoteAddress $addresses -Profile Any -Enabled False
    $rule = @(Get-NetFirewallRule -Name $ruleName -PolicyStore ActiveStore)
    if ($rule.Count -ne 1 -or $rule[0].Enabled -ne 'False') { throw 'Disabled rule not observed.' }
    $filter = $rule[0] | Get-NetFirewallAddressFilter
    $result.observedAddresses = @($filter.RemoteAddress)
    $result.disabled = $rule[0].Enabled -eq 'False'
    $result.accepted = @(Compare-Object ($addresses | Sort-Object) ($filter.RemoteAddress | Sort-Object)).Count -eq 0
} catch {
    $result.errorId = $_.FullyQualifiedErrorId
    $result.errorMessage = $_.Exception.Message
} finally {
    if ($ownedOutput) {
        $existing = @(Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0) { Remove-NetFirewallRule -Name $ruleName }
        $result.cleanupVerified = @(Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue).Count -eq 0
        $json = $result | ConvertTo-Json -Depth 5
        [IO.File]::WriteAllText((Join-Path $output 'result.json'), $json, [Text.UTF8Encoding]::new($false))
    }
}
if (-not $result.accepted -or -not $result.cleanupVerified) { exit 1 }
exit 0
