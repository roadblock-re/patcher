#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <winternl.h>
#include <stdbool.h>

bool ReadFileLine(const char *filename, char *buffer, int size) {
    FILE *fp = fopen(filename, "r");
    if (fp == 0) {
        fprintf(stderr, "Error opening file %s.\n", filename);
        return false;
    }

    if (fgets(buffer, size - 1, fp) == 0) {
        fprintf(stderr, "Error reading file %s.\n", filename);
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

void GetExecutablePath(char *buffer, size_t size) {
    char cwd[16384] = {};
    if (getcwd(cwd, sizeof(cwd) - 1) != NULL) {
        fprintf(stdout, "Current working directory: %s\n", cwd);
    }

    snprintf(buffer, size - 1, "%s\\%s", cwd, "Asphalt9_Steam_x64_rtl.exe");
}

uintptr_t GetSuspendedProcessBaseAddress(HANDLE hProcess) {
    PROCESS_BASIC_INFORMATION pbi;
    ULONG returnLength = 0;
    NTSTATUS status = NtQueryInformationProcess(
        hProcess,
        ProcessBasicInformation,
        &pbi,
        sizeof(pbi),
        &returnLength
    );

    if (!NT_SUCCESS(status)) {
        return 0;
    }

    uintptr_t pebAddressIBA = (uintptr_t) pbi.PebBaseAddress + 0x10;

    uintptr_t imageBaseAddress = 0;
    if (ReadProcessMemory(hProcess, (LPCVOID) pebAddressIBA, &imageBaseAddress, sizeof(imageBaseAddress), NULL)) {
        return imageBaseAddress;
    }

    return 0;
}

LPVOID AllocateNearEx(HANDLE hProcess, LPVOID targetAddress, SIZE_T size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);

    uintptr_t target = (uintptr_t) targetAddress;

    // Limit search boundary to +/- 1GB (within the signed 32-bit offset limit of 2GB)
    uintptr_t minAddr = target > 0x40000000 ? target - 0x40000000 : (uintptr_t) si.lpMinimumApplicationAddress;
    uintptr_t maxAddr = target < (UINTPTR_MAX - 0x40000000)
                            ? target + 0x40000000
                            : (uintptr_t) si.lpMaximumApplicationAddress;

    minAddr -= (minAddr % si.dwAllocationGranularity);

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t current = minAddr;

    while (current < maxAddr) {
        if (!VirtualQueryEx(hProcess, (LPCVOID) current, &mbi, sizeof(mbi))) {
            break;
        }

        // Search for a free region large enough for the string
        if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
            LPVOID allocated = VirtualAllocEx(hProcess, mbi.BaseAddress, size, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
            if (allocated) {
                return allocated;
            }
        }

        current = (uintptr_t) mbi.BaseAddress + mbi.RegionSize;
    }

    return NULL;
}

bool PatchInstructionString(HANDLE hProcess, uintptr_t gameBase, uintptr_t instructionRVA, const char *customString) {
    // Allocate memory close to the target instruction
    LPVOID instructionAddress = (LPVOID) (gameBase + instructionRVA);

    LPVOID stringAddressInGame = AllocateNearEx(hProcess, instructionAddress, strlen(customString) + 1);
    if (!stringAddressInGame) {
        fprintf(stderr, "Error: Failed to allocate memory near the instruction.\n");
        return false;
    }

    // Write the custom string into the allocated memory
    if (!WriteProcessMemory(hProcess, stringAddressInGame, customString, strlen(customString) + 1, NULL)) {
        fprintf(stderr, "Error: Failed to write custom string to game memory.\n");
        return false;
    }

    // Calculate the 32-bit relative offset:
    // RIP points to the instruction directly after the 7-byte LEA instruction
    const uintptr_t instructionSize = 7;
    uintptr_t nextInstructionAddress = (uintptr_t) instructionAddress + instructionSize;
    int32_t newOffset = (int32_t) ((uintptr_t) stringAddressInGame - nextInstructionAddress);

    // Write the 4-byte offset into the LEA instruction
    // Opcode format: [48 8d 15] [offset bytes (4 bytes starting at index +3)]
    LPVOID patchTargetAddress = (LPVOID) ((uintptr_t) instructionAddress + 3);
    DWORD oldProtect;

    if (!VirtualProtectEx(hProcess, instructionAddress, instructionSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        fprintf(stderr, "Error: Failed to modify memory page protection.\n");
        return false;
    }

    if (!WriteProcessMemory(hProcess, patchTargetAddress, &newOffset, sizeof(newOffset), NULL)) {
        VirtualProtectEx(hProcess, instructionAddress, instructionSize, oldProtect, &oldProtect);
        fprintf(stderr, "Error: Failed to patch instruction.\n");
        return false;
    }

    // Restore original memory protection flags
    VirtualProtectEx(hProcess, instructionAddress, instructionSize, oldProtect, &oldProtect);
    return true;
}

bool GetLaunchParameters(int argc, char **argv, char gamePath[16384], char eveString[16384]) {
    if (argc >= 2) {
        snprintf(gamePath, 16384 - 1, "%s", argv[argc - 1]);
    } else {
        GetExecutablePath(gamePath, 16384);
    }

    int fileExists = access(gamePath, F_OK) == 0;
    if (!fileExists) {
        fprintf(stderr, "Error: Executable file is not found.\n");
        return false;
    }

    if (!ReadFileLine("eve_string", eveString, 16384)) {
        return false;
    }

    return true;
}

typedef struct {
    const char *gameVersion;
    uintptr_t baseAddress;
    uintptr_t eveUrl;
    uintptr_t windowTitle;
} GAME_OFFSETS;

GAME_OFFSETS OFFSETS[] = {
    {"24.0.1f", 0x140000000, 0x141EC7A85, 0x1402E0F94,},
    {"24.6.1a", 0x140000000, 0x141D12D55, 0x14022AB21,},
    {"47.1.0a", 0x140000000, 0x141D768B5, 0x1402473C1,},
    // Legacy A9 is complicated...
};

#define GAME_PKG_MARKER "com.gameloft.asphalt9"

bool IsGameVersion(const char *string) {
    size_t i = 0;
    size_t len = strlen(string);

    for (; i < len; ++i) {
        if (string[i] >= '0' && string[i] <= '9') {
            continue;
        }
        if (string[i] == '.') {
            ++i;
            break;
        }
        return false;
    }

    if (i >= len - 1) {
        return false;
    }

    for (; i < len; ++i) {
        if (string[i] >= '0' && string[i] <= '9') {
            continue;
        }
        if (string[i] == '.') {
            ++i;
            break;
        }
        return false;
    }

    if (i >= len - 1) {
        return false;
    }

    for (; i < len; ++i) {
        if (string[i] >= '0' && string[i] <= '9') {
            continue;
        }
        if (string[i] >= 'a' && string[i] <= 'z') {
            return true;
        }
        return false;
    }

    return false;
}

bool ScanGameVersion(char gamePath[16384], GAME_OFFSETS **offsets) {
    FILE *fp = fopen(gamePath, "rb");
    if (fp == 0) {
        fprintf(stderr, "Error opening file %s.\n", gamePath);
        return false;
    }

    // hack: should be a safe call.
    fseek(fp, 0x4000000, SEEK_SET);

    bool foundPackageMarker = false;
    char buf[sizeof(GAME_PKG_MARKER)] = {};
    size_t bufLI = sizeof(buf) - 1;

    int c;
    while ((c = fgetc(fp)) != EOF) {
        memmove(buf, buf + 1, bufLI);
        buf[bufLI] = (char) c;

        if (memcmp(buf, GAME_PKG_MARKER, sizeof(GAME_PKG_MARKER)) == 0) {
            foundPackageMarker = true;
            break;
        }
    }

    if (!foundPackageMarker) {
        fclose(fp);
        return false;
    }

    memset(buf, 0, sizeof(buf));
    size_t nonZeroChars = 0;

    const char *gameVersion = 0;
    while ((c = fgetc(fp)) != EOF) {
        memmove(buf, buf + 1, bufLI);
        buf[bufLI] = (char) c;

        if (c != '\0') {
            ++nonZeroChars;
            continue;
        }

        if (nonZeroChars > bufLI) {
            nonZeroChars = bufLI;
        }

        const char* sp = buf + bufLI - nonZeroChars;
        if (nonZeroChars != 0 && IsGameVersion(sp)) {
            gameVersion = sp;
            break;
        }

        memset(buf, 0, sizeof(buf));
        nonZeroChars = 0;
    }

    if (gameVersion == 0) {
        fclose(fp);
        return false;
    }

    fprintf(stdout, "Game version is %s.\n", gameVersion);

    fclose(fp);

    for (size_t i = 0; i < sizeof(OFFSETS) / sizeof(OFFSETS[0]); ++i) {
        GAME_OFFSETS *i_offsets = &OFFSETS[i];

        if (strcmp(i_offsets->gameVersion, gameVersion) == 0) {
            *offsets = i_offsets;
            return true;
        }
    }

    return false;
}

int main(int argc, char **argv) {
    // first braces are game version, second are graphics API
    char windowTitle[16384] = {};
    if (!ReadFileLine("window_title", windowTitle, sizeof(windowTitle))) {
        fprintf(stdout, "Ignoring. Using default value.\n");
        snprintf(windowTitle, sizeof(windowTitle) - 1, "%s", "Roadblock // {} // {}");
    }

    char gamePath[16384] = {};
    char eveString[16384] = {};
    if (!GetLaunchParameters(argc, argv, gamePath, eveString)) {
        return 1;
    }

    fprintf(stdout, "Scanning game binary version.\n");

    GAME_OFFSETS *offsets = 0;
    if (!ScanGameVersion(gamePath, &offsets)) {
        fprintf(stderr, "Error: Failed to find out game version.\n");
        return 1;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    // Initialize structures to zero
    SecureZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    SecureZeroMemory(&pi, sizeof(pi));

    // Launch the game in a suspended state
    if (!CreateProcessA(gamePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "Error: Failed to launch game.\n");
        return 1;
    }

    // Query the base address of the suspended game
    uintptr_t gameBase = GetSuspendedProcessBaseAddress(pi.hProcess);
    if (gameBase == 0) {
        fprintf(stderr, "Error: Failed to query game base address.\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    printf("Game base address: 0x%llx\n", (unsigned long long) gameBase);

    if (offsets->eveUrl && !PatchInstructionString(pi.hProcess, gameBase,
                                                   offsets->eveUrl - offsets->baseAddress, eveString)) {
        fprintf(stderr, "Error: Failed to patch Eve URL string.\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    if (offsets->windowTitle && !PatchInstructionString(pi.hProcess, gameBase,
                                                        offsets->windowTitle - offsets->baseAddress, windowTitle)) {
        fprintf(stderr, "Error: Failed to patch window title string.\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    printf("Successfully patched instruction offset.\n");

    ResumeThread(pi.hThread);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
