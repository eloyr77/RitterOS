/*
 * Test: address-space randomization (step 22l). Prints where its pieces are:
 * its code and data (the program, placed at a random base since 22l), its
 * stack, its heap (malloc), an anonymous mapping, a library's function (in
 * libritteros, through dlsym) and a per-thread variable. The test runs it
 * twice: every address must differ between the two runs.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

static int data = 1;
static __thread int per_thread = 2;

int main(void)
{
    int local = 3;
    void *heap = malloc(64);
    void *map = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *library = dlsym(RTLD_DEFAULT, "snprintf");
    printf("aslrtest: code %p\n", (void *)main);
    printf("aslrtest: data %p\n", (void *)&data);
    printf("aslrtest: stack %p\n", (void *)&local);
    printf("aslrtest: heap %p\n", heap);
    printf("aslrtest: mmap %p\n", map);
    printf("aslrtest: library %p\n", library);
    printf("aslrtest: per-thread %p\n", (void *)&per_thread);
    printf("aslrtest: values %d %d %d\n", data, per_thread, local);
    return 0;
}
