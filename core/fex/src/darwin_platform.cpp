// darwin_platform.cpp — the pieces FEX's Linux frontend (FEXLoader) normally
// supplies to FEXCore, for a tvOS host: logging, threads, config, host
// features, executable-memory hooks.
#include "rlfex_internal.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Threads.h>
#include <FEXCore/fextl/memory.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

// Apple's device compiler-rt does not ship __clear_cache, which
// __builtin___clear_cache lowers to on this toolchain (FEXCore's
// ClearICache). Provide it (weak, so a runtime that has one wins).
extern "C" __attribute__((weak)) void __clear_cache(void* Start, void* End) {
  sys_icache_invalidate(Start, static_cast<size_t>(static_cast<char*>(End) - static_cast<char*>(Start)));
}

namespace rlfex {

// ------------------------------------------------------------------ logging

static rlfex_log_fn g_LogSink = nullptr;
static std::mutex g_LogMutex;

void SetLogSink(rlfex_log_fn Fn) {
  g_LogSink = Fn;
}

void Log(const char* Fmt, ...) {
  char Buf[1024];
  va_list Ap;
  va_start(Ap, Fmt);
  vsnprintf(Buf, sizeof Buf, Fmt, Ap);
  va_end(Ap);
  std::lock_guard lk(g_LogMutex);
  if (g_LogSink) {
    g_LogSink(Buf);
  } else {
    fprintf(stderr, "%s\n", Buf);
  }
}

static void ThrowHandler(const char* Message) {
  Log("fex: ASSERT %s", Message);
}

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  const char* L = "?";
  switch (Level) {
  case LogMan::NONE: L = "none"; break;
  case LogMan::ASSERT: L = "assert"; break;
  case LogMan::ERROR: L = "error"; break;
  case LogMan::DEBUG: L = "debug"; break;
  case LogMan::INFO: L = "info"; break;
  default: break;
  }
  Log("fex[%s]: %s", L, Message);
}

