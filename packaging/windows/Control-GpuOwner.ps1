[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'stop', 'start')]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$StateRoot,

    [ValidatePattern('^GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$')]
    [string]$GpuUuid,

    # The lane's managed power cap while the release runs; 0 leaves the power limit untouched
    # and the lease only records the paused state.
    [ValidateRange(0, 1000)]
    [int]$QualifiedPowerLimitW = 0,

    [ValidateRange(1, 1000)]
    [int]$PriorPowerLimitMinW = 100,

    [ValidateRange(1, 1000)]
    [int]$PriorPowerLimitMaxW = 1000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Protect-StateRoot.ps1')

# Preserve whether UUID selection was explicit; Resolve-OwnerGpu fills $GpuUuid even when
# omitted, but the single-GPU legacy lease path must remain implicit in a nested status.
$statusParameters = @{} + $PSBoundParameters
$statusParameters['Action'] = 'status'

$StateRoot = Initialize-NInferProtectedStateRoot $StateRoot
Assert-NInferProtectedStateTree $StateRoot
$statePath = Join-Path $StateRoot 'lease.json'
$qualifiedPowerLimitW = $QualifiedPowerLimitW
$managesPowerLimit = $qualifiedPowerLimitW -gt 0
$interactiveGpuMemoryThresholdBytes = 1GB
$nvidiaSmi = Join-Path ([Environment]::GetFolderPath('System')) 'nvidia-smi.exe'
if (-not (Test-Path -LiteralPath $nvidiaSmi -PathType Leaf)) {
    $nvidiaSmi = Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'
}
if (-not (Test-Path -LiteralPath $nvidiaSmi -PathType Leaf)) {
    throw 'trusted nvidia-smi installation is missing'
}

function Invoke-NvidiaSmi([string[]]$Arguments) {
    $output = @(& $nvidiaSmi @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "nvidia-smi failed with exit $LASTEXITCODE" }
    return $output
}

function Get-PowerLimitW {
    $value = ((Invoke-NvidiaSmi @(
        '-i', $GpuUuid,
        '--query-gpu=power.limit',
        '--format=csv,noheader,nounits'
    )) | Out-String).Trim()
    $parsed = 0.0
    if (-not [double]::TryParse(
            $value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed
        )) {
        throw 'GPU power limit is not numeric'
    }
    return [int][Math]::Round($parsed)
}

function Resolve-OwnerGpu {
    $script:singleGpuSelection = $false
    if ($GpuUuid) {
        $rows = @(Invoke-NvidiaSmi @('-i', $GpuUuid, '--query-gpu=uuid,pci.bus_id', '--format=csv,noheader,nounits'))
    }
    else {
        $rows = @(Invoke-NvidiaSmi @('--query-gpu=uuid,pci.bus_id', '--format=csv,noheader,nounits'))
        $script:singleGpuSelection = $true
    }
    if ($rows.Count -ne 1) { throw 'GPU owner requires exactly one visible GPU without -GpuUuid; select one explicitly' }
    $parts = @(([string]$rows[0] -split ',') | ForEach-Object { $_.Trim() })
    if ($parts.Count -ne 2 -or
        $parts[0] -cnotmatch '^GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$' -or
        $parts[1] -notmatch '^00000000:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$' -or
        ($GpuUuid -and $parts[0] -ine $GpuUuid)) {
        throw 'GPU owner cannot resolve the selected UUID and PCI address (only PCI domain zero is observable)'
    }
    $script:GpuUuid = $parts[0]
    $script:gpuPciBusId = $parts[1].ToUpperInvariant()
    $script:gpuPciAddress = $parts[1].Substring(9).ToUpperInvariant()
}

