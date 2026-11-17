/* ir_bench: bench tool to emit a raw IR waveform via the hub's /dev/i2s.
 * Experimental; see docs/hardware/hub-ir-i2s.md. Not part of the shipped hub.
 * Waveform: alternating mark/space durations (us), carrier baked into a
 * 1-bit-per-I2S-bit-clock stream. Every knob is an argument. */
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define I2S_VOLUME 0x80044e20u
#define I2S_FREQ 0x80044e21u
#define I2S_DSIZE 0x80044e22u
#define I2S_MODE 0x80044e23u
#define I2S_START 0x80044e30u
#define I2S_DRAIN 0x80044e31u

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int rc_ioctl(int fd, const char *name, unsigned cmd, unsigned long arg) {
  double t0 = now_ms();
  int r = ioctl(fd, cmd, arg);
  printf("ioctl %-10s cmd=0x%08x arg=0x%lx -> %d%s%s (%.1f ms)\n", name, cmd, arg, r,
         r < 0 ? " errno=" : "", r < 0 ? strerror(errno) : "", now_ms() - t0);
  return r;
}

static void usage(void) {
  fprintf(stderr,
          "usage: ir_bench [--dry-run|--probe] --carrier HZ --durations a,b,c,... "
          "[--repeat N] [--ports M] [--duty PCT] [--swap 0|1] [--invert 0|1] "
          "[--n BITS] [--bit-ns NS] [--presilence-us US]\n"
          "  durations alternate mark,space,... in us; the list is one repeat\n");
}

typedef struct {
  uint8_t *buf;
  size_t cap, nbits;
} bits_t;

static void put_bit(bits_t *b, int v) {
  if (b->nbits / 8 >= b->cap) {
    fprintf(stderr, "stream exceeds ring capacity\n");
    exit(2);
  }
  if (v) b->buf[b->nbits / 8] |= (uint8_t)(0x80 >> (b->nbits % 8));
  b->nbits++;
}