void InstallLogHandlers() {
  LogMan::Throw::InstallHandler(ThrowHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
}

// ------------------------------------------------------------------ threads

namespace {
class PThread final : public FEXCore::Threads::Thread, public FEXCore::Allocator::FEXAllocOperators {
public:
  PThread(FEXCore::Threads::ThreadFunc Func, void* Arg, const char* Name)
    : Func {Func}
    , Arg {Arg} {
    if (Name) {
      strncpy(ThreadName, Name, sizeof ThreadName - 1);
    }
    pthread_attr_t Attr;
    pthread_attr_init(&Attr);
    pthread_attr_setstacksize(&Attr, 8u << 20);
    const int RC = pthread_create(&Handle, &Attr, &PThread::Entry, this);
    pthread_attr_destroy(&Attr);
    Started = (RC == 0);
    if (!Started) {
      Log("fex: pthread_create failed: %d", RC);
    }
  }

  bool joinable() override {
    return Started && !Joined && !Detached;
  }

  bool join(void** Ret) override {
    if (!joinable()) {
      return false;
    }
    void* R = nullptr;
    const int RC = pthread_join(Handle, &R);
    Joined = true;
    if (Ret) {
      *Ret = R;
    }
    return RC == 0;
  }

  bool detach() override {
    if (!joinable()) {
      return false;
    }
    Detached = true;
    return pthread_detach(Handle) == 0;
  }

  bool IsSelf() override {
    return Started && pthread_equal(pthread_self(), Handle);
  }

private:
  static void* Entry(void* P) {
    auto* Self = static_cast<PThread*>(P);
    if (Self->ThreadName[0]) {
      pthread_setname_np(Self->ThreadName);
    }
    FEXCore::Allocator::InitializeThread();
    return Self->Func(Self->Arg);
  }

  FEXCore::Threads::ThreadFunc Func;
  void* Arg;
  pthread_t Handle {};
  char ThreadName[64] {};
  bool Started {false};
  bool Joined {false};
  bool Detached {false};
};

fextl::unique_ptr<FEXCore::Threads::Thread>
CreateThread(FEXCore::Threads::ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags, const char* ThreadName) {
  return fextl::make_unique<PThread>(Func, Arg, ThreadName);
}

void CleanupAfterFork() {
  // No fork on tvOS.
}
} // namespace

void InstallThreadHooks() {
  FEXCore::Threads::Thread::SetInternalPointers(FEXCore::Threads::Pointers {
    .CreateThread = CreateThread,
    .CleanupAfterFork = CleanupAfterFork,
  });
}

// ------------------------------------------------------------------ config

namespace {
class MainLayer final : public FEXCore::Config::Layer {
public:
  MainLayer()
    : FEXCore::Config::Layer(FEXCore::Config::LayerType::LAYER_MAIN) {}

  void Load() override {
    using namespace FEXCore::Config;
    Set(CONFIG_IS64BIT_MODE, "1");
    Set(CONFIG_DISABLETELEMETRY, "1");
    Set(CONFIG_GDBSERVER, "0");
    Set(CONFIG_MULTIBLOCK, "1");
    Set(CONFIG_TSOENABLED, "1");
    Set(CONFIG_BLOCKJITNAMING, "0");
    Set(CONFIG_GLOBALJITNAMING, "0");
    Set(CONFIG_LIBRARYJITNAMING, "0");
    Set(CONFIG_ENABLECODECACHINGWIP, "0");
    Set(CONFIG_DISKCACHE, "0");
    Set(CONFIG_SILENTLOG, "0");
    Set(CONFIG_OUTPUTLOG, "stderr");
    Set(CONFIG_ROOTFS, "/");
  }
};

std::once_flag g_ConfigOnce;
} // namespace

void InstallConfig() {
  std::call_once(g_ConfigOnce, [] {
    FEXCore::Config::Initialize();
    FEXCore::Config::AddLayer(fextl::make_unique<MainLayer>());
    FEXCore::Config::Load();
    FEXCore::Config::ReloadMetaLayer();
  });
}

// ------------------------------------------------------------------ host features

namespace {
bool SysctlFlag(const char* Name) {
  int32_t Value = 0;
  size_t Size = sizeof Value;
  if (::sysctlbyname(Name, &Value, &Size, nullptr, 0) != 0) {
    return false;
  }
  return Value != 0;
}

int64_t SysctlI64(const char* Name, int64_t Default) {
  int64_t Value = 0;
  size_t Size = sizeof Value;
  if (::sysctlbyname(Name, &Value, &Size, nullptr, 0) != 0) {
    return Default;
  }
  if (Size == sizeof(int32_t)) {
    return *reinterpret_cast<int32_t*>(&Value);
  }
  return Value;
}
} // namespace

FEXCore::HostFeatures FetchHostFeatures() {
  FEXCore::HostFeatures F {};
  const auto S = SysctlFlag;

  F.SupportsCacheMaintenanceOps = true;
  F.SupportsAES = S("hw.optional.arm.FEAT_AES");
  F.SupportsCRC = S("hw.optional.arm.FEAT_CRC32");
  F.SupportsSHA = S("hw.optional.arm.FEAT_SHA1") && S("hw.optional.arm.FEAT_SHA256");
  F.SupportsAtomics = S("hw.optional.arm.FEAT_LSE");
  F.SupportsRAND = S("hw.optional.arm.FEAT_RNG");
  F.SupportsAFP = S("hw.optional.arm.FEAT_AFP");
  F.SupportsRCPC = S("hw.optional.arm.FEAT_LRCPC");
  F.SupportsTSOImm9 = S("hw.optional.arm.FEAT_LRCPC2");
  F.SupportsPMULL_128Bit = S("hw.optional.arm.FEAT_PMULL");
  F.SupportsCSSC = S("hw.optional.arm.FEAT_CSSC");
  F.SupportsFCMA = S("hw.optional.arm.FEAT_FCMA");
  F.SupportsFlagM = S("hw.optional.arm.FEAT_FlagM");
  F.SupportsFlagM2 = S("hw.optional.arm.FEAT_FlagM2");
  F.SupportsFRINTTS = S("hw.optional.arm.FEAT_FRINTTS");
  F.SupportsRPRES = S("hw.optional.arm.FEAT_RPRES");
  F.SupportsECV = S("hw.optional.arm.FEAT_ECV");
  F.SupportsWFXT = S("hw.optional.arm.FEAT_WFxT");
  F.SupportsI8MM = S("hw.optional.arm.FEAT_I8MM");
  F.SupportsDotProd = S("hw.optional.arm.FEAT_DotProd");
  F.SupportsSVE128 = false; // no SVE on Apple silicon
  F.SupportsSVE256 = false;
  F.SupportsSVEBitPerm = false;
  F.SupportsMOPS = false;
  F.SupportsAVX = true; // emulated with 2x128; FEX enables it on every host
  F.SupportsAES256 = F.SupportsAVX && F.SupportsAES;
  F.SupportsPreserveAllABI = false;
  F.PreferZVAForVZero = false;
  F.Supports3DNow = true;
  F.SupportsSSE4a = false;
  F.SupportsCPUIndexInTPIDRRO = false;
  F.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux; // plain (non-Wow64/EC) code paths
  F.ProcessPID = static_cast<uint32_t>(getpid());

#if defined(__aarch64__) || defined(__arm64__)
  // DCZID_EL0 is EL0-readable on XNU (unlike CTR_EL0 / MIDR_EL1).
  uint64_t DCZID = 0;
  __asm volatile("mrs %0, dczid_el0" : "=r"(DCZID));
  const bool DZP = (DCZID >> 4) & 1;
  const uint64_t ZVABlock = 4ull << (DCZID & 0xF);
  F.SupportsCLZERO = !DZP && ZVABlock == 64;

  // Float exception trapping: try to enable the trap bits (RAZ/WI when unsupported).
  constexpr uint64_t TrapBits = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 15);
  uint64_t FPCR = 0;
  __asm volatile("mrs %0, fpcr" : "=r"(FPCR));
  const uint64_t Orig = FPCR;
  __asm volatile("msr fpcr, %0" ::"r"(FPCR | TrapBits));
  __asm volatile("mrs %0, fpcr" : "=r"(FPCR));
  F.SupportsFloatExceptions = (FPCR & TrapBits) == TrapBits;
  __asm volatile("msr fpcr, %0" ::"r"(Orig));
#endif

  // Cache line: Apple reports one size for D$ and I$ (128 on A-series).
  const int64_t CacheLine = SysctlI64("hw.cachelinesize", 64);
  uint32_t Log2 = 0;
  while ((4ll << Log2) < CacheLine && Log2 < 15) {
    ++Log2;
  }
  F.DCacheLineLog2 = Log2 & 0xF;

  // MIDR is not readable; CPUID uses the list for the core count and for
  // errata matching (Apple implementer 0x61: nothing matches). Fake an
  // A15 (Avalanche) part number so the list is non-empty.
  const int64_t NCPU = SysctlI64("hw.ncpu", 1);
  F.CPUMIDRs.resize(static_cast<size_t>(NCPU > 0 ? NCPU : 1));
  for (auto& M : F.CPUMIDRs) {
    M = 0x611F0230u;
  }
  return F;
}

