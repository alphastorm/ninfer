Set-StrictMode -Version Latest

function Replace-NInferHarnessTextOnce(
    [string]$Text,
    [string]$Needle,
    [string]$Replacement,
    [string]$Label
) {
    $index = $Text.IndexOf($Needle, [StringComparison]::Ordinal)
    if ($index -lt 0) { throw "instrumentation anchor is missing: $Label" }
    if ($Text.IndexOf($Needle, $index + $Needle.Length, [StringComparison]::Ordinal) -ge 0) {
        throw "instrumentation anchor is ambiguous: $Label"
    }
    return $Text.Substring(0, $index) + $Replacement + $Text.Substring($index + $Needle.Length)
}

function Add-NInferHarnessTextAfterOnce(
    [string]$Text,
    [string]$Needle,
    [string]$Addition,
    [string]$Label
) {
    return Replace-NInferHarnessTextOnce $Text $Needle ($Needle + $Addition) $Label
}

function New-NInferInstrumentedInstallerText([string]$PublishedInstallerText) {
    foreach ($forbidden in @(
            'InstallTestMode', 'NINFER_TEST_INSTALL', 'Invoke-InstallFault',
            'NInferSimulatedInterruption'
        )) {
        if ($PublishedInstallerText.Contains($forbidden)) {
            throw "published installer contains a callable test or fault hook: $forbidden"
        }
    }

    $instrumented = $PublishedInstallerText
    $harnessFunctions = @'

function Invoke-NInferHarnessFault([string]$Point) {
    if ([string]$env:NINFER_HARNESS_INTERRUPTION_AFTER -ceq $Point) {
        $exception = [InvalidOperationException]::new("simulated interrupted install after $Point")
        $exception.Data['NInferHarnessInterruption'] = $true
        throw $exception
    }
    if ([string]$env:NINFER_HARNESS_FAILURE_AFTER -ceq $Point) {
        throw "injected install failure after $Point"
    }
}
'@
    $instrumented = Add-NInferHarnessTextAfterOnce $instrumented `
        '$ErrorActionPreference = ''Stop''' $harnessFunctions 'harness functions'

    $productionModelPin = @'
    if ([Int64]$Spec.model.bytes -ne 18210531328) {
        throw 'release specification pinned model byte length mismatch'
    }
'@
    $instrumentedModelPin = @'
    if ([Int64]$Spec.model.bytes -le 0) {
        throw 'instrumented fixture model byte length is invalid'
    }
'@
    $instrumented = Replace-NInferHarnessTextOnce $instrumented `
        $productionModelPin $instrumentedModelPin 'fixture model pin'

    $productionGpuQuery = @'
    $rows = @(Invoke-TrustedNvidiaSmi '--query-gpu=index,uuid,name,compute_cap,driver_version --format=csv,noheader,nounits')
'@
    $instrumentedGpuQuery = @'
    $rows = @('0, GPU-fixture-4090, NVIDIA GeForce RTX 4090, 8.9, 581.15')
'@
    $instrumented = Replace-NInferHarnessTextOnce $instrumented `
        $productionGpuQuery $instrumentedGpuQuery 'fixture trusted GPU query result'

    $stageAnchor = @'
    Write-InstallEvent 'install_started' 'Started durable installer transaction' ([ordered]@{
            operation_id = $operationId
            journal_path = $journalPath
        })
'@
    $instrumented = Add-NInferHarnessTextAfterOnce $instrumented $stageAnchor `
        ([Environment]::NewLine + "    Invoke-NInferHarnessFault 'stage_create'") `
        'stage_create'

    $faultAnchors = @(
        [ordered]@{ point = 'model_reference'; anchor = '        Set-TransactionPhase $transaction ''model_reference_verified''' },
        [ordered]@{ point = 'candidate_layout'; anchor = '        Set-TransactionPhase $transaction ''candidate_layout_created''' },
        [ordered]@{ point = 'secret_copy'; anchor = '        Assert-OneLineSecret $installedKey' },
        [ordered]@{ point = 'controller_copy'; anchor = '        Copy-Item -LiteralPath $controllerSource -Destination $controllerPath -Force' },
        [ordered]@{ point = 'state_activation'; anchor = '        Write-JsonAtomic $statePath $state' },
        [ordered]@{ point = 'task_registration'; anchor = '        Register-ReleaseTask $taskName $controllerPath' },
        [ordered]@{ point = 'candidate_start'; anchor = '            & $controllerPath -Action Start -StateRoot $StateRoot | Out-Null' }
    )
    foreach ($binding in $faultAnchors) {
        $addition = [Environment]::NewLine +
            "        Invoke-NInferHarnessFault '$([string]$binding.point)'"
        $instrumented = Add-NInferHarnessTextAfterOnce $instrumented `
            ([string]$binding.anchor) $addition ([string]$binding.point)
    }

    $incumbentReplacement = @'
        Invoke-NInferHarnessFault 'incumbent_stop'
        if ($null -ne $ownerControllerSource) {
'@
    $instrumented = Replace-NInferHarnessTextOnce $instrumented `
        '        if ($null -ne $ownerControllerSource) {' `
        $incumbentReplacement 'incumbent stop'

    $ownerCopyReplacement = @'
        Invoke-NInferHarnessFault 'owner_controller_copy'
        Set-TransactionFlag $transaction 'controller_touched'
'@
    $instrumented = Replace-NInferHarnessTextOnce $instrumented `
        '        Set-TransactionFlag $transaction ''controller_touched''' `
        $ownerCopyReplacement 'owner controller copy'

    $interruptionHandler = @'

        if ([bool]$installFailure.Exception.Data['NInferHarnessInterruption']) {
            Set-ObjectProperty $transaction 'status' 'repair_required'
            Set-ObjectProperty $transaction 'phase' 'interrupted'
            Set-ObjectProperty $transaction 'diagnostics' @(
                Get-BoundedDiagnostic $installFailure.Exception.Message
            )
            Write-InstallTransaction $transaction
            Write-InstallEvent 'install_interrupted' 'Installer transaction requires repair; re-entry is blocked' ([ordered]@{
                    operation_id = $operationId
                    journal_path = $journalPath
                })
            throw $installFailure
        }
'@
    $instrumented = Add-NInferHarnessTextAfterOnce $instrumented `
        '        $installFailure = $_' $interruptionHandler 'interruption handler'
    return $instrumented
}
