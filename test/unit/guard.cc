#include "memory.h"
#include "fiber.h"
#include "unit.h"

static struct fiber_attr default_attr;

static void
sigsegf_handler(int signo)
{
	note("signal handler called");
	exit(0);
}

static int __attribute__((noinline))
stack_break_f(char *ptr)
{
	char block[2048];
	/*
	 * Make sum volatile to prevent a compiler from
	 * optimizing away call to this function.
	 */
	volatile char sum = 0;
	memset(block, 0xff, 2048);
	sum += block[(unsigned char) block[4]];
	ptrdiff_t stack_diff = ptr > block ? ptr - block : block - ptr;
	if (stack_diff < (ptrdiff_t)default_attr.stack_size)
		sum += stack_break_f(ptr);
	return sum;
}

static int
main_f(va_list ap)
{
	stack_t stack;
	stack.ss_flags = 0;
	stack.ss_size = SIGSTKSZ;
	stack.ss_sp = xmalloc(stack.ss_size);
	sigaltstack(&stack, NULL);
	struct sigaction sa;
	sa.sa_handler = sigsegf_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	int res = stack_break_f((char *)&stack);

	ev_break(loop(), EVBREAK_ALL);
	free(stack.ss_sp);
	return res;
}

int main()
{
	memory_init();
	fiber_init(fiber_cxx_invoke);
	fiber_attr_create(&default_attr);
	struct fiber *fmain = fiber_new_xc("main", main_f);
	fiber_wakeup(fmain);
	ev_run(loop(), 0);
	fiber_free();
	memory_free();
	fail("signal handler was not executed", "");
	return 0;
}
