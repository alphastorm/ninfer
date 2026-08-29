Set-StrictMode -Version Latest

function Get-NInferAllowedOwnerSidValues {
    return @('S-1-5-18', 'S-1-5-32-544')
}

function ConvertFrom-NInferOwnerString([string]$Owner) {
    try { return [Security.Principal.SecurityIdentifier]::new($Owner).Value }
    catch {
        return [Security.Principal.NTAccount]::new($Owner).Translate(
            [Security.Principal.SecurityIdentifier]
        ).Value
    }
}

function Assert-NInferNoReparseAncestors([string]$Path) {
    $current = Get-Item -LiteralPath $Path -Force
    while ($null -ne $current) {
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "protected state path traverses a reparse point: $($current.FullName)"
        }
        $current = $current.Parent
    }
}

function Assert-NInferNoReparseTree([string]$Path) {
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($Path)
    while ($queue.Count -ne 0) {
        $directory = $queue.Dequeue()
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "protected state contains a reparse point: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $queue.Enqueue($item.FullName) }
        }
    }
}

function Assert-NInferProtectedAcl([string]$Path, [bool]$RequireProtectedDacl = $false) {
    $allowed = Get-NInferAllowedOwnerSidValues
    $acl = Get-Acl -LiteralPath $Path
    $ownerSid = ConvertFrom-NInferOwnerString ([string]$acl.Owner)
    if ($allowed -cnotcontains $ownerSid) {
        throw "protected state owner is not SYSTEM or Administrators: $Path"
    }
    if ($RequireProtectedDacl -and -not $acl.AreAccessRulesProtected) {
        throw "protected state inherits an external DACL: $Path"
    }
    $rules = $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])
    foreach ($rule in $rules) {
        if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            $allowed -cnotcontains [string]$rule.IdentityReference.Value) {
            throw "protected state grants access outside SYSTEM or Administrators: $Path"
        }
    }
}

function New-NInferProtectedDirectorySecurity {
    $administrators = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
        [Security.AccessControl.InheritanceFlags]::ObjectInherit
    $security = [Security.AccessControl.DirectorySecurity]::new()
    $security.SetOwner($administrators)
    $security.SetAccessRuleProtection($true, $false)
    foreach ($sid in @($administrators, $system)) {
        $security.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $sid,
                [Security.AccessControl.FileSystemRights]::FullControl,
                $inheritance,
                [Security.AccessControl.PropagationFlags]::None,
                [Security.AccessControl.AccessControlType]::Allow
            ))
    }
    return $security
}

function New-NInferProtectedDirectoryAtomic([string]$Path) {
    if (-not ('NInferProtectedDirectoryNative' -as [type])) {
        Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class NInferProtectedDirectoryNative {
    [StructLayout(LayoutKind.Sequential)]
    private struct SecurityAttributes {
        internal int Length;
        internal IntPtr SecurityDescriptor;
        [MarshalAs(UnmanagedType.Bool)] internal bool InheritHandle;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateDirectoryW(string path, ref SecurityAttributes attributes);

    public static void Create(string path, byte[] securityDescriptor) {
        if (securityDescriptor == null || securityDescriptor.Length == 0) {
            throw new ArgumentException("A security descriptor is required.", "securityDescriptor");
        }
        IntPtr descriptor = Marshal.AllocHGlobal(securityDescriptor.Length);
        try {
            Marshal.Copy(securityDescriptor, 0, descriptor, securityDescriptor.Length);
            SecurityAttributes attributes = new SecurityAttributes {
                Length = Marshal.SizeOf(typeof(SecurityAttributes)),
                SecurityDescriptor = descriptor,
                InheritHandle = false
            };
            if (!CreateDirectoryW(path, ref attributes)) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
        }
        finally {
            Marshal.FreeHGlobal(descriptor);
        }
    }
}
'@
    }

    $security = New-NInferProtectedDirectorySecurity
    try {
        [NInferProtectedDirectoryNative]::Create(
            $Path,
            $security.GetSecurityDescriptorBinaryForm()
        )
    }
    catch [ComponentModel.Win32Exception] {
        if ($_.Exception.NativeErrorCode -eq 183) {
            throw "protected state directory appeared during atomic creation: $Path"
        }
        throw
    }
}

function Assert-NInferTrustedCreationAncestor([string]$Path) {
    Assert-NInferNoReparseAncestors $Path
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    $programData = [IO.Path]::GetFullPath($env:ProgramData).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    if ([string]::Equals($fullPath, $programData, [StringComparison]::OrdinalIgnoreCase)) {
        $ownerSid = ConvertFrom-NInferOwnerString ([string](Get-Acl -LiteralPath $Path).Owner)
        if ((Get-NInferAllowedOwnerSidValues) -cnotcontains $ownerSid) {
            throw 'ProgramData is not owned by SYSTEM or Administrators'
        }
        return
    }
    Assert-NInferProtectedAcl $Path $true
}

function Initialize-NInferProtectedStateRoot([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $fullPath) {
        if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
            throw 'protected state root exists but is not a directory'
        }
        Assert-NInferNoReparseAncestors $fullPath
        Assert-NInferProtectedAcl $fullPath $true
        Assert-NInferNoReparseTree $fullPath
        return $fullPath
    }

    $missing = [Collections.Generic.Stack[string]]::new()
    $ancestor = $fullPath
    while (-not (Test-Path -LiteralPath $ancestor)) {
        $missing.Push($ancestor)
        $parent = Split-Path -Parent $ancestor
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -ceq $ancestor) {
            throw 'protected state has no existing trusted ancestor'
        }
        $ancestor = $parent
    }
    if (-not (Test-Path -LiteralPath $ancestor -PathType Container)) {
        throw 'protected state ancestor exists but is not a directory'
    }
    Assert-NInferTrustedCreationAncestor $ancestor
    $created = [Collections.Generic.List[string]]::new()
    try {
        while ($missing.Count -ne 0) {
            $next = $missing.Pop()
            New-NInferProtectedDirectoryAtomic $next
            $created.Add($next)
            Assert-NInferNoReparseAncestors $next
            Assert-NInferProtectedAcl $next $true
        }
        Assert-NInferNoReparseAncestors $fullPath
        Assert-NInferProtectedAcl $fullPath $true
    }
    catch {
        for ($index = $created.Count - 1; $index -ge 0; $index--) {
            Remove-Item -LiteralPath $created[$index] -Recurse -Force -ErrorAction SilentlyContinue
        }
        throw
    }
    return $fullPath
}

function Assert-NInferProtectedStateTree([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    Assert-NInferNoReparseAncestors $fullPath
    Assert-NInferNoReparseTree $fullPath
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($fullPath)
    while ($queue.Count -ne 0) {
        $directory = $queue.Dequeue()
        Assert-NInferProtectedAcl $directory ($directory -ceq $fullPath)
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force)) {
            Assert-NInferProtectedAcl $item.FullName
            if ($item.PSIsContainer) { $queue.Enqueue($item.FullName) }
        }
    }
}