# Perf counters carry a LUID, not a CUDA ordinal. D3DKMT's adapter address has no PCI domain.
function Get-AdapterPciAddress([string]$High, [string]$Low) {
    if ($null -eq ('NInfer.GpuAdapterAddress' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace NInfer {
    public static class GpuAdapterAddress {
        [StructLayout(LayoutKind.Sequential)] struct Luid { public uint Low; public uint High; }
        [StructLayout(LayoutKind.Sequential)] struct Open { public Luid Luid; public uint Handle; }
        [StructLayout(LayoutKind.Sequential)] struct Query { public uint Handle; public int Type; public IntPtr Data; public uint Size; }
        [StructLayout(LayoutKind.Sequential)] struct Address { public uint Bus; public uint Device; public uint Function; }
        [StructLayout(LayoutKind.Sequential)] struct Close { public uint Handle; }
        [DllImport("gdi32.dll")] static extern int D3DKMTOpenAdapterFromLuid(ref Open value);
        [DllImport("gdi32.dll")] static extern int D3DKMTQueryAdapterInfo(ref Query value);
        [DllImport("gdi32.dll")] static extern int D3DKMTCloseAdapter(ref Close value);
        public static string Resolve(uint high, uint low) {
            var open = new Open { Luid = new Luid { High = high, Low = low } };
            if (D3DKMTOpenAdapterFromLuid(ref open) != 0) throw new InvalidOperationException("cannot observe GPU counter LUID: open failed");
            IntPtr data = IntPtr.Zero;
            try {
                data = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Address)));
                var query = new Query { Handle = open.Handle, Type = 6, Data = data, Size = (uint)Marshal.SizeOf(typeof(Address)) };
                if (D3DKMTQueryAdapterInfo(ref query) != 0) throw new InvalidOperationException("cannot observe GPU counter LUID: address query failed");
                var address = (Address)Marshal.PtrToStructure(data, typeof(Address));
                if (address.Bus == UInt32.MaxValue) return null; // Software adapter, no PCI device.
                if (address.Bus > 255 || address.Device > 31 || address.Function > 7) throw new InvalidOperationException("invalid GPU counter PCI address");
                return String.Format("{0:X2}:{1:X2}.{2}", address.Bus, address.Device, address.Function);
            }
            finally {
                if (data != IntPtr.Zero) Marshal.FreeHGlobal(data);
                var close = new Close { Handle = open.Handle };
                if (D3DKMTCloseAdapter(ref close) != 0) throw new InvalidOperationException("cannot close GPU counter adapter");
            }
        }
    }
}
'@
    }
    return [NInfer.GpuAdapterAddress]::Resolve([Convert]::ToUInt32($High, 16), [Convert]::ToUInt32($Low, 16))
}

function Get-InteractiveGpuWorkloadCount {
    # An idle card may have no process-memory instance. Establish observability from the
    # adapter-memory inventory first, then count processes on its exact adapter LUIDs.
    $adapters = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory -ErrorAction Stop)
    $addresses = @{}
    $selectedLuids = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($adapter in $adapters) {
        if ([string]$adapter.Name -notmatch '^luid_0x([0-9a-f]{8})_0x([0-9a-f]{8})_phys_\d+$') { continue }
        $high = $Matches[1]; $low = $Matches[2]
        $luid = $high + ':' + $low
        if (-not $addresses.ContainsKey($luid)) {
            try { $addresses[$luid] = Get-AdapterPciAddress $high $low }
            catch { continue } # Another adapter may be disappearing; active unknown LUIDs fail below.
        }
        if ($addresses[$luid] -ceq $gpuPciAddress) { [void]$selectedLuids.Add($luid) }
    }
    if ($selectedLuids.Count -eq 0) { throw 'cannot observe selected GPU: no adapter-memory counter LUID maps to its PCI address' }
    $rows = @(Get-CimInstance `
        -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory `
        -ErrorAction Stop)
    $owners = [Collections.Generic.HashSet[int]]::new()
    foreach ($row in $rows) {
        $usage = 0L
        if (-not [Int64]::TryParse([string]$row.DedicatedUsage, [ref]$usage) -or $usage -lt 0) {
            throw 'GPU process-memory counters returned an invalid dedicated-usage value'
        }
        if ([string]$row.Name -notmatch '^pid_(\d+)_luid_0x([0-9a-f]{8})_0x([0-9a-f]{8})_phys_\d+$') {
            if ($usage -ge $interactiveGpuMemoryThresholdBytes) {
                throw 'GPU process-memory counters returned an unparseable active owner'
            }
            continue
        }
        $ownerPid = [int]$Matches[1]
        $high = $Matches[2]; $low = $Matches[3]
        $luid = $high + ':' + $low
        if ($usage -lt $interactiveGpuMemoryThresholdBytes) { continue }
        if (-not $selectedLuids.Contains($luid)) {
            if (-not $addresses.ContainsKey($luid)) { $addresses[$luid] = Get-AdapterPciAddress $high $low }
            if ($addresses[$luid] -ceq $gpuPciAddress) { throw 'selected GPU process LUID disappeared from the adapter inventory' }
            continue
        }
        [void]$owners.Add($ownerPid)
    }
    return $owners.Count
}

