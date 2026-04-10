# CustomHiveLoader

A Windows Kernel Driver template for mounting a custom registry hive at boot.

## Overview

This driver demonstrates how to load an arbitrary `.hiv` file into the registry
at `HKLM\<YourKey>` during the boot phase, before user-mode components start.
It is intended as a starting point — swap in your own hive path, mount point,
and any post-load logic you need.

## How It Works

1. **DriverEntry** runs at boot (`SERVICE_BOOT_START`)
2. `ZwLoadKeyEx` is resolved at runtime via `MmGetSystemRoutineAddress` since it
   is not exported from `ntoskrnl.lib` on recent Windows builds
3. `SE_RESTORE_PRIVILEGE` is enabled on the System process token via
   `ZwAdjustPrivilegesToken` — required by `ZwLoadKeyEx`
4. The hive file is mounted at the configured registry path
5. On driver unload, `ZwUnloadKey` detaches the hive cleanly

## Configuration

Edit the two defines at the top of `CustomHiveLoader.c`:

```c
// NT device path to your hive file on disk
#define HIVE_FILE_PATH     L"\\Device\\HarddiskVolume3\\Hives\\custom.hiv"

// Registry path where the hive will be anchored (must be under HKLM)
#define HIVE_LINK_KEY_PATH L"\\Registry\\Machine\\CustomHive"
```

## Requirements

- Windows 10 / 11 (tested on build 29560+)
- WDK + Visual Studio
- Test signing enabled: `bcdedit /set testsigning on`
- Hive file must exist before the driver loads

## Creating a Hive File

```cmd
mkdir C:\Hives
reg save HKLM\SOFTWARE C:\Hives\custom.hiv
```

Any valid registry hive works as a seed. The mounted hive is read-write at runtime.

## Installation

```cmd
copy CustomHiveLoader.sys C:\Windows\System32\drivers\
sc create CustomHiveLoader type= kernel start= boot error= normal binPath= "C:\Windows\System32\drivers\CustomHiveLoader.sys"
```

## Verify Mount

After reboot:
```cmd
reg query HKLM\CustomHive
```

## Uninstall

```cmd
sc stop CustomHiveLoader
sc delete CustomHiveLoader
```

## Notes

- `ZwLoadKeyEx` replaces the older `ZwLoadKey2` which was removed in recent
  Windows Insider builds
- `RtlAdjustPrivilege` was also removed — `ZwAdjustPrivilegesToken` is used
  instead with an explicit process token handle
- `STATUS_OBJECT_NAME_COLLISION` on load is treated as success to handle the
  case where the driver is restarted without a reboot
- Returning failure from `DriverEntry` will NOT trigger `DriverUnload`, so the
  hive is never left in a partially-mounted state
