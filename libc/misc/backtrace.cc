#include <execinfo.h>

#if 0
// Implementation using libunwind.a. Unfortunately, despite linking with
// libwind.a I couldn't get this to link, because of some symbol
// dependency hell.
static int backtrace(void **buffer, int size) {
    unw_context_t context;
    unw_getcontext(&context);
    unw_cursor_t cursor;
    unw_init_local(&cursor, &context);

    int count = 0;
    while (count < size && unw_step(&cursor)) {
        unw_word_t ip;
        unw_get_reg (&cursor, UNW_REG_IP, &ip);
        buffer[count++] = (void*) ip;
    }
    return count;
}
#endif

#if 0
// An alternative, slightly more awkward but functioning implementation,
// using the _Unwind* functions which are used by the GCC runtime and
// supplied in libgcc_s.so). These also use libunwind.a internally, I believe.
//
// Unfortunately, while this implementation works nicely on our own code,
// it fails miserably when java.so is running. Supposedly it sees some
// unexpected frame pointers, but rather than let us see them and deal
// with them in worker(), it fails in _Unwind_Backtrace before calling
// worker(), I don't know why. So stay tuned for the third implementation,
// below.
#include <unwind.h>
struct worker_arg {
    void **buffer;
    int size;
    int pos;
    unsigned long prevcfa;
};

static _Unwind_Reason_Code worker (struct _Unwind_Context *ctx, void *data)
{
    struct worker_arg *arg = (struct worker_arg *) data;
    if (arg->pos >= 0) {
        arg->buffer[arg->pos] = (void *)_Unwind_GetIP(ctx);
        unsigned long cfa = _Unwind_GetCFA(ctx);
        if (arg->pos > 0 && arg->buffer[arg->pos-1] == arg->buffer[arg->pos]
                         && arg->prevcfa == cfa) {
            return _URC_END_OF_STACK;
        }
        arg->prevcfa = cfa;
    }
    if (++arg->pos == arg->size) {
        return _URC_END_OF_STACK;
    }
    return _URC_NO_REASON;
}

int backtrace(void **buffer, int size)
{
    // we start with arg.pos = -1 to skip the first frame (since backtrace()
    // is not supposed to include the backtrace() function itself).
    struct worker_arg arg { buffer, size, -1, 0 };
    _Unwind_Backtrace(worker, &arg);
    return arg.pos > 0 ? arg.pos : 0;
}
#endif

// This is the third implementation of backtrace(), using gcc's builtins
//__builtin_frame_address and __builtin_return_address. This is the ugliest
// of the three implementation, because these builtins awkwardly require
// constant arguments, so instead of a simple loop, we needed to resort
// to ugly C-preprocessor hacks. This implementation also requires
// compilation without -fomit-frame-pointer (currently our "release"
// build is compiled with it, so use our "debug" build).
//
// The good thing about this implementation is that the gcc builtin
// interface gives us a chance to ignore suspicious frame addresses before
// continuing to investigate them - and thus allows us to also backtrace
// when running Java code - some of it running from the heap and, evidently,
// contains broken frame information.
int backtrace(void **buffer, int size)
{
    for(unsigned int i = 0; i < (unsigned int) size; i++){
        // Unfortunately, __builtin_return_address requires a constant argument
        // so we resort to clever but ugly C-preprocessor hacks.
        switch (i) {
        // Officially, frame address == 0 means we reached the end of the call
        // chain. But it can also be garbage, and we stop if we see suspicious
        // frame addresses or return addresses (we must check the frame address
        // before looking in it for the return address).
#define sanepointer(x) ((unsigned long)(x)<<16>>16 == (unsigned long)(x))
#define TRY(i) case i: { \
        void *fa = __builtin_frame_address(i); \
        if (!fa || (unsigned long)fa < 0x200000000000UL || !sanepointer(fa)) \
            return i; \
        void *ra = __builtin_return_address(i); \
        if ((unsigned long)ra < 4096UL || (unsigned long)ra > 0x200000000000UL) \
            return i; \
        buffer[i] = ra; \
        } break;
#define TRY7(i) TRY(i) TRY(i+1) TRY(i+2) TRY(i+3) TRY(i+4) TRY(i+5) TRY(i+6)
        TRY7(7*0) TRY7(7*1) TRY7(7*2) TRY7(7*3) TRY7(7*4) TRY7(7*5)
        default: return i; // can happen if call is very deep and size > 42.
        }
    }
    return size; // if we haven't returned yet, entire size was filled.
}