function Assert-InteractiveGpuWorkloadAbsent {
    if ((Get-InteractiveGpuWorkloadCount) -ne 0) {
        throw 'GPU has an active interactive workload; close it before appliance start'
    }
}

function Read-State {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { return $null }
    $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
    $bound = $state.PSObject.Properties['gpu_uuid']
    if (($null -eq $bound -and -not $singleGpuSelection) -or
        ($null -ne $bound -and [string]$bound.Value -cne $GpuUuid)) {
        throw 'GPU-owner lease UUID is absent or differs; restore with its original controller before upgrading'
    }
    $pci = $state.PSObject.Properties['gpu_pci_bus_id']
    if (($null -eq $pci -and $null -ne $bound) -or
        ($null -ne $pci -and [string]$pci.Value -cne $gpuPciBusId)) {
        throw 'GPU-owner lease PCI identity is absent or differs'
    }
    $phase = if ($null -eq $state.PSObject.Properties['phase']) { 'paused' } else { [string]$state.phase }
    if ([int]$state.schema_version -ne 1 -or -not [bool]$state.paused -or
        $phase -cnotin @('prepared', 'paused') -or
        [int]$state.qualified_power_limit_w -ne $qualifiedPowerLimitW -or
        [int]$state.prior_power_limit_w -lt $PriorPowerLimitMinW -or
        [int]$state.prior_power_limit_w -gt $PriorPowerLimitMaxW) {
        throw 'GPU-owner lease state is invalid or the safe power cap drifted'
    }
    return $state
}

