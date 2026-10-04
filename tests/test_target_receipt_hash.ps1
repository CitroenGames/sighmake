[CmdletBinding()]
param([Parameter(Mandatory=$true)][string[]]$ReceiptScripts)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Simulate a native environment in which the hashing cmdlet cannot be used.
function Get-FileHash { throw 'Receipt writer attempted module-dependent hashing' }
$fixtureRoot = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'sighmake-receipt-' + [Guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($fixtureRoot)
try {
    $primary = [IO.Path]::Combine($fixtureRoot, 'artifact.bin')
    $empty = [IO.Path]::Combine($fixtureRoot, 'empty.bin')
    $metadata = [IO.Path]::Combine($fixtureRoot, 'runtime.txt')
    $receipt = [IO.Path]::Combine($fixtureRoot, 'artifact.targetreceipt.json')
    [IO.File]::WriteAllBytes($primary, [Text.Encoding]::ASCII.GetBytes('abc'))
    [IO.File]::WriteAllBytes($empty, [byte[]]@())
    [IO.File]::WriteAllText($metadata, "Empty|$empty|empty.bin|1")
    $arguments = @{ Metadata=$metadata; Target='Fixture'; Platform='Windows'; Architecture='x64'; Configuration='Release'; PrimaryArtifact=$primary; Output=$receipt }
    foreach ($script in $ReceiptScripts) {
        & $script @arguments
        $result = [IO.File]::ReadAllText($receipt) | ConvertFrom-Json
        if ($result.FormatVersion -ne 1 -or $result.PrimaryArtifactSize -ne 3 -or
            $result.PrimaryArtifactSHA256 -cne 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad' -or
            $result.RuntimeDependencies.Count -ne 1 -or $result.RuntimeDependencies[0].Size -ne 0 -or
            $result.RuntimeDependencies[0].SHA256 -cne 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855') {
            throw "Incorrect authenticated receipt: $script"
        }
        # Exclusive handles prove both hash streams were closed.
        foreach ($path in @($primary, $empty)) {
            $probe = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $probe.Dispose()
        }
        $beforeFailure = [IO.File]::ReadAllText($receipt)
        [IO.File]::WriteAllText($metadata, "Missing|$fixtureRoot/missing.bin|missing.bin|1")
        $failed = $false
        try { & $script @arguments } catch { $failed = $true }
        if (-not $failed -or [IO.File]::ReadAllText($receipt) -cne $beforeFailure) {
            throw "Missing required input did not fail without replacing receipt: $script"
        }
        [IO.File]::WriteAllText($metadata, "Empty|$empty|empty.bin|1")
        $arguments.PrimaryArtifact = "$fixtureRoot/missing-primary.bin"
        $failed = $false
        try { & $script @arguments } catch { $failed = $true }
        if (-not $failed -or [IO.File]::ReadAllText($receipt) -cne $beforeFailure) {
            throw "Missing primary input did not fail without replacing receipt: $script"
        }
        $arguments.PrimaryArtifact = $primary
        Write-Output "PASS: streaming SHA256, empty file, stream disposal and failure integrity: $script"
    }
} finally {
    # This directory is created above with a fresh GUID; it contains only this fixture's files.
    if ([IO.Directory]::Exists($fixtureRoot)) { [IO.Directory]::Delete($fixtureRoot, $true) }
}
