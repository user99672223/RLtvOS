// rlfex.cpp — FEXCore embedded on tvOS: initialisation and the Phase B
// "run a bare x86-64 function" path (no kernel: syscalls are logged and
// return -ENOSYS, exit parks the guest on a hlt).
#include "rlfex_internal.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/fextl/memory.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <unistd.h>

#ifndef RLFEX_FEX_VERSION
#define RLFEX_FEX_VERSION "FEX-59f85d6-rltvos"
#endif

namespace {

using namespace FEXCore;

// ------------------------------------------------------------------ fault guard
//
// A fault while a guest runs (bad test program, JIT bug) is turned into a
// reported failure instead of a process crash: our handler longjmps back to
// the runner. Every other fault is chained to the previously installed
// handler (the app's crash reporter).
struct FaultInfo {
  int Signal {};
  int Code {};
  uint64_t PC {};
  uint64_t Addr {};
  bool InJIT {};
};

thread_local bool t_GuardActive = false;
thread_local sigjmp_buf t_Jmp;
thread_local FaultInfo t_Fault;
thread_local Core::InternalThreadState* t_GuardThread = nullptr; // the FEX thread running under the guard
std::atomic<uint64_t> g_UnalignedFixups {0};
constexpr int GuardSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
struct sigaction g_Prev[NSIG] {};
bool g_GuardInstalled = false;

void GuardHandler(int Sig, siginfo_t* SI, void* UC) {
  if (t_GuardActive) {
    auto* U = static_cast<ucontext_t*>(UC);
    uint64_t PC = 0;
#if defined(__aarch64__) || defined(__arm64__)
    if (U && U->uc_mcontext) {
      PC = static_cast<uint64_t>(__darwin_arm_thread_state64_get_pc(U->uc_mcontext->__ss));
    }
#endif
    Core::InternalThreadState* Thread = t_GuardThread;
    const bool InJIT = Thread && PC && Thread->CTX->IsAddressInCodeBuffer(Thread, PC);
#if defined(__aarch64__) || defined(__arm64__)
    if (Sig == SIGBUS && InJIT && U && U->uc_mcontext) {
      // FEX's TSO loads/stores (ldapur/stlur) fault when an access crosses a
      // 16-byte boundary; Darwin reports that as SIGBUS. FEX back-patches the
      // instruction (plain access + half barrier, written through the pool's
      // RW alias) and tells us how far to move the pc — the same thing its
      // Linux frontend does in its SIGBUS handler. x0..x30 are contiguous in
      // the Darwin thread state (x[29], fp, lr).
      uint64_t* GPRs = reinterpret_cast<uint64_t*>(&U->uc_mcontext->__ss.__x[0]);
      const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
        Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, GPRs);
      if (Result.has_value()) {
        g_UnalignedFixups.fetch_add(1, std::memory_order_relaxed);
        __darwin_arm_thread_state64_set_pc_fptr(U->uc_mcontext->__ss, reinterpret_cast<void*>(PC + *Result));
        return;
      }
    }
#endif
    t_Fault.Signal = Sig;
    t_Fault.Code = SI ? SI->si_code : 0;
    t_Fault.Addr = SI ? reinterpret_cast<uint64_t>(SI->si_addr) : 0;
    t_Fault.PC = PC;
    t_Fault.InJIT = InJIT;
    t_GuardActive = false;
    siglongjmp(t_Jmp, 1);
  }
  struct sigaction& P = g_Prev[Sig];
  if (P.sa_flags & SA_SIGINFO) {
    if (P.sa_sigaction) {
      P.sa_sigaction(Sig, SI, UC);
    }
    return;
  }
  if (P.sa_handler == SIG_IGN) {
    return;
  }
  if (P.sa_handler != SIG_DFL && P.sa_handler != nullptr) {
    P.sa_handler(Sig);
    return;
  }
  signal(Sig, SIG_DFL);
  raise(Sig);
}

