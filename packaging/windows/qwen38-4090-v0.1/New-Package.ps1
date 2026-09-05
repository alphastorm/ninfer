[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerExecutable,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$SourceArchive,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$PatchStackSha,

    [string[]]$RuntimeFile = @(),

    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerConfig = (Join-Path $PSScriptRoot 'server-config.json'),

    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$QualificationRecord = (Join-Path $PSScriptRoot '../../../docs/qualification/qwen3.8-27b-rtx-4090-v0.1.json'),

    [Parameter(Mandatory = $true)]
    [string]$SbomCreatedUtc,

    [switch]$FinalizeExistingAssets,

    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'out')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-JsonFile([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Set-JsonProperty([object]$Object, [string]$Name, [object]$Value) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        Add-Member -InputObject $Object -MemberType NoteProperty -Name $Name -Value $Value
    }
    else {
        $property.Value = $Value
    }
}

function Assert-PublicQualificationDisclosure([object]$Qualification) {
    $privateValue = '(?i)(?:[A-Z]:\\Users\\|/Users/|/home/|(?:nyc|sf)-[a-z0-9-]+|(?:[0-9]{1,3}\.){3}[0-9]{1,3})'
    $forbiddenProperty = '^(?i:gpu_uuid|api_key|api_key_file|controller_path|health_url|private_host)$'
    $pending = [Collections.Generic.Stack[object]]::new()
    $pending.Push($Qualification)
    while ($pending.Count -ne 0) {
        $value = $pending.Pop()
        if ($null -eq $value) { continue }
        if ($value -is [string]) {
            if ([string]$value -match $privateValue) {
                throw 'public qualification disclosure violation'
            }
            continue
        }
        if ($value -is [Collections.IDictionary]) {
            foreach ($key in $value.Keys) {
                if ([string]$key -match $forbiddenProperty) {
                    throw 'public qualification disclosure violation'
                }
                $pending.Push($value[$key])
            }
            continue
        }
        if ($value -is [Collections.IEnumerable]) {
            foreach ($item in $value) { $pending.Push($item) }
            continue
        }
        if ($value -is [ValueType]) { continue }
        foreach ($property in $value.PSObject.Properties) {
            if ([string]$property.Name -match $forbiddenProperty) {
                throw 'public qualification disclosure violation'
            }
            $pending.Push($property.Value)
        }
    }
}

function Assert-NoPublishedPowerShellHooks([string[]]$Paths) {
    $hookPattern = '(?i)(?:\b(?:TestMode|Mock|Fixture|Fault|Bypass|Harness)\b|\bInject(?:ed|ion)?\b|\bSimulat(?:e|ed|ion)\b|test[_-]?mode|mock[_-]|fixture[_-]|fault[_-]|inject[_-]|bypass[_-]|simulat[_-]|harness[_-]|NINFER_[A-Z0-9_]*(?:TEST|MOCK|FAULT|INJECT|BYPASS|SIMULAT|HARNESS))'
    foreach ($path in $Paths) {
        $tokens = $null
        $errors = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile(
            $path,
            [ref]$tokens,
            [ref]$errors
        )
        if (@($errors).Count -ne 0) { throw "published PowerShell script does not parse: $path" }
        $namedHooks = @($tokens | Where-Object {
                $_.Kind -cin @('Variable', 'SplattedVariable', 'Identifier', 'Generic') -and
                [string]$_.Text -cmatch $hookPattern
            })
        if ($namedHooks.Count -ne 0) {
            throw "published PowerShell script exposes hook vocabulary: $([IO.Path]::GetFileName($path)): $([string]::Join(',', @($namedHooks.Text)))"
        }

        $predicateText = [Collections.Generic.List[string]]::new()
        foreach ($node in $ast.FindAll({
                    param($candidate)
                    $candidate -is [Management.Automation.Language.IfStatementAst] -or
                    $candidate -is [Management.Automation.Language.WhileStatementAst] -or
                    $candidate -is [Management.Automation.Language.DoWhileStatementAst] -or
                    $candidate -is [Management.Automation.Language.DoUntilStatementAst] -or
                    $candidate -is [Management.Automation.Language.ForStatementAst] -or
                    $candidate -is [Management.Automation.Language.SwitchStatementAst] -or
                    $candidate -is [Management.Automation.Language.ParameterAst]
                }, $true)) {
            if ($node -is [Management.Automation.Language.IfStatementAst]) {
                foreach ($clause in $node.Clauses) { $predicateText.Add($clause.Item1.Extent.Text) }
            }
            elseif ($node -is [Management.Automation.Language.SwitchStatementAst]) {
                $predicateText.Add($node.Condition.Extent.Text)
                foreach ($clause in $node.Clauses) { $predicateText.Add($clause.Item1.Extent.Text) }
            }
            elseif ($node -is [Management.Automation.Language.ParameterAst]) {
                $predicateText.Add($node.Extent.Text)
            }
            elseif ($null -ne $node.Condition) { $predicateText.Add($node.Condition.Extent.Text) }
        }
        if (@($predicateText | Where-Object { $_ -cmatch $hookPattern }).Count -ne 0) {
            throw "published PowerShell predicate exposes hook vocabulary: $([IO.Path]::GetFileName($path))"
        }
    }
}

