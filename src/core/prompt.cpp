#include "core/prompt.h"

#include <windows.h>

namespace ng {

char PromptTray(const std::string& app, const std::string& dest, int port) {
    const wchar_t* kPipe = L"\\\\.\\pipe\\neuralguard";
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10; ++i) {
        pipe = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return 0;  // tray not running
        WaitNamedPipeW(kPipe, 2000);
    }
    if (pipe == INVALID_HANDLE_VALUE) return 0;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    std::string msg = app + "\t" + dest + "\t" + std::to_string(port);
    DWORD wr = 0;
    WriteFile(pipe, msg.c_str(), (DWORD)msg.size(), &wr, nullptr);

    char decision = 0;
    DWORD rd = 0;
    ReadFile(pipe, &decision, 1, &rd, nullptr);
    CloseHandle(pipe);
    return (rd == 1) ? decision : 0;
}

}  // namespace ng
