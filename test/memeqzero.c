#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define N 100000

char buf1[1024], buf2[1024], buf3[1024], buf4[65536];

static __attribute__((noinline)) bool memeqzero1(const void *data, size_t length)
{
    const unsigned char *p = data;

    while (length) {
        if (*p)
            return false;
        p++;
        length--;
    }
    return true;
}

static __attribute__((noinline)) bool memeqzero2(const void *data, size_t length)
{
    const unsigned char *p = data;
    static unsigned long zeroes[16];

    while (length > sizeof(zeroes)) {
        if (memcmp(zeroes, p, sizeof(zeroes)))
            return false;
        p += sizeof(zeroes);
        length -= sizeof(zeroes);
    }
    return memcmp(zeroes, p, length) == 0;
}

static __attribute__((noinline)) bool memeqzero3_rusty(const void *data, size_t length)
{
    const unsigned char *p = data;
    const unsigned long zero = 0;
    size_t pre;
    pre = (size_t)p % sizeof(unsigned long);
    if (pre) {
        size_t n = sizeof(unsigned long) - pre;
        if (n > length)
            n = length;
        if (memcmp(p, &zero, n) != 0)
            return false;
        p += n;
        length -= n;
    }
    while (length > sizeof(zero)) {
        if (*(unsigned long *)p != zero)
            return false;
        p += sizeof(zero);
        length -= sizeof(zero);
    }
    return memcmp(&zero, p, length) == 0;
}

static __attribute__((noinline)) bool memeqzero3_paolo(const void *data, size_t length)
{
    const unsigned char *p = data;
    unsigned long word;

    while (length & (sizeof(word) - 1)) {
        if (*p)
            return false;
        p++;
        length--;
    }
    while (length) {
        memcpy(&word, p, sizeof(word));
        if (word)
            return false;
        p += sizeof(word);
        length -= sizeof(word);
    }
    return true;
}

static __attribute__((noinline)) bool memeqzero4_rusty(const void *data, size_t length)
{
    const unsigned char *p = data;
    size_t len;

    /* Check first 16 bytes manually */
    for (len = 0; len < 16; len++) {
        if (!length)
            return true;
        if (*p)
            return false;
        p++;
        length--;
    }

    /* Now we know that's zero, memcmp with self. */
    return memcmp(data, p, length) == 0;
}

static __attribute__((noinline)) bool memeqzero4_paolo(const void *data, size_t length)
{
    const unsigned char *p = data;
    unsigned long word;

    if (!length)
        return true;

    while (__builtin_expect(length & (sizeof(word) - 1), 0)) {
        if (*p)
            return false;
        p++;
        length--;
        if (!length)
            return true;
    }

    /* We must always read one byte or word, even if everything is aligned!
     * Otherwise, memcmp(data, data, length) is trivially true.
     */
    for (;;) {
        memcpy(&word, p, sizeof(word));
        if (word)
            return false;
	if (__builtin_expect(length & (16 - sizeof(word)), 0) == 0)
            break;
        p += sizeof(word);
        length -= sizeof(word);
	if (!length)
            return true;
    }

     /* Now we know that's zero, memcmp with self. */
     return memcmp(data, p, length) == 0;
}

static inline unsigned long rdtsc() {
    unsigned long cycles;
    asm volatile("rdtsc; shlq $32, %%rdx; movl %%eax, %%eax; orq %%rdx, %%rax " : "=A"(cycles));
    return cycles;
}

static int bench(char *buf, int size, bool(*memeqzero)(const void *, size_t))
{
    int i = N;
    int count;

    unsigned long start = rdtsc();
    while(i--) asm volatile("" : : "r" (memeqzero(buf, size)));
    unsigned long end = rdtsc();
    i = count = 3000000000.0 * N / (end - start);

    start = rdtsc();
    while(i--) asm volatile("" : : "r" (memeqzero(buf, size)));
    end = rdtsc();
    return (end - start) / count;
}

static __attribute__((__flatten__))
int run(bool(*memeqzero)(const void *, size_t))
{
    printf ("%d\t%d\t%d\t%d\n",
	    bench(buf1, 1, memeqzero), bench(buf2, 8, memeqzero),
	    bench(buf3, 512, memeqzero), bench(buf4, 65536, memeqzero));
}

int main()
{
    run(memeqzero1);
    run(memeqzero2);
    run(memeqzero3_rusty);
    run(memeqzero3_paolo);
    run(memeqzero4_rusty);
    run(memeqzero4_paolo);
}