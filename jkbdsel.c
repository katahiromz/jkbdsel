/*
 * jkbdsel.c - Windows-like Japanese Keyboard Setup (native app)
 * Author: katahiromz
 * License: MIT or GPL
 *
 * This application is launched via the BootExecute value of smss.exe.
 * This application does not use Win32 API.
 *   Hankaku/Zenkaku (scancode 0x29) : 106 Japanese keyboard
 *   Space           (scancode 0x39) : 101 English keyboard
 *   S               (scancode 0x1F) : Other keyboard
 *   F3              (scancode 0x3D) : Skip
 *
 * It times out after 30 seconds.
 * If F3 is pressed or no key is pressed for 30 seconds, this app is skipped
 * and will not ask again (HKLM\SYSTEM\CurrentControlSet\Control\JKBDSEL\Done=1).
 * To show this screen again, delete that "Done" value.
 *
 * boot/bootdata/hivesys.inf:
 * HKLM,"SYSTEM\CurrentControlSet\Control\Session Manager","BootExecute",0x00010000,"autocheck autochk *","jkbdsel"
 */
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnt.h>
#include <winreg.h>
#include <wchar.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>

#define KEY_BREAK 0x01
#define KEY_E0    0x02

typedef struct tagKBD_INPUT_DATA
{
    USHORT UnitId;
    USHORT MakeCode;
    USHORT Flags;
    USHORT Reserved;
    ULONG  ExtraInformation;
} KBD_INPUT_DATA;

#define PARAMS_KEY L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\i8042prt\\Parameters"
#define DONE_KEY   L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\JKBDSEL"

typedef enum { C_106, C_101, C_OTHER, C_SKIP, C_TIMEOUT, C_ERROR } CHOICE;

static void Print(PCWSTR s)
{
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, s);
    NtDisplayString(&us);
}

static void Delay(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000LL; /* negative value = relative */
    NtDelayExecution(FALSE, &t);
}

/* ---------- Registry ---------- */
static NTSTATUS OpenKey(PCWSTR path, BOOLEAN create, HANDLE *h)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    ULONG disp;

    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (create)
        return NtCreateKey(h, KEY_ALL_ACCESS, &oa, 0, NULL,
                           REG_OPTION_NON_VOLATILE, &disp);
    return NtOpenKey(h, KEY_QUERY_VALUE, &oa);
}

static void SetDword(HANDLE h, PCWSTR name, ULONG v)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    NtSetValueKey(h, &n, 0, REG_DWORD, &v, sizeof(v));
}

static void SetSz(HANDLE h, PCWSTR name, PCWSTR v)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    NtSetValueKey(h, &n, 0, REG_SZ, (PVOID)v, (wcslen(v) + 1) * sizeof(WCHAR));
}

static void DelValue(HANDLE h, PCWSTR name)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    NtDeleteValueKey(h, &n);
}

static BOOLEAN FileExists(PCWSTR path)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS st;

    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = NtCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
                      NULL, FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (NT_SUCCESS(st))
        NtClose(h);
    return NT_SUCCESS(st);
}

static BOOLEAN IsUnattended(void)
{
    return FileExists(L"\\SystemRoot\\unattend.inf");
}

#define NLS_LANG_KEY \
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Nls\\Language"

static USHORT ReadLangId(HANDLE h, PCWSTR valueName)
{
    UNICODE_STRING n, s;
    ULONG buf[16], len, val = 0;
    PKEY_VALUE_PARTIAL_INFORMATION info = (PVOID)buf;

    RtlInitUnicodeString(&n, valueName);
    if (!NT_SUCCESS(NtQueryValueKey(h, &n, KeyValuePartialInformation,
                                    info, sizeof(buf), &len)))
        return 0;
    if (info->Type != REG_SZ || info->DataLength < sizeof(WCHAR))
        return 0;

    s.Buffer = (PWSTR)info->Data;
    s.Length = (USHORT)(info->DataLength - sizeof(WCHAR));   /* Excluding NUL */
    s.MaximumLength = (USHORT)info->DataLength;
    if (!NT_SUCCESS(RtlUnicodeStringToInteger(&s, 16, &val)))
        return 0;
    return (USHORT)val;
}

static BOOLEAN IsJapaneseSystem(void)
{
    HANDLE h;
    USHORT lang;
    BOOLEAN ja = FALSE;

    if (!NT_SUCCESS(OpenKey(NLS_LANG_KEY, FALSE, &h)))
        return FALSE;

    lang = ReadLangId(h, L"InstallLanguage");
    if (lang == 0)
        lang = ReadLangId(h, L"Default");

    if (lang != 0 && PRIMARYLANGID(lang) == LANG_JAPANESE)
        ja = TRUE;

    NtClose(h);
    return ja;
}

static BOOLEAN IsDone(void)
{
    HANDLE h;
    UNICODE_STRING n;
    ULONG buf[8], len;
    PKEY_VALUE_PARTIAL_INFORMATION info = (PVOID)buf;
    BOOLEAN done = FALSE;

    if (!NT_SUCCESS(OpenKey(DONE_KEY, FALSE, &h)))
        return FALSE;
    RtlInitUnicodeString(&n, L"Done");
    if (NT_SUCCESS(NtQueryValueKey(h, &n, KeyValuePartialInformation,
                                   info, sizeof(buf), &len))
        && info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)
        && *(PULONG)info->Data != 0)
        done = TRUE;
    NtClose(h);
    return done;
}