void InstallGuard() {
  if (g_GuardInstalled) {
    return;
  }
  for (int Sig : GuardSignals) {
    struct sigaction SA {};
    SA.sa_sigaction = GuardHandler;
    SA.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&SA.sa_mask);
    sigaction(Sig, &SA, &g_Prev[Sig]);
  }
  g_GuardInstalled = true;
}

// ------------------------------------------------------------------ handlers

uint8_t* g_HltPage = nullptr; // one `hlt`; exit syscalls park the guest here

// Drops every translation of a guest range (defined after the state below).
void InvalidateRange(Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length);

class RlSyscallHandler final : public HLE::SyscallHandler, public Allocator::FEXAllocOperators {
public:
  void InvalidateGuestCodeRange(Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override {
    InvalidateRange(Thread, Start, Length);
  }
  void HandleSyscall(Core::CpuStateFrame* Frame) override {
    auto& S = Frame->State;
    const uint64_t Nr = S.gregs[X86State::REG_RAX];
    const uint64_t A0 = S.gregs[X86State::REG_RDI];
    Calls.fetch_add(1, std::memory_order_relaxed);
    LastNr = Nr;
    switch (Nr) {
    case 39: // getpid
      S.gregs[X86State::REG_RAX] = 4242;
      break;
    case 60:  // exit
    case 231: // exit_group
      rlfex::Log("guest: exit(%llu) -> parking on hlt", (unsigned long long)A0);
      S.gregs[X86State::REG_RAX] = A0;
      S.rip = reinterpret_cast<uint64_t>(g_HltPage);
      return;
    default:
      rlfex::Log("guest: syscall %llu(0x%llx, 0x%llx, 0x%llx) = -ENOSYS (no kernel yet)", (unsigned long long)Nr,
                 (unsigned long long)A0, (unsigned long long)S.gregs[X86State::REG_RSI],
                 (unsigned long long)S.gregs[X86State::REG_RDX]);
      S.gregs[X86State::REG_RAX] = static_cast<uint64_t>(-38); // -ENOSYS
      break;
    }
    S.rip += 2; // past `syscall` (0F 05)
  }

  HLE::ExecutableRangeInfo QueryGuestExecutableRange(Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }

  std::optional<ExecutableFileSectionInfo> LookupExecutableFileSection(Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

  std::atomic<uint64_t> Calls {0};
  uint64_t LastNr {0};
};

class RlSignalDelegator final : public SignalDelegator, public Allocator::FEXAllocOperators {};

// ------------------------------------------------------------------ state

std::mutex g_Mutex; // init + one guest at a time
bool g_Init = false;
bool g_PlatformInit = false;
bool g_Poisoned = false;
char g_LastError[256] = "";
char g_InitJson[2048] = "";
uint64_t g_Runs = 0;
rlfex_exec_alloc_fn g_ExecAlloc = nullptr;
rlfex_exec_free_fn g_ExecFree = nullptr;
void* g_ProbeRW = nullptr;
void* g_ProbeRX = nullptr;
Context::Context* g_Ctx = nullptr;
RlSyscallHandler* g_Syscalls = nullptr;
RlSignalDelegator* g_Signals = nullptr;
HostFeatures g_Features {};
std::array<Core::CPUState::gdt_segment, 32> g_GDT {};

double NowMs() {
  struct timeval TV;
  gettimeofday(&TV, nullptr);
  return static_cast<double>(TV.tv_sec) * 1000.0 + static_cast<double>(TV.tv_usec) / 1000.0;
}

// FEX's lookup cache is shared between threads and keyed by guest address:
// a fresh thread running new code at an address a previous run used would
// execute the old translation (build-15 on the TV: every self-test ran the
// "add" block). Same call sequence as FEX's Linux frontend.
void InvalidateRange(Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {
  if (!g_Ctx || Length == 0) {
    return;
  }
  auto lk = FEXCore::GuardSignalDeferringSectionWithFallback(g_Ctx->GetCodeInvalidationMutex(), Thread);
  g_Ctx->InvalidateCodeBuffersCodeRange(Start, Length);
  if (Thread) {
    g_Ctx->InvalidateThreadCachedCodeRange(Thread, Start, Length);
  }
}

void SetError(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));
void SetError(const char* Fmt, ...) {
  va_list Ap;
  va_start(Ap, Fmt);
  vsnprintf(g_LastError, sizeof g_LastError, Fmt, Ap);
  va_end(Ap);
  rlfex::Log("rlfex: %s", g_LastError);
}

void InitSegments(Core::CPUState& State) {
  State.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = g_GDT.data();
  State.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = g_GDT.data();
  State.cs_idx = Core::CPUState::DEFAULT_USER_CS << 3;
  auto* CS = Core::CPUState::GetSegmentFromIndex(State, State.cs_idx);
  Core::CPUState::SetGDTBase(CS, 0);
  Core::CPUState::SetGDTLimit(CS, 0xF'FFFFU);
  CS->L = 1; // 64-bit code segment
  CS->D = 0;
  State.cs_cached = Core::CPUState::CalculateGDTBase(*CS);
}

struct Mapping {
  void* Ptr {nullptr};
  size_t Size {0};
  bool Map(size_t Bytes, int Prot) {
    Size = Bytes;
    Ptr = ::mmap(nullptr, Bytes, Prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Ptr == MAP_FAILED) {
      Ptr = nullptr;
      return false;
    }
    return true;
  }
  ~Mapping() {
    if (Ptr) {
      ::munmap(Ptr, Size);
    }
  }
};

struct RunResult {
  bool ReachedHlt {};
  uint64_t RAX {};
  uint64_t RIP {};
  double Ms {};
  FaultInfo Fault {};
  const char* Error {nullptr};
};

// Runs `Code` on a fresh guest thread. Caller holds g_Mutex.
RunResult RunBareLocked(const uint8_t* Code, size_t Len, uint64_t RDI, uint64_t RSI, uint64_t RDX) {
  RunResult R {};
  if (!g_Init) {
    R.Error = "not initialised";
    return R;
  }
  if (g_Poisoned) {
    R.Error = "a previous guest faulted; relaunch the app";
    return R;
  }
  Allocator::InitializeThread();
  const size_t PS = rlfex::HostPageSize();

  Mapping CodePage, Stack, TLS, CallRet;
  const size_t CodeBytes = ((Len + 64) + PS - 1) & ~(PS - 1);
  if (!CodePage.Map(CodeBytes, PROT_READ | PROT_WRITE)) {
    R.Error = "mmap code";
    return R;
  }
  memcpy(CodePage.Ptr, Code, Len);
  static_cast<uint8_t*>(CodePage.Ptr)[Len] = 0xF4; // safety hlt after the program
  constexpr size_t StackBytes = 1u << 20;
  if (!Stack.Map(StackBytes, PROT_READ | PROT_WRITE)) {
    R.Error = "mmap stack";
    return R;
  }
  if (!TLS.Map(PS, PROT_READ | PROT_WRITE)) {
    R.Error = "mmap tls";
    return R;
  }
  constexpr size_t CallRetSize = Core::InternalThreadState::CALLRET_STACK_SIZE;
  if (!CallRet.Map(CallRetSize + 2 * PS, PROT_NONE)) {
    R.Error = "mmap callret";
    return R;
  }
  uint8_t* CallRetBase = static_cast<uint8_t*>(CallRet.Ptr) + PS;
  if (::mprotect(CallRetBase, CallRetSize, PROT_READ | PROT_WRITE) != 0) {
    R.Error = "mprotect callret";
    return R;
  }

  Core::CPUState State {};
  State.rip = reinterpret_cast<uint64_t>(CodePage.Ptr);
  State.gregs[X86State::REG_RSP] = reinterpret_cast<uint64_t>(Stack.Ptr) + StackBytes - 64;
  State.gregs[X86State::REG_RDI] = RDI;
  State.gregs[X86State::REG_RSI] = RSI;
  State.gregs[X86State::REG_RDX] = RDX;
  State.fs_cached = reinterpret_cast<uint64_t>(TLS.Ptr);
  InitSegments(State);

  auto* Thread = g_Ctx->CreateThread(&State);
  if (!Thread) {
    R.Error = "CreateThread failed";
    return R;
  }
  Thread->CallRetStackBase = CallRetBase;
  Thread->CurrentFrame->State.callret_sp = reinterpret_cast<uint64_t>(CallRetBase) + CallRetSize / 4;
  // The code page is usually handed back at the address of the previous run.
  InvalidateRange(Thread, reinterpret_cast<uint64_t>(CodePage.Ptr), CodeBytes);

  const double T0 = NowMs();
  t_GuardThread = Thread;
  if (sigsetjmp(t_Jmp, 1) == 0) {
    t_GuardActive = true;
    g_Ctx->ExecuteThread(Thread);
    t_GuardActive = false;
    R.ReachedHlt = true;
  } else {
    R.ReachedHlt = false;
    R.Fault = t_Fault;
    g_Poisoned = true;
    rlfex::Log("rlfex: guest FAULT signal=%d code=%d pc=0x%llx%s addr=0x%llx", R.Fault.Signal, R.Fault.Code,
               (unsigned long long)R.Fault.PC, R.Fault.InJIT ? " (in JIT code)" : "", (unsigned long long)R.Fault.Addr);
  }
  t_GuardThread = nullptr;
  R.Ms = NowMs() - T0;
  R.RAX = Thread->CurrentFrame->State.gregs[X86State::REG_RAX];
  R.RIP = Thread->CurrentFrame->State.rip;
  if (R.ReachedHlt) {
    g_Ctx->DestroyThread(Thread);
  }
  ++g_Runs;
  return R;
}

size_t FormatRun(char* Out, size_t Cap, const RunResult& R) {
  if (R.Error) {
    return static_cast<size_t>(snprintf(Out, Cap, "{\"ok\":false,\"error\":\"%s\"}", R.Error));
  }
  return static_cast<size_t>(
    snprintf(Out, Cap,
             "{\"ok\":%s,\"rax\":%llu,\"rax_hex\":\"0x%llx\",\"rip\":\"0x%llx\",\"ms\":%.3f,\"exit\":\"%s\","
             "\"fault\":{\"signal\":%d,\"pc\":\"0x%llx\",\"addr\":\"0x%llx\"},\"syscalls\":%llu}",
             R.ReachedHlt ? "true" : "false", (unsigned long long)R.RAX, (unsigned long long)R.RAX, (unsigned long long)R.RIP,
             R.Ms, R.ReachedHlt ? "hlt" : "fault", R.Fault.Signal, (unsigned long long)R.Fault.PC,
             (unsigned long long)R.Fault.Addr, (unsigned long long)(g_Syscalls ? g_Syscalls->Calls.load() : 0)));
}

// ------------------------------------------------------------------ test programs

struct TestProgram {
  const char* Name;
  const uint8_t* Code;
  size_t Len;
  uint64_t RDI, RSI, RDX;
  uint64_t Expected;
};

// mov rax,rdi ; add rax,rsi ; add rax,rdx ; hlt
constexpr uint8_t P_Add[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x01, 0xd0, 0xf4};
// xor eax,eax ; 1: test rdi,rdi ; jz 2f ; add rax,rdi ; dec rdi ; jmp 1b ; 2: hlt
constexpr uint8_t P_Loop[] = {0x31, 0xc0, 0x48, 0x85, 0xff, 0x74, 0x08, 0x48, 0x01, 0xf8, 0x48, 0xff, 0xcf, 0xeb, 0xf3, 0xf4};
// cvtsi2sd xmm0,rdi ; cvtsi2sd xmm1,rsi ; mulsd xmm0,xmm1 ; cvttsd2si rax,xmm0 ; hlt
constexpr uint8_t P_Sse[] = {0xf2, 0x48, 0x0f, 0x2a, 0xc7, 0xf2, 0x48, 0x0f, 0x2a, 0xce, 0xf2, 0x0f,
                             0x59, 0xc1, 0xf2, 0x48, 0x0f, 0x2c, 0xc0, 0xf4};
// call 1f ; hlt ; 1: lea rax,[rdi+rsi] ; push rax ; pop rax ; ret
constexpr uint8_t P_Call[] = {0xe8, 0x01, 0x00, 0x00, 0x00, 0xf4, 0x48, 0x8d, 0x04, 0x37, 0x50, 0x58, 0xc3};
// mov [rsp-8],rdi ; mov [rsp-16],rsi ; mov rax,[rsp-8] ; add rax,[rsp-16] ; hlt
constexpr uint8_t P_Mem[] = {0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0x89, 0x74, 0x24, 0xf0, 0x48, 0x8b,
                             0x44, 0x24, 0xf8, 0x48, 0x03, 0x44, 0x24, 0xf0, 0xf4};
// mov eax,39 (getpid) ; syscall ; hlt
constexpr uint8_t P_Syscall[] = {0xb8, 0x27, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xf4};
// mov edi,7 ; mov eax,231 (exit_group) ; syscall ; hlt   -> parks on the hlt page with rax = 7
constexpr uint8_t P_Exit[] = {0xbf, 0x07, 0x00, 0x00, 0x00, 0xb8, 0xe7, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xf4};

constexpr TestProgram Tests[] = {
  {"add", P_Add, sizeof P_Add, 7, 8, 9, 24},
  {"loop", P_Loop, sizeof P_Loop, 100, 0, 0, 5050},
  {"sse", P_Sse, sizeof P_Sse, 6, 7, 0, 42},
  {"call", P_Call, sizeof P_Call, 20, 22, 0, 42},
  {"mem", P_Mem, sizeof P_Mem, 40, 2, 0, 42},
  {"syscall", P_Syscall, sizeof P_Syscall, 0, 0, 0, 4242},
  {"exit", P_Exit, sizeof P_Exit, 0, 0, 0, 7},
};

// ------------------------------------------------------------------ platform init
//
// Everything FEXCore needs before a Context exists. Shared by the
// bare-function runner (rlfex_init) and the fake kernel
// (rlfex_platform_init). Caller holds g_Mutex. On failure Stage names the
// step and g_LastError has the message.
bool PlatformInitLocked(const char*& Stage) {
  if (g_PlatformInit) {
    Stage = "";
    return true;
  }
  Stage = "log";
  rlfex::InstallLogHandlers();
  Allocator::SetupHooks(rlfex::HostPageSize());
  Allocator::InitializeThread();
  rlfex::InstallThreadHooks();
  Stage = "alloc";
  if (!rlfex::InstallAllocatorHooks(g_ExecAlloc, g_ExecFree)) {
    SetError("no exec allocator set (rlfex_set_exec_allocator)");
    return false;
  }
  Stage = "probe";
  void* ProbeRW = nullptr;
  void* ProbeRX = nullptr;
  if (!rlfex::ProbeExecAllocator(&ProbeRW, &ProbeRX)) {
    SetError("exec allocator probe failed: JIT pool not ready (rw=%p rx=%p)", ProbeRW, ProbeRX);
    return false;
  }
  g_ProbeRW = ProbeRW;
  g_ProbeRX = ProbeRX;
  Stage = "config";
  rlfex::InstallConfig();
  g_Features = rlfex::FetchHostFeatures();
  InstallGuard();
  g_PlatformInit = true;
  Stage = "";
  return true;
}

} // namespace

// ------------------------------------------------------------------ kernel-facing API

bool rlfex_platform_init(char* Err, size_t Cap) {
  std::lock_guard lk(g_Mutex);
  const char* Stage = "";
  if (PlatformInitLocked(Stage)) {
    if (Err && Cap) {
      Err[0] = 0;
    }
    return true;
  }
  if (Err && Cap) {
    snprintf(Err, Cap, "%s: %s", Stage, g_LastError);
  }
  return false;
}

const FEXCore::HostFeatures& rlfex_host_features() {
  return g_Features;
}

int rlfex_run_guarded(void (*Fn)(void*), void* Arg, FEXCore::Core::InternalThreadState* Thread, rlfex_fault_info* Out) {
  if (!g_GuardInstalled) {
    rlfex::Log("rlfex: run_guarded before platform init (no fault guard)");
  }
  t_GuardThread = Thread;
  if (sigsetjmp(t_Jmp, 1) == 0) {
    t_GuardActive = true;
    Fn(Arg);
    t_GuardActive = false;
    t_GuardThread = nullptr;
    return 0;
  }
  t_GuardActive = false;
  t_GuardThread = nullptr;
  if (Out) {
    Out->signal = t_Fault.Signal;
    Out->code = t_Fault.Code;
    Out->pc = t_Fault.PC;
    Out->addr = t_Fault.Addr;
    Out->in_jit = t_Fault.InJIT;
    Out->unaligned_fixups = g_UnalignedFixups.load(std::memory_order_relaxed);
  }
  return 1;
}

// ------------------------------------------------------------------ C API

extern "C" {

void rlfex_set_exec_allocator(rlfex_exec_alloc_fn Alloc, rlfex_exec_free_fn Free) {
  g_ExecAlloc = Alloc;
  g_ExecFree = Free;
}

void rlfex_set_log(rlfex_log_fn Fn) {
  rlfex::SetLogSink(Fn);
}

int rlfex_init(char* Out, size_t Cap) {
  std::lock_guard lk(g_Mutex);
  if (g_Init) {
    snprintf(Out, Cap, "%s", g_InitJson);
    return 1;
  }
  const double T0 = NowMs();
  const char* Stage = "";
  if (!PlatformInitLocked(Stage)) {
    snprintf(Out, Cap, "{\"ok\":false,\"stage\":\"%s\",\"error\":\"%s\"}", Stage, g_LastError);
    return 0;
  }
  Allocator::InitializeThread();
  auto Ctx = Context::Context::CreateNewContext(g_Features);
  if (!Ctx) {
    SetError("CreateNewContext failed");
    snprintf(Out, Cap, "{\"ok\":false,\"stage\":\"context\",\"error\":\"%s\"}", g_LastError);
    return 0;
  }
  g_Ctx = Ctx.release();
  g_Syscalls = new RlSyscallHandler();
  g_Signals = new RlSignalDelegator();
  g_Ctx->SetSignalDelegator(g_Signals);
  g_Ctx->SetSyscallHandler(g_Syscalls);
  g_Ctx->EnableExitOnHLT();
  if (!g_Ctx->InitCore()) {
    SetError("InitCore failed");
    snprintf(Out, Cap, "{\"ok\":false,\"stage\":\"initcore\",\"error\":\"%s\"}", g_LastError);
    return 0;
  }
  g_HltPage = static_cast<uint8_t*>(::mmap(nullptr, rlfex::HostPageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (g_HltPage == MAP_FAILED) {
    g_HltPage = nullptr;
    SetError("mmap hlt page failed");
    snprintf(Out, Cap, "{\"ok\":false,\"stage\":\"hltpage\",\"error\":\"%s\"}", g_LastError);
    return 0;
  }
  g_HltPage[0] = 0xF4;
  g_Init = true;
  const auto& F = g_Features;
  void* ProbeRW = g_ProbeRW;
  void* ProbeRX = g_ProbeRX;
  snprintf(g_InitJson, sizeof g_InitJson,
           "{\"ok\":true,\"version\":\"%s\",\"init_ms\":%.1f,\"page_size\":%zu,\"probe\":{\"rw\":\"%p\",\"rx\":\"%p\"},"
           "\"host_features\":{\"aes\":%u,\"crc\":%u,\"sha\":%u,\"lse\":%u,\"afp\":%u,\"rcpc\":%u,\"tso_imm9\":%u,"
           "\"pmull128\":%u,\"cssc\":%u,\"fcma\":%u,\"flagm\":%u,\"flagm2\":%u,\"frintts\":%u,\"rpres\":%u,\"ecv\":%u,"
           "\"wfxt\":%u,\"i8mm\":%u,\"dotprod\":%u,\"clzero\":%u,\"float_exceptions\":%u,\"avx\":%u,"
           "\"dcache_line\":%u,\"cpus\":%zu}}",
           RLFEX_FEX_VERSION, NowMs() - T0, rlfex::HostPageSize(), ProbeRW, ProbeRX, F.SupportsAES, F.SupportsCRC,
           F.SupportsSHA, F.SupportsAtomics, F.SupportsAFP, F.SupportsRCPC, F.SupportsTSOImm9, F.SupportsPMULL_128Bit,
           F.SupportsCSSC, F.SupportsFCMA, F.SupportsFlagM, F.SupportsFlagM2, F.SupportsFRINTTS, F.SupportsRPRES,
           F.SupportsECV, F.SupportsWFXT, F.SupportsI8MM, F.SupportsDotProd, F.SupportsCLZERO, F.SupportsFloatExceptions,
           F.SupportsAVX, 4u << F.DCacheLineLog2, F.CPUMIDRs.size());
  rlfex::Log("rlfex: initialised %s", g_InitJson);
  snprintf(Out, Cap, "%s", g_InitJson);
  return 1;
}

void rlfex_status_json(char* Out, size_t Cap) {
  snprintf(Out, Cap,
           "{\"initialised\":%s,\"runs\":%llu,\"poisoned\":%s,\"syscalls\":%llu,\"unaligned_fixups\":%llu,\"last_error\":\"%s\"}",
           g_Init ? "true" : "false", (unsigned long long)g_Runs, g_Poisoned ? "true" : "false",
           (unsigned long long)(g_Syscalls ? g_Syscalls->Calls.load() : 0),
           (unsigned long long)g_UnalignedFixups.load(std::memory_order_relaxed), g_LastError);
}

int rlfex_run_bare(const uint8_t* Code, size_t CodeLen, uint64_t RDI, uint64_t RSI, uint64_t RDX, char* Out, size_t Cap) {
  std::lock_guard lk(g_Mutex);
  if (!Code || CodeLen == 0 || CodeLen > (1u << 20)) {
    snprintf(Out, Cap, "{\"ok\":false,\"error\":\"bad code length\"}");
    return 0;
  }
  RunResult R = RunBareLocked(Code, CodeLen, RDI, RSI, RDX);
  FormatRun(Out, Cap, R);
  return R.ReachedHlt ? 1 : 0;
}

int rlfex_selftest(const char* Which, char* Out, size_t Cap) {
  std::lock_guard lk(g_Mutex);
  const bool All = !Which || !*Which || strcmp(Which, "all") == 0;
  size_t N = 0;
  int AllOk = 1, Any = 0;
  N += static_cast<size_t>(snprintf(Out + N, Cap - N, "{\"tests\":["));
  for (const auto& T : Tests) {
    if (!All && strcmp(Which, T.Name) != 0) {
      continue;
    }
    RunResult R = RunBareLocked(T.Code, T.Len, T.RDI, T.RSI, T.RDX);
    const bool Ok = R.ReachedHlt && !R.Error && R.RAX == T.Expected;
    AllOk = AllOk && Ok;
    if (N + 400 < Cap) {
      char Run[512];
      FormatRun(Run, sizeof Run, R);
      N += static_cast<size_t>(snprintf(Out + N, Cap - N, "%s{\"name\":\"%s\",\"ok\":%s,\"expected\":%llu,\"run\":%s}", Any ? "," : "",
                                        T.Name, Ok ? "true" : "false", (unsigned long long)T.Expected, Run));
    }
    Any = 1;
    rlfex::Log("rlfex: selftest %s %s rax=%llu expected=%llu %.2f ms", T.Name, Ok ? "ok" : "FAIL",
               (unsigned long long)R.RAX, (unsigned long long)T.Expected, R.Ms);
    if (!R.ReachedHlt) {
      break; // poisoned
    }
  }
  if (!Any) {
    snprintf(Out, Cap, "{\"ok\":false,\"error\":\"unknown test '%s'\"}", Which ? Which : "");
    return 0;
  }
  snprintf(Out + N, Cap - N, "],\"ok\":%s,\"initialised\":%s}", AllOk ? "true" : "false", g_Init ? "true" : "false");
  return AllOk;
}

} // extern "C"
