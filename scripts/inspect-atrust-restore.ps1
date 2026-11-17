param(
    [string]$LogPath = (Join-Path $env:APPDATA 'Sangfor/aTrust/logs'),
    [string]$OutputFile = (Join-Path $env:TEMP 'EZ4Connectpro-atrust-restore-check.json')
)

$ErrorActionPreference = 'Stop'
$source = Get-Item -LiteralPath $LogPath
$files = @(if ($source.PSIsContainer) {
    Get-ChildItem -LiteralPath $source.FullName -File -Filter '*aTrustCore*.log' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 3
} else { $source })
if (-not $files) { throw 'No core logs found. Pass -LogPath with the readable log exported by the official client.' }

# Only an allowlist of event names, timestamps and numeric status is exported.
# Never copy original lines: official logs may contain reusable credentials.
$patterns = [ordered]@{
    token_exchange = 'acquirePassportTokenPromise|/passport/v1/security/passportTokenExchange'
    token_online = 'acquireOnlinePromiseV2|/passport/v1/auth/passportTokenOnline'
    restore_context = 'PassportTokenBusinessHandlerImpl::initOnlineContext'
    restore_environment = 'PassportTokenBusinessHandlerImpl::handleReportEnvBeforeLoginResult'
    restore_online_result = 'PassportTokenBusinessHandlerImpl::handleAcquireOnlineResult'
    restore_complete = 'passport token online fini[s]?[h]?ed|passport token online finised'
    token_present = 'passport token of sdpc entity not empty'
    token_missing = 'passport token of sdpc entity is empty'
    token_offline = '/passport/v1/public/passportTokenOffline'
    explicit_logout = '/passport/v1/user/logout'
    auth_config = '/passport/v1/public/authConfig'
    browser_handoff = '/passport/v1/public/sessionIdExchange|/passport/v1/public/ticketExchange'
}
$events = [Collections.Generic.List[object]]::new()
foreach ($file in $files) {
    if ($file.Length -gt 32MB) { throw 'Log exceeds the 32 MiB limit. Use a smaller exported log.' }
    $reader = [IO.File]::OpenText($file.FullName)
    try {
        while (($line = $reader.ReadLine()) -ne $null) {
            foreach ($item in $patterns.GetEnumerator()) {
                if ($line -notmatch $item.Value) { continue }
                $stamp = [regex]::Match($line, '\d{4}[-/]\d{2}[-/]\d{2}[ T]\d{2}:\d{2}:\d{2}').Value
                $code = [regex]::Match($line, '(?i)\b(?:code|status)\s*[:=]\s*(\d{1,8})\b')
                $events.Add([pscustomobject]@{
                    time = $stamp; event = $item.Key
                    code = $(if ($code.Success) { $code.Groups[1].Value } else { $null })
                    reports_error = [bool]($line -match '(?i)failed|failure|error')
                })
            }
            if ($events.Count -ge 1000) { break }
        }
    } finally { $reader.Dispose() }
    if ($events.Count -ge 1000) { break }
}
$report = [ordered]@{
    schema = 1; files_checked = $files.Count; events = @($events.ToArray())
    note = 'No original lines, account names, hosts, device identifiers, cookies or tokens are exported. No events can mean encrypted or unmatched logs.'
}
$json = $report | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputFile), $json, [Text.UTF8Encoding]::new($false))
Write-Output ('Sanitized events: ' + $events.Count)
Write-Output ('Report: ' + $OutputFile)
