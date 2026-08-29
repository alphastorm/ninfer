Set-StrictMode -Version Latest

if (-not ('NInferAtomicProtectedDirectoryV1' -as [type])) {
    Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class NInferAtomicProtectedDirectoryV1 {
    [StructLayout(LayoutKind.Sequential)]
    private struct SecurityAttributes {
        public int Length;
        public IntPtr SecurityDescriptor;
        [MarshalAs(UnmanagedType.Bool)] public bool InheritHandle;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(
        string text, uint revision, out IntPtr descriptor, out uint descriptorBytes);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateDirectoryW(string path, ref SecurityAttributes attributes);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    public static void Create(string path) {
        const string sddl = "O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
        IntPtr descriptor;
        uint descriptorBytes;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, 1, out descriptor, out descriptorBytes)) {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        try {
            SecurityAttributes attributes = new SecurityAttributes {
                Length = Marshal.SizeOf<SecurityAttributes>(),
                SecurityDescriptor = descriptor,
                InheritHandle = false,
            };
            if (!CreateDirectoryW(path, ref attributes)) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
        }
        finally {
            LocalFree(descriptor);
        }
    }
}
'@
}

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

function Test-NInferWriteCapableRights([Security.AccessControl.FileSystemRights]$Rights) {
    $writeMask = [Security.AccessControl.FileSystemRights]::WriteData -bor
        [Security.AccessControl.FileSystemRights]::AppendData -bor
        [Security.AccessControl.FileSystemRights]::CreateFiles -bor
        [Security.AccessControl.FileSystemRights]::CreateDirectories -bor
        [Security.AccessControl.FileSystemRights]::WriteAttributes -bor
        [Security.AccessControl.FileSystemRights]::WriteExtendedAttributes -bor
        [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [Security.AccessControl.FileSystemRights]::TakeOwnership
    return ($Rights -band $writeMask) -ne 0
}

function Assert-NInferTrustedMigrationAcl([string]$Path, [bool]$RequireProtectedDacl = $false) {
    $allowed = Get-NInferAllowedOwnerSidValues
    $acl = Get-Acl -LiteralPath $Path
    $raw = [Security.AccessControl.RawSecurityDescriptor]::new(
        $acl.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access)
    )
    if ($null -eq $raw.DiscretionaryAcl) {
        throw "protected state has a NULL DACL: $Path"
    }
    $ownerSid = ConvertFrom-NInferOwnerString ([string]$acl.Owner)
    if ($allowed -cnotcontains $ownerSid) {
        throw "protected state owner is not SYSTEM or Administrators: $Path"
    }
    if ($RequireProtectedDacl -and -not $acl.AreAccessRulesProtected) {
        throw "protected state inherits an external DACL: $Path"
    }
    $rules = $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])
    foreach ($rule in $rules) {
        if ($rule.AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow -or
            -not (Test-NInferWriteCapableRights $rule.FileSystemRights)) {
            continue
        }
        if ($allowed -cnotcontains [string]$rule.IdentityReference.Value) {
            throw "protected state grants write-capable access outside SYSTEM or Administrators: $Path"
        }
    }
}

function Assert-NInferProtectedStateRoot([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    Assert-NInferNoReparseAncestors $fullPath
    Assert-NInferProtectedAcl $fullPath $true
}

function Assert-NInferProtectedAcl([string]$Path, [bool]$RequireProtectedDacl = $false) {
    $allowed = Get-NInferAllowedOwnerSidValues
    $acl = Get-Acl -LiteralPath $Path
    $raw = [Security.AccessControl.RawSecurityDescriptor]::new(
        $acl.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access)
    )
    if ($null -eq $raw.DiscretionaryAcl) {
        throw "protected state has a NULL DACL: $Path"
    }
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

function Set-NInferProtectedRootAcl([string]$Path) {
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
    Set-Acl -LiteralPath $Path -AclObject $security
}

function Set-NInferProtectedFileAcl([string]$Path) {
    $administrators = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $security = [Security.AccessControl.FileSecurity]::new()
    $security.SetOwner($administrators)
    $security.SetAccessRuleProtection($true, $false)
    foreach ($sid in @($administrators, $system)) {
        $security.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $sid,
                [Security.AccessControl.FileSystemRights]::FullControl,
                [Security.AccessControl.AccessControlType]::Allow
            ))
    }
    Set-Acl -LiteralPath $Path -AclObject $security
}

function New-NInferProtectedDirectoryAtomic([string]$Path) {
    [NInferAtomicProtectedDirectoryV1]::Create([IO.Path]::GetFullPath($Path))
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
        Assert-NInferTrustedMigrationAcl $fullPath $true
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
        for ($index = $created.Count - 1; $index -ge 0; --$index) {
            $createdPath = $created[$index]
            try {
                if (Test-Path -LiteralPath $createdPath -PathType Container) {
                    Assert-NInferNoReparseAncestors $createdPath
                    Assert-NInferProtectedAcl $createdPath $true
                    if (@(Get-ChildItem -LiteralPath $createdPath -Force).Count -eq 0) {
                        [IO.Directory]::Delete($createdPath, $false)
                    }
                }
            }
            catch {
                # Leave any nonempty, replaced, or no-longer-trusted path untouched. Cleanup never
                # traverses a path that another principal could have raced into existence.
            }
        }
        throw
    }
    return $fullPath
}

function Protect-NInferRetainedSecretAcls([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    Assert-NInferNoReparseAncestors $fullPath
    Assert-NInferNoReparseTree $fullPath

    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($fullPath)
    while ($queue.Count -ne 0) {
        $directory = $queue.Dequeue()
        Assert-NInferTrustedMigrationAcl $directory ($directory -ceq $fullPath)
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force)) {
            Assert-NInferTrustedMigrationAcl $item.FullName
            if ($item.PSIsContainer) { $queue.Enqueue($item.FullName) }
        }
    }

    $secrets = Join-Path $fullPath 'secrets'
    if (-not (Test-Path -LiteralPath $secrets)) { return }
    if (-not (Test-Path -LiteralPath $secrets -PathType Container)) {
        throw 'release secrets path exists but is not a directory'
    }
    Assert-NInferNoReparseTree $secrets
    $secretDirectories = [Collections.Generic.List[string]]::new()
    $secretDirectories.Add($secrets)
    foreach ($item in @(Get-ChildItem -LiteralPath $secrets -Force -Recurse)) {
        if ($item.PSIsContainer) {
            $secretDirectories.Add($item.FullName)
        }
        else {
            Set-NInferProtectedFileAcl $item.FullName
        }
    }
    for ($index = $secretDirectories.Count - 1; $index -ge 0; --$index) {
        Set-NInferProtectedRootAcl $secretDirectories[$index]
    }
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
