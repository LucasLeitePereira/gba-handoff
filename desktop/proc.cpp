#include "proc.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>

namespace {

std::wstring widen(const std::string &s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

// Quoting rules of CommandLineToArgvW / the MSVC runtime.
void append_quoted(std::wstring &cl, const std::wstring &a)
{
	if (!cl.empty()) cl += L' ';
	if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) { cl += a; return; }
	cl += L'"';
	for (size_t i = 0;; i++) {
		size_t bs = 0;
		while (i < a.size() && a[i] == L'\\') { i++; bs++; }
		if (i == a.size()) { cl.append(bs * 2, L'\\'); break; }
		if (a[i] == L'"') { cl.append(bs * 2 + 1, L'\\'); cl += L'"'; }
		else { cl.append(bs, L'\\'); cl += a[i]; }
	}
	cl += L'"';
}

} // namespace

namespace proc {

int run(const std::vector<std::string> &argv, int timeout_ms, std::string &out)
{
	out.clear();
	if (argv.empty()) return -1;
	std::wstring cl;
	for (const std::string &a : argv) append_quoted(cl, widen(a));

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	HANDLE rd = nullptr, wr = nullptr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

	// Only these two handles are inherited. Without the list, a long-lived child
	// (the adb server daemon) would also inherit our listening socket and keep
	// the port busy after we exit.
	SIZE_T asz = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &asz);
	std::string abuf(asz, '\0');
	auto *attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)&abuf[0];
	HANDLE inherit[2] = { nul, wr };
	bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &asz) &&
	          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = SW_HIDE;
	si.StartupInfo.hStdInput = nul;
	si.StartupInfo.hStdOutput = wr;
	si.StartupInfo.hStdError = wr;
	si.lpAttributeList = attrs;
	PROCESS_INFORMATION pi{};
	if (ok)
		ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE,
		                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
		                    &si.StartupInfo, &pi) != 0;
	if (asz) DeleteProcThreadAttributeList(attrs);
	CloseHandle(wr);
	if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (!ok) { CloseHandle(rd); return -1; }

	auto drain = [&] {
		for (;;) {
			DWORD avail = 0, got = 0;
			if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || !avail) return;
			char buf[1024];
			if (!ReadFile(rd, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &got, nullptr) || !got) return;
			out.append(buf, got);
		}
	};
	int rc = -2;
	DWORD waited = 0;
	for (;;) {
		DWORD w = WaitForSingleObject(pi.hProcess, 20);
		drain();
		if (w == WAIT_OBJECT_0) {
			DWORD code = 0;
			GetExitCodeProcess(pi.hProcess, &code);
			rc = (int)code;
			break;
		}
		waited += 20;
		if ((int)waited >= timeout_ms) { TerminateProcess(pi.hProcess, 1); break; }
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(rd);
	return rc;
}

} // namespace proc

#else // POSIX

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

namespace proc {

static long now_ms()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int run(const std::vector<std::string> &argv, int timeout_ms, std::string &out)
{
	out.clear();
	if (argv.empty()) return -1;
	int fds[2];
	if (pipe(fds) != 0) return -1;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 2);

	std::vector<char *> args;
	for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
	args.push_back(nullptr);
	pid_t pid = 0;
	int err = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (err != 0) { close(fds[0]); return -1; }

	auto drain = [&] {
		char buf[1024];
		for (;;) {
			pollfd p{fds[0], POLLIN, 0};
			if (poll(&p, 1, 0) <= 0) return;
			ssize_t n = read(fds[0], buf, sizeof buf);
			if (n <= 0) return;
			out.append(buf, (size_t)n);
		}
	};
	const long deadline = now_ms() + timeout_ms;
	int rc = -2;
	for (;;) {
		pollfd p{fds[0], POLLIN, 0};
		poll(&p, 1, 20);
		drain();
		if (p.revents & POLLHUP) usleep(5000); // pipe closed but child alive: do not spin
		int st = 0;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid) {
			drain();
			rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			break;
		}
		if (now_ms() >= deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			break;
		}
	}
	close(fds[0]);
	return rc;
}

} // namespace proc

#endif
