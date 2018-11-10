// SPDX-License-Identifier: GPL-2.0

#include <linux/limits.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <pthread.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "../kselftest.h"
#include "cgroup_util.h"

#define gettid() syscall(SYS_gettid)

#define CGTESTMUTEX "/cgtestmutex"
#define CGTESTCONDR  "/cgtestwaitcond"
#define CGTESTCONDW  "/cgtestreadycond"

struct thread_arg {
	char *cgroup;
	int *waiting_threads;
	const bool *run;
	pthread_cond_t *cond_ready;
	pthread_cond_t *cond_waiting;
	pthread_mutex_t *mtx;
};

void *thread_function(void *vargp)
{
	intptr_t ret = -1;

	if (!vargp)
		return (void *)ret;

	struct thread_arg *arg = (struct thread_arg *)vargp;

	if (arg->cgroup) {
		char tidbuf[64];

		snprintf(tidbuf, sizeof(tidbuf), "%ld", gettid());
		if (cg_write(arg->cgroup, "cgroup.threads", tidbuf))
			goto ret_thread;
	}

	if (pthread_mutex_lock(arg->mtx))
		goto ret_thread;
	(*arg->waiting_threads)++;
	if (pthread_cond_signal(arg->cond_waiting))
		goto ret_thread;
	if (pthread_cond_wait(arg->cond_ready, arg->mtx))
		goto ret_thread;
	(*arg->waiting_threads)--;
	if (pthread_mutex_unlock(arg->mtx))
		goto ret_thread;

	ret = 0;
	do {
	} while (*arg->run);

ret_thread:
	return (void *)ret;
}

static int spawn_threads(struct thread_arg *t_arg, int cpu_weight)
{
		int ret = -1;
		int threads = get_nprocs() * 16;
		char weightbuff[64];

		pthread_t *thread_pool = (pthread_t *)malloc(threads * sizeof(pthread_t));

		if (!thread_pool)
			goto cleanup;

		if (cg_create(t_arg->cgroup))
			goto cleanup;

		if (cg_write(t_arg->cgroup, "cgroup.subtree_control", "+cpu"))
			goto cleanup;

		if (cg_enter_current(t_arg->cgroup))
			goto cleanup;

		snprintf(weightbuff, sizeof(weightbuff), "%d", cpu_weight);
		if (cg_write(t_arg->cgroup, "cpu.weight", weightbuff))
			goto cleanup;

		for (int i = 0; i < threads; i++) {
			if (pthread_create(&thread_pool[i], NULL, thread_function, (void *)t_arg))
				goto cleanup;
		}

		for (int i = 0; i < threads; i++) {
			intptr_t retval;

			if (pthread_join(thread_pool[i], (void *)&retval))
				goto cleanup;
			if (retval)
				goto cleanup;
		}

		ret = 0;

cleanup:
			free(thread_pool);
			return ret;
}