// ------------------------------------------------------------------ allocator hooks

namespace {
rlfex_exec_alloc_fn g_ExecAlloc = nullptr;
rlfex_exec_free_fn g_ExecFree = nullptr;
std::atomic<uint64_t> g_ExecAllocs {0};

void* HookMmap(void* Addr, size_t Len, int Prot, int Flags, int FD, off_t Off) {
  if ((Prot & PROT_EXEC) && FD == -1) {
    void* RX = nullptr;
    void* RW = g_ExecAlloc ? g_ExecAlloc(Len, &RX) : nullptr;
    const uint64_t N = g_ExecAllocs.fetch_add(1) + 1;
    if (!RW || !RX) {
      Log("fex: exec alloc #%llu of %zu bytes FAILED (pool not ready or full)", (unsigned long long)N, Len);
      errno = ENOMEM;
      return MAP_FAILED;
    }
    FEXCore::Allocator::RegisterDualMapping(RW, RX, Len);
    Log("fex: exec alloc #%llu %zu KB rw=%p rx=%p", (unsigned long long)N, Len >> 10, RW, RX);
    return RW;
  }
  return ::mmap(Addr, Len, Prot, Flags, FD, Off);
}

int HookMunmap(void* Ptr, size_t Len) {
  void* RX = nullptr;
  size_t Size = 0;
  if (FEXCore::Allocator::UnregisterDualMapping(Ptr, &RX, &Size)) {
    if (g_ExecFree) {
      g_ExecFree(Ptr, Size);
    }
    Log("fex: exec free %zu KB rw=%p", Size >> 10, Ptr);
    return 0;
  }
  return ::munmap(Ptr, Len);
}
} // namespace

bool InstallAllocatorHooks(rlfex_exec_alloc_fn Alloc, rlfex_exec_free_fn Free) {
  g_ExecAlloc = Alloc;
  g_ExecFree = Free;
  FEXCore::Allocator::mmap = HookMmap;
  FEXCore::Allocator::munmap = HookMunmap;
  return Alloc != nullptr;
}

bool ProbeExecAllocator(void** RW, void** RX) {
  if (RW) {
    *RW = nullptr;
  }
  if (RX) {
    *RX = nullptr;
  }
  void* P = FEXCore::Allocator::VirtualAlloc(HostPageSize(), true);
  if (!P || P == MAP_FAILED) {
    return false;
  }
  void* X = FEXCore::Allocator::GetExecutableAddress(P);
  if (RW) {
    *RW = P;
  }
  if (RX) {
    *RX = X;
  }
  FEXCore::Allocator::VirtualFree(P, HostPageSize());
  return X != nullptr && X != P;
}

size_t HostPageSize() {
  static const size_t PS = [] {
    const long V = sysconf(_SC_PAGESIZE);
    return V > 0 ? static_cast<size_t>(V) : 16384u;
  }();
  return PS;
}

} // namespace rlfex