int main(int argc, char **argv) {
  long carrier = 0, repeat = 1, ports = 0, duty = 33, swap = 1, invert = 0;
  long n_over = 0, bit_ns_over = 0, presil = 0;
  int dry = 0, probe = 0;
  const char *dur_s = NULL;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--dry-run")) dry = 1;
    else if (!strcmp(a, "--probe")) probe = 1;
    else if (!v) { usage(); return 2; }
    else if (!strcmp(a, "--carrier")) carrier = atol(v), i++;
    else if (!strcmp(a, "--durations")) dur_s = v, i++;
    else if (!strcmp(a, "--repeat")) repeat = atol(v), i++;
    else if (!strcmp(a, "--ports")) ports = strtol(v, NULL, 0), i++;
    else if (!strcmp(a, "--duty")) duty = atol(v), i++;
    else if (!strcmp(a, "--swap")) swap = atol(v), i++;
    else if (!strcmp(a, "--invert")) invert = atol(v), i++;
    else if (!strcmp(a, "--n")) n_over = atol(v), i++;
    else if (!strcmp(a, "--bit-ns")) bit_ns_over = atol(v), i++;
    else if (!strcmp(a, "--presilence-us")) presil = atol(v), i++;
    else { usage(); return 2; }
  }
  if (probe) {
    int fd = open("/dev/i2s", O_RDONLY);
    printf("open /dev/i2s O_RDONLY -> %d%s%s\n", fd, fd < 0 ? " errno=" : "",
           fd < 0 ? strerror(errno) : "");
    if (fd >= 0) close(fd);
    return fd < 0;
  }
  if (carrier < 9000 || carrier > 500000 || !dur_s || repeat < 1 || duty < 1) {
    usage();
    return 2;
  }
  long X = (1000000000L + carrier / 2) / carrier; /* carrier period ns */
  if (duty > 50) duty = 50;
  long N = n_over ? n_over : X < 2251 ? 2 : X < 2501 ? 4 : X < 3000 ? 8 : X < 12000 ? 10 : 12;
  long b = bit_ns_over ? bit_ns_over : (X - 1 + N) / N;
  long pulse = (duty * N + 99) / 100, space = N - pulse;

  long dur[1024];
  int nd = 0;
  for (char *s = strdup(dur_s), *t = strtok(s, ","); t && nd < 1024; t = strtok(NULL, ","))
    dur[nd++] = atol(t);

  bits_t bs = {calloc(393216, 1), 393216, 0};
  /* presilence */
  for (long acc = 0; acc < presil * 1000L - X / 2; acc += X)
    for (long k = 0; k < N; k++) put_bit(&bs, 0);
  for (long r = 0; r < repeat; r++) {
    long acc = 0, target = 0; /* ns, cumulative target vs emitted */
    for (int e = 0; e < nd; e++) {
      int mark = (e % 2) == 0;
      target += dur[e] * 1000L;
      while (acc < target - X / 2) {
        for (long k = 0; k < N; k++) put_bit(&bs, mark && k < pulse);
        acc += X;
      }
    }
    while (bs.nbits % 16) put_bit(&bs, 0); /* byte/word align each repeat */
  }
  for (int k = 0; k < 32; k++) put_bit(&bs, 0);
  while (bs.nbits % 16) put_bit(&bs, 0);
  size_t nbytes = bs.nbits / 8;
  if (invert)
    for (size_t i = 0; i < nbytes; i++) bs.buf[i] = (uint8_t)~bs.buf[i];
  if (swap)
    for (size_t i = 0; i + 1 < nbytes; i += 2) {
      uint8_t t = bs.buf[i]; bs.buf[i] = bs.buf[i + 1]; bs.buf[i + 1] = t;
    }
  double D = (double)b / 40.0; /* FP is soft-float; fine here */
  uint32_t ip = (uint32_t)D;
  uint32_t div = (ip << 16) | ((uint32_t)((D - ip) * 65536.0) & 0xffff);
  printf("carrier=%ldHz X=%ldns N=%ld duty=%ld%% pulse_bits=%ld space_bits=%ld\n", carrier, X, N,
         duty, pulse, space);
  printf("bit_ns=%ld div=0x%08x (%.4f) total_bits=%zu bytes=%zu duration=%.1fms ring=%.1f%% "
         "swap=%ld invert=%ld ports=0x%lx repeat=%ld\n",
         b, div, D, bs.nbits, nbytes, (double)bs.nbits * b / 1e6,
         100.0 * nbytes / 393216.0, swap, invert, ports, repeat);
  if (nbytes > 393216) { fprintf(stderr, "stream exceeds ring\n"); return 2; }
  if (dry) return 0;

  int fd = open("/dev/i2s", O_WRONLY);
  printf("open /dev/i2s O_WRONLY -> %d%s%s\n", fd, fd < 0 ? " errno=" : "",
         fd < 0 ? strerror(errno) : "");
  if (fd < 0) return 1;
  int bad = 0;
  bad |= rc_ioctl(fd, "DSIZE", I2S_DSIZE, 16) < 0;
  bad |= rc_ioctl(fd, "MODE", I2S_MODE, 2) < 0;
  bad |= rc_ioctl(fd, "VOLUME", I2S_VOLUME, 15) < 0;
  bad |= rc_ioctl(fd, "FREQ", I2S_FREQ, div) < 0;
  size_t off = 0;
  double tw = now_ms();
  while (!bad && off < nbytes) {
    ssize_t w = write(fd, bs.buf + off, nbytes - off);
    printf("write(%zu) -> %zd%s%s\n", nbytes - off, w, w < 0 ? " errno=" : "",
           w < 0 ? strerror(errno) : "");
    if (w < 0) { bad = 1; break; }
    off += (size_t)w;
  }
  printf("bytes written total %zu / %zu in %.1f ms\n", off, nbytes, now_ms() - tw);
  if (!bad) bad |= rc_ioctl(fd, "START", I2S_START, ((unsigned long)ports << 16)) < 0;
  if (!bad) bad |= rc_ioctl(fd, "DRAIN", I2S_DRAIN, 0) < 0;
  int cr = close(fd);
  printf("close -> %d\n", cr);
  return bad ? 1 : 0;
}