function ConvertTo-ReleaseTextBytes([string]$Path) {
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    $text = $encoding.GetString([IO.File]::ReadAllBytes($Path))
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xfeff) {
        $text = $text.Substring(1)
    }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n").Replace("`n", "`r`n")
    return [Text.UTF8Encoding]::new($false).GetBytes($text)
}

function Get-BytesSha256([byte[]]$Bytes) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha256.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

$releaseId = 'qwen38-4090-v0.2'
$releaseVersion = '0.2.3'
$assetStem = 'ninfer-4090-qwen38-v0.2.3-win-x64'
$profile = 'qwen38-4090-v0.2'
$upstreamBaseSha = '9ec1b82c7afa021314682d7a95390f8935ead7c2'
$server = (Resolve-Path -LiteralPath $ServerExecutable).Path
$sourceArchiveInput = (Resolve-Path -LiteralPath $SourceArchive).Path
$sourceArchiveSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceArchiveInput).Hash.ToLowerInvariant()
$sourceArchiveBytes = (Get-Item -LiteralPath $sourceArchiveInput).Length
$configSource = (Resolve-Path -LiteralPath $ServerConfig).Path
$qualificationSource = (Resolve-Path -LiteralPath $QualificationRecord).Path
$versionOutput = (& $server --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "ninfer-serve --version failed with exit code $LASTEXITCODE"
}
foreach ($required in @(
        "upstream_base_sha=$upstreamBaseSha",
        "patch_stack_sha=$PatchStackSha",
        "build_profile=$profile",
        'source_dirty=false'
    )) {
    if ($versionOutput.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
        throw "ninfer-serve --version is missing required release identity: $required"
    }
}

$specSource = Join-Path $PSScriptRoot 'release-spec.json'
$publishedScriptNames = @(
    'Compare-MtpQualification.ps1',
    'Control-GpuOwner.ps1',
    'Control-Release.ps1',
    'Install-Release.ps1',
    'Invoke-Qualification.ps1',
    'New-Package.ps1',
    'New-QualificationReceipt.ps1',
    'Protect-StateRoot.ps1'
)
$publishedScriptPaths = @($publishedScriptNames | ForEach-Object { Join-Path $PSScriptRoot $_ })
Assert-NoPublishedPowerShellHooks $publishedScriptPaths
$spec = Read-JsonFile $specSource
$config = Read-JsonFile $configSource
$qualification = Read-JsonFile $qualificationSource
if ($spec.artifact_type -cne 'ninfer_windows_release_spec' -or
    [int]$spec.schema_version -ne 1 -or
    [string]$spec.release_id -cne $releaseId -or
    [string]$spec.release_version -cne $releaseVersion -or
    [string]$spec.platform -cne 'windows-x86_64') {
    throw 'release specification identity mismatch'
}
$expectedCandidateRoots = @('bin', 'config', 'logs', 'receipts')
$candidateRoots = @($spec.lifecycle.candidate_root_directories)
if ($candidateRoots.Count -ne $expectedCandidateRoots.Count) {
    throw 'release specification installer architecture mismatch'
}
for ($index = 0; $index -lt $expectedCandidateRoots.Count; $index++) {
    if ([string]$candidateRoots[$index] -cne $expectedCandidateRoots[$index]) {
        throw 'release specification installer architecture mismatch'
    }
}
if ([Int64]$spec.model.bytes -ne 18210531328 -or
    [string]$spec.lifecycle.model_artifact_ownership -cne 'external-pinned-read-only' -or
    [string]$spec.lifecycle.cache_ownership -cne 'state-root-per-release' -or
    [string]$spec.lifecycle.interrupted_install_reentry -cne 'repair-required') {
    throw 'release specification installer architecture mismatch'
}
if ($config.artifact_type -cne 'ninfer_windows_server_config' -or
    [int]$config.schema_version -ne 1 -or
    [string]$config.release_id -cne $releaseId -or
    [string]$config.deployment_profile -cne $profile) {
    throw 'server configuration identity mismatch'
}
if ([bool]$config.session_checkpoint.enabled -ne $true -or
    [int]$config.session_checkpoint.quota_mib -lt 2048 -or
    [int]$config.session_checkpoint.staging_mib -lt 64 -or
    [int]$config.session_checkpoint.staging_mib -gt [int]$config.session_checkpoint.quota_mib) {
    throw 'server session checkpoint configuration is not release-safe'
}
if ($qualification.artifact_type -cne 'ninfer_public_windows_release_qualification' -or
    [int]$qualification.schema_version -ne 1 -or
    [string]$qualification.source.qualified_commit -cne $PatchStackSha) {
    throw 'qualification record is not bound to the executable source identity'
}
if ([string]$qualification.source.package_source_archive_sha256 -cne $sourceArchiveSha256 -or
    [Int64]$qualification.source.package_source_archive_bytes -ne $sourceArchiveBytes) {
    throw 'qualification record is not bound to the package source archive'
}
Assert-PublicQualificationDisclosure $qualification
if ([string]$spec.qualification_authority.in_archive_status -cne
        'candidate-only-not-release-eligible' -or
    [string]$spec.qualification_authority.external_sidecar_filename -cne
        "$assetStem-qualification.json" -or
    [string]$spec.qualification_authority.external_artifact_type -cne
        'ninfer_public_windows_release_qualification' -or
    [string]$spec.qualification_authority.external_required_status -cne 'passed' -or
    $spec.qualification_authority.external_required_release_eligible -ne $true) {
    throw 'release specification qualification authority contract mismatch'
}
if ([string]$qualification.authority.role -cne
        'external-final-qualification-authority' -or
    [string]$qualification.authority.binding -cne 'SHA256SUMS-bound-sidecar' -or
    [string]$qualification.authority.sidecar_filename -cne
        [string]$spec.qualification_authority.external_sidecar_filename) {
    throw 'qualification record supersession authority mismatch'
}
if ([string]$qualification.status -cnotin @('candidate_ready', 'passed')) {
    throw 'qualification record has an unsupported authority status'
}
if ([string]$spec.source.qualified_source_head -cne $PatchStackSha) {
    throw 'release specification is not bound to the executable source identity'
}
$serverSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $server).Hash.ToLowerInvariant()
$configBytes = ConvertTo-ReleaseTextBytes $configSource
$configSha256 = Get-BytesSha256 $configBytes
if ([string]$qualification.identity.binary_sha256 -cne $serverSha256 -or
    [string]$qualification.identity.config_sha256 -cne $configSha256 -or
    [string]$qualification.identity.model_artifact_sha256 -cne [string]$spec.model.sha256) {
    throw 'qualification record artifact hashes do not match the release inputs'
}
if ([string]$qualification.status -ceq 'passed') {
    if ($qualification.release_eligible -ne $true -or
        $qualification.authority.supersedes_package_candidate_status -ne $true -or
        [string]$qualification.authority.superseded_status -cne 'candidate_ready') {
        throw 'passed qualification does not supersede candidate status unambiguously'
    }
    foreach ($gate in @('G', 'L', 'R')) {
        $property = $qualification.release_gates.PSObject.Properties[$gate]
        if ($null -eq $property -or $null -eq $property.Value.PSObject.Properties['status'] -or
            [string]$property.Value.status -cne 'passed') {
            throw 'qualification record claims passed while required release gates remain incomplete'
        }
    }
}
elseif ($qualification.release_eligible -ne $false -or
    $qualification.authority.supersedes_package_candidate_status -ne $false) {
    throw 'candidate qualification claims release eligibility or supersession'
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path

$zipPath = Join-Path $outputRoot "$assetStem.zip"
$sbomPath = Join-Path $outputRoot "$assetStem.spdx.json"
$sourceArchiveAsset = Join-Path $outputRoot "$assetStem-source.tar.gz"
$releaseManifestAsset = Join-Path $outputRoot "$assetStem-release-manifest.json"
$packageReceiptAsset = Join-Path $outputRoot 'package-build-receipt.json'
$qualificationPath = Join-Path $outputRoot "$assetStem-qualification.json"
$installerAsset = Join-Path $outputRoot 'Install-Release.ps1'
$controllerAsset = Join-Path $outputRoot 'Control-Release.ps1'
$gpuOwnerAsset = Join-Path $outputRoot 'Control-GpuOwner.ps1'
$stateProtectionAsset = Join-Path $outputRoot 'Protect-StateRoot.ps1'
$standaloneScriptAssets = [ordered]@{}
foreach ($name in $publishedScriptNames) {
    $standaloneScriptAssets[$name] = Join-Path $outputRoot $name
}
$immutableAssets = @(
    $zipPath, $sbomPath, $sourceArchiveAsset, $releaseManifestAsset, $packageReceiptAsset
) + @($standaloneScriptAssets.Values)
$shaSumsPath = Join-Path $outputRoot 'SHA256SUMS'
if ($FinalizeExistingAssets) {
    if ([string]$qualification.status -cne 'passed' -or
        $qualification.release_eligible -ne $true -or
        $qualification.authority.supersedes_package_candidate_status -ne $true) {
        throw 'only a passed final authority may finalize existing package assets'
    }
    foreach ($path in $immutableAssets) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "existing immutable release asset is missing: $path"
        }
    }
    if (-not (Test-Path -LiteralPath $shaSumsPath -PathType Leaf)) {
        throw 'candidate SHA256SUMS is missing before final authority publication'
    }
    $candidateSums = @{}
    foreach ($line in @(Get-Content -LiteralPath $shaSumsPath -Encoding UTF8)) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$' -or $candidateSums.ContainsKey($Matches[2])) {
            throw 'candidate SHA256SUMS is malformed or contains duplicates'
        }
        $candidateSums[$Matches[2]] = $Matches[1]
    }
    $expectedCandidateNames = @($immutableAssets + $qualificationPath | ForEach-Object {
            [IO.Path]::GetFileName($_)
        })
    if ($candidateSums.Count -ne $expectedCandidateNames.Count -or
        $candidateSums.ContainsKey('SHA256SUMS')) {
        throw 'candidate SHA256SUMS does not contain the exact non-self asset set'
    }
    foreach ($name in $expectedCandidateNames) {
        if (-not $candidateSums.ContainsKey($name)) {
            throw 'candidate SHA256SUMS does not contain the exact non-self asset set'
        }
    }
    foreach ($path in $immutableAssets) {
        $name = [IO.Path]::GetFileName($path)
        if (-not $candidateSums.ContainsKey($name) -or
            [string]$candidateSums[$name] -cne
                (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()) {
            throw "immutable release asset changed after candidate assembly: $name"
        }
    }
    $expectedPackageBytes = if ($null -ne $qualification.package.PSObject.Properties['bytes']) {
        [Int64]$qualification.package.bytes
    }
    else { [Int64]$qualification.package.size_bytes }
    $expectedSbomBytes = if ($null -ne $qualification.package.sbom.PSObject.Properties['bytes']) {
        [Int64]$qualification.package.sbom.bytes
    }
    else { [Int64]$qualification.package.sbom.size_bytes }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant() -cne
            [string]$qualification.package.sha256 -or
        (Get-Item -LiteralPath $zipPath).Length -ne $expectedPackageBytes -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $sbomPath).Hash.ToLowerInvariant() -cne
            [string]$qualification.package.sbom.sha256 -or
        (Get-Item -LiteralPath $sbomPath).Length -ne $expectedSbomBytes) {
        throw 'final authority does not match existing immutable ZIP or SPDX assets'
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $sourceArchiveAsset).Hash.ToLowerInvariant() -cne
            $sourceArchiveSha256 -or
        (Get-Item -LiteralPath $sourceArchiveAsset).Length -ne $sourceArchiveBytes) {
        throw 'final authority source archive does not match candidate assembly'
    }
    [IO.File]::WriteAllBytes($qualificationPath, [IO.File]::ReadAllBytes($qualificationSource))
    $assets = @($immutableAssets + $qualificationPath) |
        Sort-Object { [IO.Path]::GetFileName($_) }
    $sumLines = foreach ($asset in $assets) {
        "$((Get-FileHash -Algorithm SHA256 -LiteralPath $asset).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($asset))"
    }
    $lineFeed = [string][char]10
    [IO.File]::WriteAllText(
        $shaSumsPath,
        ([string]::Join($lineFeed, $sumLines) + $lineFeed),
        [Text.UTF8Encoding]::new($false)
    )
    [ordered]@{
        artifact_type = 'ninfer_package_finalization_receipt'
        schema_version = 1
        status = 'passed'
        mode = 'finalized-existing-assets'
        patch_stack_sha = $PatchStackSha
        package_sha256 = [string]$qualification.package.sha256
        sbom_sha256 = [string]$qualification.package.sbom.sha256
        qualification_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $qualificationPath).Hash.ToLowerInvariant()
        checksum_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $shaSumsPath).Hash.ToLowerInvariant()
        checksum_entries = $sumLines.Count
        checksum_line_ending = 'LF'
        candidate_status_superseded = $true
        release_eligible = $true
    } | ConvertTo-Json -Depth 6
    return
}
$stage = Join-Path $outputRoot ('.stage-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage $releaseId
$bin = Join-Path $payload 'bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null
foreach ($path in @($immutableAssets + $qualificationPath + $shaSumsPath)) {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}

try {
    Copy-Item -LiteralPath $server -Destination (Join-Path $bin 'ninfer-serve.exe')
    $runtimeNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $runtimeNames.Add('ninfer-serve.exe') | Out-Null
    foreach ($file in $RuntimeFile) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "runtime file does not exist: $file"
        }
        $name = [IO.Path]::GetFileName($file)
        if (-not $runtimeNames.Add($name)) { throw "duplicate runtime filename: $name" }
        Copy-Item -LiteralPath $file -Destination (Join-Path $bin $name)
    }

    foreach ($name in @(
            'release-spec.json',
            'Control-Release.ps1',
            'Control-GpuOwner.ps1',
            'Protect-StateRoot.ps1',
            'Install-Release.ps1',
            'New-QualificationReceipt.ps1',
            'Invoke-Qualification.ps1',
            'Compare-MtpQualification.ps1'
        )) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $payload $name)
    }
    [IO.File]::WriteAllBytes((Join-Path $payload 'server-config.json'), $configBytes)

    $repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
    $smokeDirectory = Join-Path $payload 'smoke'
    New-Item -ItemType Directory -Force -Path $smokeDirectory | Out-Null
    foreach ($name in @(
            'agent_protocol.py',
            'golden_equivalent.py',
            'golden_equivalent_extension.ts',
            'golden_equivalent_contract.json'
        )) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $smokeDirectory
    }
    foreach ($name in @('serve_contract.py', '__init__.py')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "tools/smoke/$name") -Destination $smokeDirectory
    }

    $binaryPath = Join-Path $bin 'ninfer-serve.exe'
    $configPath = Join-Path $payload 'server-config.json'
    $specPath = Join-Path $payload 'release-spec.json'
    $controllerPath = Join-Path $payload 'Control-Release.ps1'
    $gpuOwnerPath = Join-Path $payload 'Control-GpuOwner.ps1'
    $stateProtectionPath = Join-Path $payload 'Protect-StateRoot.ps1'
    $installerPath = Join-Path $payload 'Install-Release.ps1'
    $manifest = [ordered]@{
        artifact_type = 'ninfer_windows_release_manifest'
        schema_version = 1
        release_id = $releaseId
        release_version = $releaseVersion
        release_instance_id = "$releaseId-$($PatchStackSha.Substring(0, 12))"
        asset_filename = [IO.Path]::GetFileName($zipPath)
        deployment_profile = $profile
        upstream_base_sha = $upstreamBaseSha
        patch_stack_sha = $PatchStackSha
        source_dirty = $false
        binary_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $binaryPath).Hash.ToLowerInvariant()
        config_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
        release_spec_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $specPath).Hash.ToLowerInvariant()
        installer_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $installerPath).Hash.ToLowerInvariant()
        controller_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $controllerPath).Hash.ToLowerInvariant()
        gpu_owner_controller_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $gpuOwnerPath).Hash.ToLowerInvariant()
        state_protection_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $stateProtectionPath).Hash.ToLowerInvariant()
        created_utc = [DateTime]::UtcNow.ToString('o')
    }
    $manifestPath = Join-Path $payload 'release-manifest.json'
    [IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false)
    )

    foreach ($payloadFile in @(Get-ChildItem -LiteralPath $payload -File -Recurse -Force)) {
        $payloadFile.Attributes = $payloadFile.Attributes -band (-bnot ([IO.FileAttributes]::Hidden -bor [IO.FileAttributes]::System))
    }
    $checksumEntries = @(
        Get-ChildItem -LiteralPath $payload -File -Recurse -Force |
            Sort-Object FullName |
            ForEach-Object {
                [ordered]@{
                    relative_path = $_.FullName.Substring($payload.Length + 1).Replace('\', '/')
                    sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
                    bytes = $_.Length
                }
            }
    )
    $checksums = [ordered]@{
        artifact_type = 'ninfer_package_checksums'
        schema_version = 1
        files = $checksumEntries
    }
    [IO.File]::WriteAllText(
        (Join-Path $payload 'checksums.json'),
        ($checksums | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false)
    )

    Compress-Archive -LiteralPath $payload -DestinationPath $zipPath -CompressionLevel Optimal
    $zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
    $runtimeDllCount = @(Get-ChildItem -LiteralPath $bin -File -Filter '*.dll').Count
    $python = (Get-Command python.exe -ErrorAction Stop).Source
    & $python (Join-Path $PSScriptRoot 'new_sbom.py') --archive $zipPath --output $sbomPath --release-tag 'v0.1.0-qwen38-4090-beta.1' --source-commit $PatchStackSha --created $SbomCreatedUtc | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $sbomPath -PathType Leaf)) {
        throw 'package-owned SPDX generation failed'
    }
    $sbom = Read-JsonFile $sbomPath
    if ([string]$sbom.spdxVersion -cne 'SPDX-2.3' -or
        @($sbom.files).Count -ne ($checksumEntries.Count + 1)) {
        throw 'package-owned SPDX inventory does not match ZIP members'
    }
    $sbomSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sbomPath).Hash.ToLowerInvariant()

    Set-JsonProperty $qualification 'release_id' ([string]$manifest.release_instance_id)
    Set-JsonProperty $qualification 'release_version' $releaseVersion
    Set-JsonProperty $qualification 'package_assembled_utc' ([DateTime]::UtcNow.ToString('o'))
    Set-JsonProperty $qualification 'package' ([ordered]@{
            filename = [IO.Path]::GetFileName($zipPath)
            sha256 = $zipHash
            size_bytes = (Get-Item -LiteralPath $zipPath).Length
            manifest_entries = $checksumEntries.Count
            runtime_dlls = $runtimeDllCount
            checksum_verification = 'passed'
            release_manifest_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant()
            installer_sha256 = [string]$manifest.installer_sha256
            controller_sha256 = [string]$manifest.controller_sha256
            sbom = [ordered]@{
                filename = [IO.Path]::GetFileName($sbomPath)
                sha256 = $sbomSha256
                size_bytes = (Get-Item -LiteralPath $sbomPath).Length
                files_analyzed = @($sbom.files).Count
            }
        })
    [IO.File]::WriteAllText(
        $qualificationPath,
        ($qualification | ConvertTo-Json -Depth 24),
        [Text.UTF8Encoding]::new($false)
    )

    Copy-Item -LiteralPath $sourceArchiveInput -Destination $sourceArchiveAsset
    Copy-Item -LiteralPath $manifestPath -Destination $releaseManifestAsset
    foreach ($name in $publishedScriptNames) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) `
            -Destination ([string]$standaloneScriptAssets[$name])
    }
    $scriptHashes = [ordered]@{}
    foreach ($name in $publishedScriptNames) {
        $scriptPath = [string]$standaloneScriptAssets[$name]
        $scriptHashes[$name] = [ordered]@{
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $scriptPath).Hash.ToLowerInvariant()
            bytes = (Get-Item -LiteralPath $scriptPath).Length
        }
    }
    $packageReceipt = [ordered]@{
        artifact_type = 'ninfer_package_build_receipt'
        schema_version = 1
        status = 'passed'
        release_id = $releaseId
        release_version = $releaseVersion
        patch_stack_sha = $PatchStackSha
        package_sha256 = $zipHash
        package_bytes = (Get-Item -LiteralPath $zipPath).Length
        package_file = [IO.Path]::GetFileName($zipPath)
        source_archive_file = [IO.Path]::GetFileName($sourceArchiveAsset)
        source_archive_sha256 = $sourceArchiveSha256
        source_archive_bytes = $sourceArchiveBytes
        release_manifest_file = [IO.Path]::GetFileName($releaseManifestAsset)
        release_manifest_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $releaseManifestAsset).Hash.ToLowerInvariant()
        sbom_file = [IO.Path]::GetFileName($sbomPath)
        sbom_sha256 = $sbomSha256
        sbom_bytes = (Get-Item -LiteralPath $sbomPath).Length
        sbom_files_analyzed = @($sbom.files).Count
        qualification_file = [IO.Path]::GetFileName($qualificationPath)
        qualification_status = [string]$qualification.status
        release_eligible_at_assembly = [bool]$qualification.release_eligible
        external_final_qualification_required = $true
        candidate_status_superseded = [bool]$qualification.authority.supersedes_package_candidate_status
        published_scripts = $scriptHashes
        checksum_file = [IO.Path]::GetFileName($shaSumsPath)
        checksum_entries = $immutableAssets.Count + 1
        checksum_line_ending = 'LF'
    }
    [IO.File]::WriteAllText(
        $packageReceiptAsset,
        ($packageReceipt | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false)
    )
    $assets = @($immutableAssets + $qualificationPath) |
        Sort-Object { [IO.Path]::GetFileName($_) }
    if ($assets.Count -ne $packageReceipt.checksum_entries) {
        throw 'outer release asset inventory is incomplete'
    }
    $sumLines = foreach ($asset in $assets) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $asset).Hash.ToLowerInvariant()
        "$hash  $([IO.Path]::GetFileName($asset))"
    }
    $lineFeed = [string][char]10
    [IO.File]::WriteAllText(
        $shaSumsPath,
        ([string]::Join($lineFeed, $sumLines) + $lineFeed),
        [Text.UTF8Encoding]::new($false)
    )

    $packageReceipt | ConvertTo-Json -Depth 8
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