static int test_cgcpu_weight(const char *root)
{
	int ret = KSFT_FAIL;
	int condr_id, condw_id, mtx_id, run_shmid, waiting_threads_shmid;
	int *waiting_threads;
	int status;
	bool *run;
	long total_usage_usec, usage_usec_child_1, usage_usec_child_2;
	float usage_usec_child_1_percent, usage_usec_child_2_percent;
	char *cg_test_parent = NULL, *cg_test_child_1 = NULL, *cg_test_child_2 = NULL;
	pid_t child1_pid, child2_pid;
	struct thread_arg t_arg1, t_arg2;
	pthread_cond_t *cond_ready;
	pthread_cond_t *cond_waiting;
	pthread_mutex_t *mtx;
	pthread_mutexattr_t mattr;
	pthread_condattr_t cattr_w;
	pthread_condattr_t cattr_r;
	key_t key;

	if (cg_read_strstr(root, "cgroup.controllers", "cpu") ||
	    cg_read_strstr(root, "cgroup.subtree_control", "cpu")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	cg_test_parent = cg_name(root, "cg_test_parent");
	cg_test_child_1 = cg_name(root, "cg_test_parent/cg_test_child_1");
	cg_test_child_2 = cg_name(root, "cg_test_parent/cg_test_child_2");
	if (!cg_test_parent || !cg_test_child_1 || !cg_test_child_2)
		goto cleanup;

	if (cg_create(cg_test_parent))
		goto cleanup;

	if (cg_write(cg_test_parent, "cgroup.subtree_control", "+cpu"))
		goto cleanup;

	key = ftok("/tmp", 'r');
	run_shmid = shmget(key, sizeof(*run), IPC_CREAT | 0666);
	if (run_shmid < 0)
		goto cleanup;

	run = shmat(run_shmid, 0, 0);
	if (run == (void *)-1)
		goto cleanup;

	key = ftok("/tmp", 'w');
	waiting_threads_shmid = shmget(key, sizeof(*waiting_threads), IPC_CREAT | 0666);
	if (waiting_threads_shmid < 0)
		goto cleanup;

	waiting_threads = shmat(waiting_threads_shmid, 0, 0);
	if (waiting_threads == (void *)-1)
		goto cleanup;

	mtx_id = shm_open(CGTESTMUTEX, O_CREAT | O_RDWR | O_TRUNC, 0700);
	if (mtx_id < 0)
		goto cleanup;

	if (ftruncate(mtx_id, sizeof(pthread_mutex_t)) == -1)
		goto cleanup;

	mtx = (pthread_mutex_t *)mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED, mtx_id, 0);
	if (mtx == MAP_FAILED)
		goto cleanup;

	condw_id = shm_open(CGTESTCONDW, O_CREAT | O_RDWR | O_TRUNC, 0700);
	if (condw_id < 0)
		goto cleanup;

	if (ftruncate(condw_id, sizeof(pthread_cond_t)) == -1)
		goto cleanup;

	cond_waiting = (pthread_cond_t *)mmap(NULL, sizeof(pthread_cond_t), PROT_READ | PROT_WRITE, MAP_SHARED, condw_id, 0);
	if (cond_waiting == MAP_FAILED)
		goto cleanup;

	condr_id = shm_open(CGTESTCONDR, O_CREAT | O_RDWR | O_TRUNC, 0700);
	if (condr_id < 0)
		goto cleanup;

	if (ftruncate(condr_id, sizeof(pthread_cond_t)) == -1)
		goto cleanup;

	cond_ready = (pthread_cond_t *)mmap(NULL, sizeof(pthread_cond_t), PROT_READ | PROT_WRITE, MAP_SHARED, condr_id, 0);
	if (cond_ready == MAP_FAILED)
		goto cleanup;

	if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED))
		goto cleanup;
	if (pthread_mutex_init(mtx, &mattr))
		goto cleanup;
	if (pthread_mutexattr_destroy(&mattr))
		goto cleanup;

	if (pthread_condattr_setpshared(&cattr_w, PTHREAD_PROCESS_SHARED))
		goto cleanup;
	if (pthread_cond_init(cond_waiting, &cattr_w))
		goto cleanup;
	if (pthread_condattr_destroy(&cattr_w))
		goto cleanup;

	if (pthread_condattr_setpshared(&cattr_r, PTHREAD_PROCESS_SHARED))
		goto cleanup;
	if (pthread_cond_init(cond_ready, &cattr_r))
		goto cleanup;
	if (pthread_condattr_destroy(&cattr_r))
		goto cleanup;

	*waiting_threads = 0;
	*run = false;

	child1_pid = fork();
	if (child1_pid == 0) {
		t_arg1.cgroup = cg_test_child_1;
		t_arg1.waiting_threads = waiting_threads;
		t_arg1.run = run;
		t_arg1.cond_ready = cond_ready;
		t_arg1.cond_waiting = cond_waiting;
		t_arg1.mtx = mtx;
		exit(spawn_threads(&t_arg1, 8000));
	} else if (child1_pid < 0) {
		goto cleanup;
	}

	child2_pid = fork();
	if (child2_pid == 0) {
		t_arg2.cgroup = cg_test_child_2;
		t_arg2.waiting_threads = waiting_threads;
		t_arg2.run = run;
		t_arg2.cond_ready = cond_ready;
		t_arg2.cond_waiting = cond_waiting;
		t_arg2.mtx = mtx;
		exit(spawn_threads(&t_arg2, 2000));
	} else if (child2_pid < 0) {
		goto cleanup;
	}

	if (pthread_mutex_lock(mtx))
		goto cleanup;
	while (*waiting_threads < get_nprocs() * 32) {
		if (pthread_cond_wait(cond_waiting, mtx))
			goto cleanup;
	}
	*run = true;
	if (pthread_cond_broadcast(cond_ready))
		goto cleanup;
	if (pthread_mutex_unlock(mtx))
		goto cleanup;

	sleep(2);
	*run = false;

	wait(&status);
	if (status)
		goto cleanup;
	wait(&status);
	if (status)
		goto cleanup;

	total_usage_usec = cg_read_key_long(cg_test_parent, "cpu.stat", "usage_usec");

	usage_usec_child_1 = cg_read_key_long(cg_test_child_1, "cpu.stat", "usage_usec ");
	usage_usec_child_1_percent = 100 * ((float)usage_usec_child_1 / total_usage_usec);

	usage_usec_child_2 = cg_read_key_long(cg_test_child_2, "cpu.stat", "usage_usec ");
	usage_usec_child_2_percent = 100 * ((float)usage_usec_child_2 / total_usage_usec);

	printf("Child1 Weight: 8000\n");
	printf("usage_usec: %ld\n", usage_usec_child_1);
	printf("usage_usec%%: %f\n", usage_usec_child_1_percent);
	printf("=======================\n");

	printf("Child2 Weight: 2000\n");
	printf("usage_usec: %ld\n", usage_usec_child_2);
	printf("usage_usec%%: %f\n", usage_usec_child_2_percent);
	printf("=======================\n");

	if (usage_usec_child_1_percent <= 81 && usage_usec_child_2_percent <= 21)
		ret = KSFT_PASS;

