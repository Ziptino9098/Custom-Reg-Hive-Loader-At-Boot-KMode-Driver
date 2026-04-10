#include <ntifs.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define HIVE_FILE_PATH     L"\\Device\\HarddiskVolume3\\Windows\\System32\\EFI"
#define HIVE_LINK_KEY_PATH L"\\Registry\\Machine\\EFI"

// ---------------------------------------------------------------------------
// Typedefs — resolved at runtime via MmGetSystemRoutineAddress
// ---------------------------------------------------------------------------

typedef NTSTATUS(NTAPI* pfnZwLoadKeyEx)(
    _In_      POBJECT_ATTRIBUTES TargetKey,
    _In_      POBJECT_ATTRIBUTES SourceFile,
    _In_      ULONG              Flags,
    _In_opt_  HANDLE             TrustClassKey,
    _In_opt_  HANDLE             Event,
    _In_opt_  ACCESS_MASK        DesiredAccess,
    _Out_opt_ PHANDLE            RootHandle,
    _Reserved_ PVOID             Reserved
    );

// ZwUnloadKey and ZwAdjustPrivilegesToken are in ntoskrnl.lib
NTSTATUS ZwUnloadKey(_In_ POBJECT_ATTRIBUTES TargetKey);

// ZwAdjustPrivilegesToken is exported — declare manually (not in DDK headers)
NTSTATUS ZwAdjustPrivilegesToken(
    _In_      HANDLE            TokenHandle,
    _In_      BOOLEAN           DisableAllPrivileges,
    _In_opt_  PTOKEN_PRIVILEGES NewState,
    _In_      ULONG             BufferLength,
    _Out_opt_ PTOKEN_PRIVILEGES PreviousState,
    _Out_opt_ PULONG            ReturnLength
);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static UNICODE_STRING g_LinkKeyName = RTL_CONSTANT_STRING(HIVE_LINK_KEY_PATH);
static UNICODE_STRING g_HiveFilePath = RTL_CONSTANT_STRING(HIVE_FILE_PATH);

static pfnZwLoadKeyEx g_ZwLoadKeyEx = NULL;

// ---------------------------------------------------------------------------
// ResolveRoutines
// ---------------------------------------------------------------------------
static NTSTATUS ResolveRoutines(void)
{
    UNICODE_STRING name;

    RtlInitUnicodeString(&name, L"ZwLoadKeyEx");
    g_ZwLoadKeyEx = (pfnZwLoadKeyEx)MmGetSystemRoutineAddress(&name);
    if (!g_ZwLoadKeyEx)
    {
        KdPrint(("[CustomHiveLoader] Failed to resolve ZwLoadKeyEx\n"));
        return STATUS_ENTRYPOINT_NOT_FOUND;
    }

    KdPrint(("[CustomHiveLoader] ZwLoadKeyEx resolved OK\n"));
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// EnableRestorePrivilege
//
// Opens the current process token and enables SE_RESTORE_PRIVILEGE (18)
// using ZwAdjustPrivilegesToken, which IS in ntoskrnl.lib on this build.
// ---------------------------------------------------------------------------
static NTSTATUS EnableRestorePrivilege(void)
{
    HANDLE tokenHandle = NULL;

    // Open the token of the current process (System process at boot)
    NTSTATUS status = ZwOpenProcessTokenEx(
        ZwCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        OBJ_KERNEL_HANDLE,
        &tokenHandle
    );

    if (!NT_SUCCESS(status))
    {
        KdPrint(("[CustomHiveLoader] ZwOpenProcessTokenEx failed: 0x%08X\n", status));
        return status;
    }

    // Build a TOKEN_PRIVILEGES structure for SE_RESTORE_PRIVILEGE (LUID = {18, 0})
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid.LowPart = SE_RESTORE_PRIVILEGE; // 18
    tp.Privileges[0].Luid.HighPart = 0;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    status = ZwAdjustPrivilegesToken(
        tokenHandle,
        FALSE,          // do not disable all
        &tp,
        sizeof(tp),
        NULL,           // don't need previous state
        NULL
    );

    ZwClose(tokenHandle);

    if (!NT_SUCCESS(status))
    {
        KdPrint(("[CustomHiveLoader] ZwAdjustPrivilegesToken failed: 0x%08X\n", status));
    }
    else
    {
        KdPrint(("[CustomHiveLoader] SeRestorePrivilege enabled\n"));
    }

    return status;
}

// ---------------------------------------------------------------------------
// LoadCustomHive
// ---------------------------------------------------------------------------
static NTSTATUS LoadCustomHive(void)
{
    OBJECT_ATTRIBUTES targetAttrs;
    OBJECT_ATTRIBUTES sourceAttrs;

    InitializeObjectAttributes(
        &targetAttrs,
        &g_LinkKeyName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    InitializeObjectAttributes(
        &sourceAttrs,
        &g_HiveFilePath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );


    NTSTATUS status = g_ZwLoadKeyEx(
        &targetAttrs,
        &sourceAttrs,
        0,
        NULL,
        NULL,
        0,
        NULL,
        NULL
    );

    if (NT_SUCCESS(status))
    {
        KdPrint(("[CustomHiveLoader] Hive mounted at %wZ\n", &g_LinkKeyName));
    }
    else if (status == STATUS_OBJECT_NAME_COLLISION)
    {
        KdPrint(("[CustomHiveLoader] Hive already mounted, continuing\n"));
        status = STATUS_SUCCESS;
    }
    else
    {
        KdPrint(("[CustomHiveLoader] ZwLoadKeyEx failed: 0x%08X\n", status));
    }

    return status;
}

// ---------------------------------------------------------------------------
// DriverUnload
// ---------------------------------------------------------------------------
static VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    OBJECT_ATTRIBUTES keyAttrs;

    InitializeObjectAttributes(
        &keyAttrs,
        &g_LinkKeyName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    NTSTATUS status = ZwUnloadKey(&keyAttrs);

    if (NT_SUCCESS(status))
        KdPrint(("[CustomHiveLoader] Hive unloaded\n"));
    else
        KdPrint(("[CustomHiveLoader] ZwUnloadKey failed: 0x%08X\n", status));
}

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrint(("[CustomHiveLoader] DriverEntry\n"));

    DriverObject->DriverUnload = DriverUnload;

    // 1. Resolve ZwLoadKeyEx at runtime
    NTSTATUS status = ResolveRoutines();
    if (!NT_SUCCESS(status))
        return status;

    // 2. Enable SeRestorePrivilege via ZwAdjustPrivilegesToken
    status = EnableRestorePrivilege();
    if (!NT_SUCCESS(status))
        return status;

    // 3. Mount the hive
    return LoadCustomHive();
}