static BOOLEAN WriteDone(void)
{
    HANDLE h;
    if (!NT_SUCCESS(OpenKey(DONE_KEY, TRUE, &h)))
        return FALSE;
    SetDword(h, L"Done", 1);
    NtFlushKey(h);
    NtClose(h);
    return TRUE;
}

static BOOLEAN Apply(CHOICE c)
{
    HANDLE h;

    if (!NT_SUCCESS(OpenKey(PARAMS_KEY, TRUE, &h)))
        return FALSE;

    if (c == C_OTHER)
    {
        DelValue(h, L"OverrideKeyboardType");
        DelValue(h, L"OverrideKeyboardSubtype");
        DelValue(h, L"LayerDriver JPN");
    }
    else
    {
        SetDword(h, L"OverrideKeyboardType", 7);
        SetDword(h, L"OverrideKeyboardSubtype", (c == C_106) ? 2 : 0);
        SetSz(h, L"LayerDriver JPN", (c == C_106) ? L"kbd106.dll" : L"kbd101.dll");
    }
    NtFlushKey(h);
    NtClose(h);

    return WriteDone();
}

/* ---------- Key Input ---------- */

#define WAIT_SECONDS 30

static CHOICE WaitForChoice(void)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    static IO_STATUS_BLOCK iosb;
    static KBD_INPUT_DATA kd;
    IO_STATUS_BLOCK cancelIosb;
    HANDLE hKbd = NULL, hEvent = NULL;
    LARGE_INTEGER deadline;
    NTSTATUS st;
    CHOICE ret = C_ERROR;

    RtlInitUnicodeString(&name, L"\\Device\\KeyboardClass0");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);

    st = NtCreateFile(&hKbd, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                      FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      FILE_OPEN, 0, NULL, 0);
    if (!NT_SUCCESS(st))
        return C_ERROR;

    st = NtCreateEvent(&hEvent, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(st))
    {
        NtClose(hKbd);
        return C_ERROR;
    }

    NtQuerySystemTime(&deadline);
    deadline.QuadPart += (LONGLONG)WAIT_SECONDS * 10000000LL;

    for (;;)
    {
        NtResetEvent(hEvent, NULL);
        st = NtReadFile(hKbd, hEvent, NULL, NULL, &iosb,
                        &kd, sizeof(kd), NULL, NULL);

        if (st == STATUS_PENDING)
        {
            st = NtWaitForSingleObject(hEvent, FALSE, &deadline);
            if (st == STATUS_TIMEOUT)
            {
                LARGE_INTEGER t;
                t.QuadPart = -3LL * 10000000LL;

                NtCancelIoFile(hKbd, &cancelIosb);
                NtWaitForSingleObject(hEvent, FALSE, &t);
                ret = C_TIMEOUT;
                break;
            }
            st = iosb.Status;
        }

        if (!NT_SUCCESS(st))
            break;
        if ((kd.Flags & KEY_BREAK) || (kd.Flags & KEY_E0))
            continue;

        switch (kd.MakeCode)
        {
            case 0x29: ret = C_106;   goto done; /* Hankaku/Zenkaku */
            case 0x39: ret = C_101;   goto done; /* Space */
            case 0x1F: ret = C_OTHER; goto done; /* S */
            case 0x3D: ret = C_SKIP;  goto done; /* F3 */
        }
    }

done:
    NtClose(hEvent);
    NtClose(hKbd);
    return ret;
}

static void Reboot(void)
{
    BOOLEAN old;
    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);
    NtShutdownSystem(ShutdownReboot);
}

/* ---------- Entry ---------- */
VOID NTAPI NtProcessStartup(PPEB Peb)
{
    CHOICE c;

    UNREFERENCED_PARAMETER(Peb);

    if (!IsJapaneseSystem())
        goto quit;

    if (IsDone())
        goto quit;

    if (IsUnattended())
    {
        Print(L"JKBDSEL: Detected unattended\n");
        WriteDone();
        Delay(300);
        goto quit;
    }

    Print(L"\n\n  ReactOS Setup - Japanese Keyboard Type\n");
    Print(L"  -----------------------------\n\n");
    Print(L"  Press one of the following keys to identify your keyboard.\n\n");
    Print(L"    Hankaku/Zenkaku key : 106 Japanese keyboard\n");
    Print(L"    Space key           : 101 English keyboard\n");
    Print(L"    S key               : Other keyboard (use default)\n\n");
    Print(L"  F3 : Skip (will not ask again)\n");
    Print(L"  (No input for 30 seconds: skip and will not ask again)\n");

    c = WaitForChoice();

    if (c == C_TIMEOUT || c == C_SKIP)
    {
        WriteDone();
        goto quit;
    }

    if (c != C_106 && c != C_101 && c != C_OTHER)
        goto quit;

    if (Apply(c))
    {
        Print(L"\n  Keyboard setting saved. Restarting...\n");
        Reboot();
    }
    else
    {
        Print(L"\n  Failed to write the registry.\n");
        Delay(3000);
    }

quit:
    NtTerminateProcess(NtCurrentProcess(), 0);
}
