/* Defaults de Lorica para Termux/Android: lo que funciona ahi queda activo sin variables.
 * setenv(..., 0): si el usuario define la variable, su valor manda.
 * Lo llama initialize_gl4es() (src/gl/init.c) antes de leer cualquier LIBGL_*. */
typedef int lorica_defaults_unit;
#ifdef __ANDROID__
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <dlfcn.h>
#include <android/log.h>

/* Registro de fallos (LIBGL_DEBUG o LORICA_CRASHLOG): backtrace de un SIGSEGV/SIGBUS en logcat (etiqueta LORICA); despues
 * la senal sigue al manejador anterior (Wine). Direcciones como modulo+offset (llvm-addr2line sobre la libGL sin strip). */
static struct sigaction g_old_segv, g_old_bus;
static void lorica_log_pc(int i, uintptr_t pc) {
    Dl_info di;
    if (dladdr((void*)pc, &di) && di.dli_fname)
        __android_log_print(ANDROID_LOG_ERROR, "LORICA", "frame #%02d pc %p  %s+0x%lx (%s)", i, (void*)pc, di.dli_fname,
                            (unsigned long)(pc - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
    else
        __android_log_print(ANDROID_LOG_ERROR, "LORICA", "frame #%02d pc %p", i, (void*)pc);
}

static void lorica_crash_handler(int sig, siginfo_t* si, void* uc) {
    __android_log_print(ANDROID_LOG_ERROR, "LORICA", "signal %d, fault address %p", sig, si ? si->si_addr : NULL);
#if defined(__aarch64__)
    {   /* pc y lr del contexto interrumpido, y la cadena de frame pointers (x29) */
        ucontext_t* u = (ucontext_t*)uc;
        uintptr_t fp = (uintptr_t)u->uc_mcontext.regs[29];
        int i;
        lorica_log_pc(0, (uintptr_t)u->uc_mcontext.pc);
        lorica_log_pc(1, (uintptr_t)u->uc_mcontext.regs[30]);
        for (i = 2; i < 26 && fp && !(fp & 7); i++) {
            uintptr_t lr = ((uintptr_t*)fp)[1];
            if (!lr) break;
            lorica_log_pc(i, lr);
            fp = ((uintptr_t*)fp)[0];
        }
    }
#else
    (void)uc;
#endif
    /* se restaura el manejador anterior: la instruccion que fallo se repite y lo atiende el */
    sigaction(sig, sig == SIGBUS ? &g_old_bus : &g_old_segv, NULL);
}

static void lorica_install_crash_log(void) {
    struct sigaction sa;
    if (!getenv("LIBGL_DEBUG") && !getenv("LORICA_CRASHLOG")) return;
    sa.sa_sigaction = lorica_crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

void lorica_android_defaults(void) {
    lorica_install_crash_log();
    const char *egl = "/system/lib64/libEGL.so", *gles = "/system/lib64/libGLESv2.so";
    if (access(egl, R_OK) != 0) { egl = "/system/lib/libEGL.so"; gles = "/system/lib/libGLESv2.so"; }
    if (access(egl, R_OK) == 0 && access(gles, R_OK) == 0) {
        setenv("LIBGL_EGL", egl, 0);
        setenv("LIBGL_GLES", gles, 0);
    }
    setenv("LIBGL_FB", "3", 0);
    setenv("LIBGL_GL", "43", 0);
}
#endif
