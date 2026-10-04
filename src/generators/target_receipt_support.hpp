#pragma once

namespace sighmake {
// Shared by the Visual Studio and Makefile receipt writers. Native build
// environments need not expose the module which provides Get-FileHash.
inline constexpr const char* target_receipt_file_record_powershell = R"PS(function New-FileRecord([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "target receipt input is missing: $Path"
    }
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        return [ordered]@{
            Size = [uint64]$stream.Length
            SHA256 = [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        }
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $hash.Dispose()
    }
}

)PS";
}
