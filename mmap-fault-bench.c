/*
 * mmap-fault-bench.c — SSRFS mmap fault concurrency benchmark
 *
 * Spawns N worker processes, each mmap'ing a distinct per-worker file and
 * performing random-access reads (exercises .fault / folio_read) or writes
 * (exercises .page_mkwrite / folio_prepare_write).
 *
 * Build: cc -D_GNU_SOURCE -O2 -Wall -Wextra -o mmap-fault-bench mmap-fault-bench.c
 * Usage: mmap-fault-bench -w <workers> -s <duration_sec> -f <filesize> -d <test_dir> <read|write|rw>
 */
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include <signal.h>

struct bench_cfg {
	const char	*directory;
	const char	*mode;		/* "read", "write", "rw" */
	unsigned int	 workers;
	unsigned int	 duration;
	uintmax_t	 filesize;
};

static volatile sig_atomic_t stop;

static void
sigalrm_handler(int signum __attribute__((unused)))
{
	stop = 1;
}

static uintmax_t
bench_read(const char *path, uintmax_t filesize)
{
	uintmax_t iters = 0;
	char *addr, *p;
	size_t chunk = 64;	/* cache-line sized */
	uintmax_t range;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(1, "open: %s", path);

	addr = mmap(NULL, filesize, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED)
		err(1, "mmap read: %s", path);

	range = filesize / chunk;

	while (!stop) {
		unsigned int r = (unsigned int)rand() % (unsigned int)range;
		p = addr + (uintmax_t)r * chunk;
		/* Touch the page — triggers read fault if cold */
		__asm__ volatile("" :: "r"(*p));
		iters++;
	}

	munmap(addr, filesize);
	close(fd);
	return iters;
}

static uintmax_t
bench_write(const char *path, uintmax_t filesize)
{
	uintmax_t iters = 0;
	char *addr, *p;
	size_t chunk = 4096;	/* page sized */
	uintmax_t range;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0)
		err(1, "open: %s", path);

	addr = mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED)
		err(1, "mmap write: %s", path);

	range = filesize / chunk;

	while (!stop) {
		unsigned int r = (unsigned int)rand() % (unsigned int)range;
		p = addr + (uintmax_t)r * chunk;
		p[0] = (char)(iters & 0xFF);	/* triggers write fault if first write */
		iters++;
	}

	munmap(addr, filesize);
	close(fd);
	return iters;
}

static void
worker(const struct bench_cfg *cfg, unsigned int id)
{
	char path[PATH_MAX];
	uintmax_t iters;

	snprintf(path, sizeof(path), "%s/fault-bench-w%d.dat",
		 cfg->directory, id);

	if (cfg->mode[0] == 'r' && cfg->mode[1] != 'w')
		iters = bench_read(path, cfg->filesize);
	else if (cfg->mode[0] == 'w')
		iters = bench_write(path, cfg->filesize);
	else
		iters = bench_read(path, cfg->filesize); /* rw: default read for now */

	printf("worker-%u\t%ju\n", id, iters);
}

static void
prepare_files(const struct bench_cfg *cfg)
{
	char path[PATH_MAX];
	unsigned int i;

	for (i = 0; i < cfg->workers; i++) {
		int fd;
		snprintf(path, sizeof(path), "%s/fault-bench-w%d.dat",
			 cfg->directory, i);
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (fd < 0)
			err(1, "create: %s", path);
		if (ftruncate(fd, (off_t)cfg->filesize) < 0)
			err(1, "ftruncate: %s", path);
		/*
		 * Pre-populate with data so read faults hit the page cache
		 * path (no disk I/O) and write faults go through the SSRFS
		 * range-lock + block-allocation path.
		 */
		{
			char *buf = malloc(4096);
			uintmax_t off;
			if (!buf)
				err(1, "malloc");
			memset(buf, 0xAA, 4096);
			for (off = 0; off < cfg->filesize; off += 4096)
				if (pwrite(fd, buf, 4096, (off_t)off) != 4096)
					err(1, "pwrite prep: %s", path);
			free(buf);
		}
		close(fd);
	}
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: mmap-fault-bench -w <workers> -s <sec> -f <bytes>"
		" -d <dir> [read|write|rw]\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct bench_cfg cfg = {0};
	struct sigaction sa = {0};
	pid_t *pids;
	int ch, i;
	unsigned int done;

	while ((ch = getopt(argc, argv, "w:s:f:d:")) != -1) {
		switch (ch) {
		case 'w':
			cfg.workers = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 's':
			cfg.duration = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'f':
			cfg.filesize = strtoumax(optarg, NULL, 10);
			break;
		case 'd':
			cfg.directory = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 0)
		cfg.mode = argv[0];
	else
		cfg.mode = "read";

	if (!cfg.workers || !cfg.duration || !cfg.filesize || !cfg.directory)
		usage();

	prepare_files(&cfg);

	sa.sa_handler = sigalrm_handler;
	sigaction(SIGALRM, &sa, NULL);
	alarm(cfg.duration);

	pids = calloc(cfg.workers, sizeof(pid_t));
	if (!pids)
		err(1, "calloc");

	for (i = 0; i < (int)cfg.workers; i++) {
		pid_t pid = fork();
		if (pid < 0)
			err(1, "fork");
		if (pid == 0) {
			free(pids);
			worker(&cfg, (unsigned int)i);
			_exit(0);
		}
		pids[i] = pid;
	}

	done = 0;
	while (done < cfg.workers) {
		int status;
		if (wait(&status) < 0)
			break;
		done++;
	}

	free(pids);
	return 0;
}
