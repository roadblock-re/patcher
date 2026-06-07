#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <winternl.h>

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

    uintptr_t target = (uintptr_t)targetAddress;

    // Limit search boundary to +/- 1GB (within the signed 32-bit offset limit of 2GB)
    uintptr_t minAddr = target > 0x40000000 ? target - 0x40000000 : (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t maxAddr = target < (UINTPTR_MAX - 0x40000000) ? target + 0x40000000 : (uintptr_t)si.lpMaximumApplicationAddress;

    minAddr -= (minAddr % si.dwAllocationGranularity);

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t current = minAddr;

    while (current < maxAddr) {
        if (!VirtualQueryEx(hProcess, (LPCVOID)current, &mbi, sizeof(mbi))) {
            break;
        }

        // Search for a free region large enough for the string
        if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
            LPVOID allocated = VirtualAllocEx(hProcess, mbi.BaseAddress, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (allocated) {
                return allocated;
            }
        }

        current = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }

    return NULL;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Error: Game path is not specified.\n");
        return 2;
    }

    char eveString[4096] = {};
    const char* gamePath = argv[argc - 1];
    uintptr_t instructionRVA = 0x1D12D55; // 24.6.1a

    FILE *fp = fopen("eve_string", "r");
    if (fp == 0) {
        fprintf(stderr, "Error opening file.\n");
        return 3;
    }

    if (fgets(eveString, sizeof(eveString), fp) == 0) {
        fprintf(stderr, "Error reading file.\n");
        fclose(fp);
        return 3;
    }

    fclose(fp);

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

    // Allocate memory close to the target instruction
    LPVOID instructionAddress = (LPVOID)(gameBase + instructionRVA);
    LPVOID stringAddressInGame = AllocateNearEx(pi.hProcess, instructionAddress, strlen(eveString) + 1);
    if (!stringAddressInGame) {
        fprintf(stderr, "Error: Failed to allocate memory near the instruction.\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    // Write the custom string into the allocated memory
    if (!WriteProcessMemory(pi.hProcess, stringAddressInGame, eveString, strlen(eveString) + 1, NULL)) {
        fprintf(stderr, "Error: Failed to write custom string to game memory.\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    // Calculate the 32-bit relative offset:
    // RIP points to the instruction directly after the 7-byte LEA instruction
    const uintptr_t instructionSize = 7;
    uintptr_t nextInstructionAddress = (uintptr_t)instructionAddress + instructionSize;
    int32_t newOffset = (int32_t)((uintptr_t)stringAddressInGame - nextInstructionAddress);

    // Write the 4-byte offset into the LEA instruction
    // Opcode format: [48 8d 15] [offset bytes (4 bytes starting at index +3)]
    LPVOID patchTargetAddress = (LPVOID)((uintptr_t)instructionAddress + 3);
    DWORD oldProtect;

    if (!VirtualProtectEx(pi.hProcess, instructionAddress, instructionSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        fprintf(stderr, "Error: Failed to modify memory page protection.\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    if (!WriteProcessMemory(pi.hProcess, patchTargetAddress, &newOffset, sizeof(newOffset), NULL)) {
        fprintf(stderr, "Error: Failed to patch instruction.\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    printf("Successfully patched instruction offset.\n");

    // Restore original memory protection flags
    VirtualProtectEx(pi.hProcess, instructionAddress, instructionSize, oldProtect, &oldProtect);

    ResumeThread(pi.hThread);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}