cleanup:
	if (cg_test_child_2)
		cg_destroy(cg_test_child_2);
	if (cg_test_child_1)
		cg_destroy(cg_test_child_1);
	if (cg_test_parent)
		cg_destroy(cg_test_parent);
	free(cg_test_child_2);
	free(cg_test_child_1);
	free(cg_test_parent);
	shm_unlink(CGTESTCONDR);
	shm_unlink(CGTESTCONDW);
	shm_unlink(CGTESTMUTEX);
	return ret;
}

static int test_cgcpu_weight_threaded(const char *root)
{
	int ret = KSFT_FAIL;
	int threads;
	int waiting_threads = 0;
	bool run = false;
	pthread_cond_t cond_ready;
	pthread_cond_t cond_waiting;
	pthread_mutex_t mtx;
	long total_usage_usec, usage_usec_group_1, usage_usec_group_2;
	float usage_usec_group_1_percent, usage_usec_group_2_percent;
	char *cg_test_parent = NULL, *cg_test_group_1 = NULL, *cg_test_group_2 = NULL;

	if (cg_read_strstr(root, "cgroup.controllers", "cpu") ||
	    cg_read_strstr(root, "cgroup.subtree_control", "cpu")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	cg_test_parent = cg_name(root, "cg_test_parent");
	cg_test_group_1 = cg_name(root, "cg_test_parent/cg_test_group_1");
	cg_test_group_2 = cg_name(root, "cg_test_parent/cg_test_group_2");
	if (!cg_test_parent || !cg_test_group_1 || !cg_test_group_2)
		goto cleanup;

	if (cg_create(cg_test_parent))
		goto cleanup;

	if (cg_create(cg_test_group_1))
		goto cleanup;

	if (cg_create(cg_test_group_2))
		goto cleanup;

	if (cg_write(cg_test_parent, "cgroup.subtree_control", "+cpu"))
		goto cleanup;

	if (cg_write(cg_test_group_1, "cgroup.type", "threaded"))
		goto cleanup;

	if (cg_write(cg_test_group_1, "cgroup.subtree_control", "+cpu"))
		goto cleanup;

	if (cg_write(cg_test_group_1, "cpu.weight", "8000"))
		goto cleanup;

	if (cg_write(cg_test_group_2, "cgroup.type", "threaded"))
		goto cleanup;

	if (cg_write(cg_test_group_2, "cgroup.subtree_control", "+cpu"))
		goto cleanup;

	if (cg_write(cg_test_group_2, "cpu.weight", "2000"))
		goto cleanup;

	if (cg_enter_current(cg_test_group_1))
		goto cleanup;

	threads = get_nprocs() * 32;

	pthread_t *thread_pool = (pthread_t *)malloc(threads * sizeof(pthread_t));

	if (!thread_pool)
		goto cleanup;

	if (pthread_cond_init(&cond_ready, NULL))
		goto cleanup;

	if (pthread_cond_init(&cond_waiting, NULL))
		goto cleanup;

	if (pthread_mutex_init(&mtx, NULL))
		goto cleanup;

	struct thread_arg *args_pool = (struct thread_arg *)malloc(threads * sizeof(struct thread_arg));

	if (!args_pool)
		goto cleanup;

	for (int i = 0; i < threads; i++) {
		args_pool[i].cgroup = i % 2 == 0 ? cg_test_group_1 : cg_test_group_2;
		args_pool[i].waiting_threads = &waiting_threads;
		args_pool[i].run = &run;
		args_pool[i].cond_ready = &cond_ready;
		args_pool[i].cond_waiting = &cond_waiting;
		args_pool[i].mtx = &mtx;

		if (pthread_create(&thread_pool[i], NULL, thread_function, (void *)&args_pool[i]))
			goto cleanup;
	}

	if (pthread_mutex_lock(&mtx))
		goto cleanup;
	while (waiting_threads < threads) {
		if (pthread_cond_wait(&cond_waiting, &mtx))
			goto cleanup;
	}
	run = true;
	if (pthread_cond_broadcast(&cond_ready))
		goto cleanup;
	if (pthread_mutex_unlock(&mtx))
		goto cleanup;

	sleep(2);
	run = false;

	for (int i = 0; i < threads; i++) {
		intptr_t retval;

		if (pthread_join(thread_pool[i], (void *)&retval))
			goto cleanup;
		if (retval)
			goto cleanup;
	}

	total_usage_usec = cg_read_key_long(cg_test_parent, "cpu.stat", "usage_usec");

	usage_usec_group_1 = cg_read_key_long(cg_test_group_1, "cpu.stat", "usage_usec ");
	usage_usec_group_1_percent = 100 * ((float)usage_usec_group_1 / total_usage_usec);

	usage_usec_group_2 = cg_read_key_long(cg_test_group_2, "cpu.stat", "usage_usec ");
	usage_usec_group_2_percent = 100 * ((float)usage_usec_group_2 / total_usage_usec);

	printf("Group1 Weight: 8000\n");
	printf("usage_usec: %ld\n", usage_usec_group_1);
	printf("usage_usec%%: %f\n", usage_usec_group_1_percent);
	printf("=======================\n");

	printf("Group2 Weight: 2000\n");
	printf("usage_usec: %ld\n", usage_usec_group_2);
	printf("usage_usec%%: %f\n", usage_usec_group_2_percent);
	printf("=======================\n");

	if (usage_usec_group_1_percent <= 83 && usage_usec_group_2_percent <= 23)
		ret = KSFT_PASS;

cleanup:
	cg_enter_current(root);
	if (cg_test_group_2)
		cg_destroy(cg_test_group_2);
	if (cg_test_group_1)
		cg_destroy(cg_test_group_1);
	if (cg_test_parent)
		cg_destroy(cg_test_parent);
	pthread_cond_destroy(&cond_ready);
	pthread_cond_destroy(&cond_waiting);
	pthread_mutex_destroy(&mtx);
	free(cg_test_group_2);
	free(cg_test_group_1);
	free(cg_test_parent);
	free(thread_pool);
	free(args_pool);
	return ret;
}

#define T(x) { x, #x }
struct corecg_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_cgcpu_weight),
	T(test_cgcpu_weight_threaded),
};

#undef T

int main(int argc, char *argv[])
{
	char root[PATH_MAX];
	int i, ret = EXIT_SUCCESS;

	if (cg_find_unified_root(root, sizeof(root)))
		ksft_exit_skip("cgroup v2 isn't mounted\n");
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ret = EXIT_FAILURE;
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	return ret;
}