function Write-State([int]$PriorPowerLimitW, [ValidateSet('prepared', 'paused')][string]$Phase) {
    $temporary = Join-Path $StateRoot ('.lease-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        [IO.File]::WriteAllText(
            $temporary,
            (([ordered]@{
                schema_version = 1
                gpu_uuid = $GpuUuid
                gpu_pci_bus_id = $gpuPciBusId
                paused = $true
                phase = $Phase
                qualified_power_limit_w = $qualifiedPowerLimitW
                prior_power_limit_w = $PriorPowerLimitW
            } | ConvertTo-Json -Compress) + [Environment]::NewLine),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $statePath -Force
        Assert-NInferProtectedStateTree $StateRoot
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Write-Status([bool]$Paused, [int]$PowerLimitW) {
    [ordered]@{
        schema_version = 1
        gpu_uuid = $GpuUuid
        gpu_pci_bus_id = $gpuPciBusId
        paused = $Paused
        power_limit_w = $PowerLimitW
        qualified_power_limit_w = $qualifiedPowerLimitW
        interactive_gpu_workload_count = (Get-InteractiveGpuWorkloadCount)
    } | ConvertTo-Json -Compress
}

Resolve-OwnerGpu

switch ($Action) {
    'status' {
        $state = Read-State
        $limit = Get-PowerLimitW
        if ($null -ne $state) {
            $phase = if ($null -eq $state.PSObject.Properties['phase']) { 'paused' } else { [string]$state.phase }
            if (-not $managesPowerLimit) {
                if ($phase -ceq 'prepared') { Write-State ([int]$state.prior_power_limit_w) 'paused' }
                Write-Status $true $limit
            }
            elseif ($phase -ceq 'prepared') {
                if ($limit -eq $qualifiedPowerLimitW) {
                    Write-State ([int]$state.prior_power_limit_w) 'paused'
                    Write-Status $true $limit
                }
                elseif ($limit -eq [int]$state.prior_power_limit_w) {
                    Write-Status $false $limit
                }
                else {
                    throw 'prepared GPU-owner lease has an unrecoverable power-limit drift'
                }
            }
            elseif ($limit -ne $qualifiedPowerLimitW) {
                throw 'GPU-owner lease state is invalid or the safe power cap drifted'
            }
            else { Write-Status $true $limit }
        }
        else {
            Write-Status $false $limit
        }
    }
    'stop' {
        $state = Read-State
        if ($null -ne $state) {
            Assert-InteractiveGpuWorkloadAbsent
            $phase = if ($null -eq $state.PSObject.Properties['phase']) { 'paused' } else { [string]$state.phase }
            if ($phase -ceq 'prepared' -and $managesPowerLimit) {
                if ((Get-PowerLimitW) -ne $qualifiedPowerLimitW) {
                    Invoke-NvidiaSmi @('-i', $GpuUuid, '-pl', [string]$qualifiedPowerLimitW) | Out-Null
                }
                if ((Get-PowerLimitW) -ne $qualifiedPowerLimitW) {
                    throw 'GPU did not enter the qualified safe power cap'
                }
                Write-State ([int]$state.prior_power_limit_w) 'paused'
            }
            & $PSCommandPath @statusParameters
            break
        }
        Assert-InteractiveGpuWorkloadAbsent
        $prior = Get-PowerLimitW
        if (-not $managesPowerLimit) {
            Write-State $prior 'paused'
            Write-Status $true $prior
            break
        }
        Write-State $prior 'prepared'
        try {
            Invoke-NvidiaSmi @('-i', $GpuUuid, '-pl', [string]$qualifiedPowerLimitW) | Out-Null
            if ((Get-PowerLimitW) -ne $qualifiedPowerLimitW) {
                throw 'GPU did not enter the qualified safe power cap'
            }
            Write-State $prior 'paused'
        }
        catch {
            $failure = $_
            try {
                Invoke-NvidiaSmi @('-i', $GpuUuid, '-pl', [string]$prior) | Out-Null
                if ((Get-PowerLimitW) -ne $prior) { throw 'prior GPU power limit restore failed' }
                Remove-Item -LiteralPath $statePath -Force
            }
            catch {
                throw [InvalidOperationException]::new(
                    "GPU cap acquisition failed and prepared lease restoration is required: $($_.Exception.Message)",
                    $failure.Exception
                )
            }
            throw $failure
        }
        Write-Status $true $qualifiedPowerLimitW
    }
    'start' {
        $state = Read-State
        if ($null -eq $state) {
            Write-Status $false (Get-PowerLimitW)
            break
        }
        $prior = [int]$state.prior_power_limit_w
        if ($prior -lt $PriorPowerLimitMinW -or $prior -gt $PriorPowerLimitMaxW) { throw 'recorded prior GPU power limit is invalid' }
        if ($managesPowerLimit) {
            Invoke-NvidiaSmi @('-i', $GpuUuid, '-pl', [string]$prior) | Out-Null
            if ((Get-PowerLimitW) -ne $prior) { throw 'GPU prior power limit was not restored' }
        }
        # The end state is that no lease remains; another caller reaching it first is not an
        # error, only the same outcome arriving twice.
        Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue
        Write-Status $false (Get-PowerLimitW)
    }
}
