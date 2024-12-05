// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Various unit tests for the "ntsync" synchronization primitive driver.
 *
 * Copyright (C) 2021-2022 Elizabeth Figura <zfigura@codeweavers.com>
 */

#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <linux/ntsync.h>
#include "../../kselftest_harness.h"

static int read_sem_state(int sem, __u32 *count, __u32 *max)
{
	struct ntsync_sem_args args;
	int ret;

	memset(&args, 0xcc, sizeof(args));
	ret = ioctl(sem, NTSYNC_IOC_SEM_READ, &args);
	*count = args.count;
	*max = args.max;
	return ret;
}

#define check_sem_state(sem, count, max) \
	({ \
		__u32 __count, __max; \
		int ret = read_sem_state((sem), &__count, &__max); \
		EXPECT_EQ(0, ret); \
		EXPECT_EQ((count), __count); \
		EXPECT_EQ((max), __max); \
	})

static int post_sem(int sem, __u32 *count)
{
	return ioctl(sem, NTSYNC_IOC_SEM_POST, count);
}

static int read_mutex_state(int mutex, __u32 *count, __u32 *owner)
{
	struct ntsync_mutex_args args;
	int ret;

	memset(&args, 0xcc, sizeof(args));
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_READ, &args);
	*count = args.count;
	*owner = args.owner;
	return ret;
}

#define check_mutex_state(mutex, count, owner) \
	({ \
		__u32 __count, __owner; \
		int ret = read_mutex_state((mutex), &__count, &__owner); \
		EXPECT_EQ(0, ret); \
		EXPECT_EQ((count), __count); \
		EXPECT_EQ((owner), __owner); \
	})

static int unlock_mutex(int mutex, __u32 owner, __u32 *count)
{
	struct ntsync_mutex_args args;
	int ret;

	args.owner = owner;
	args.count = 0xdeadbeef;
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_UNLOCK, &args);
	*count = args.count;
	return ret;
}

static int wait_any(int fd, __u32 count, const int *objs, __u32 owner, __u32 *index)
{
	struct ntsync_wait_args args = {0};
	struct timespec timeout;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	args.timeout = timeout.tv_sec * 1000000000 + timeout.tv_nsec;
	args.count = count;
	args.objs = (uintptr_t)objs;
	args.owner = owner;
	args.index = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_WAIT_ANY, &args);
	*index = args.index;
	return ret;
}

TEST(semaphore_state)
{
	struct ntsync_sem_args sem_args;
	struct timespec timeout;
	__u32 count, index;
	int fd, ret, sem;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	fd = open("/dev/ntsync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	sem_args.count = 3;
	sem_args.max = 2;
	sem_args.sem = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	sem_args.count = 2;
	sem_args.max = 2;
	sem_args.sem = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, sem_args.sem);
	sem = sem_args.sem;
	check_sem_state(sem, 2, 2);

	count = 0;
	ret = post_sem(sem, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, count);
	check_sem_state(sem, 2, 2);

	count = 1;
	ret = post_sem(sem, &count);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOVERFLOW, errno);
	check_sem_state(sem, 2, 2);

	ret = wait_any(fd, 1, &sem, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_sem_state(sem, 1, 2);

	ret = wait_any(fd, 1, &sem, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_sem_state(sem, 0, 2);

	ret = wait_any(fd, 1, &sem, 123, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	count = 3;
	ret = post_sem(sem, &count);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOVERFLOW, errno);
	check_sem_state(sem, 0, 2);

	count = 2;
	ret = post_sem(sem, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, count);
	check_sem_state(sem, 2, 2);

	ret = wait_any(fd, 1, &sem, 123, &index);
	EXPECT_EQ(0, ret);
	ret = wait_any(fd, 1, &sem, 123, &index);
	EXPECT_EQ(0, ret);

	count = 1;
	ret = post_sem(sem, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, count);
	check_sem_state(sem, 1, 2);

	count = ~0u;
	ret = post_sem(sem, &count);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOVERFLOW, errno);
	check_sem_state(sem, 1, 2);

	close(sem);

	close(fd);
}

TEST(mutex_state)
{
	struct ntsync_mutex_args mutex_args;
	__u32 owner, count, index;
	struct timespec timeout;
	int fd, ret, mutex;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	fd = open("/dev/ntsync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	mutex_args.owner = 123;
	mutex_args.count = 0;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	mutex_args.owner = 0;
	mutex_args.count = 2;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	mutex_args.owner = 123;
	mutex_args.count = 2;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);
	mutex = mutex_args.mutex;
	check_mutex_state(mutex, 2, 123);

	ret = unlock_mutex(mutex, 0, &count);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = unlock_mutex(mutex, 456, &count);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
	check_mutex_state(mutex, 2, 123);

	ret = unlock_mutex(mutex, 123, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, count);
	check_mutex_state(mutex, 1, 123);

	ret = unlock_mutex(mutex, 123, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, count);
	check_mutex_state(mutex, 0, 0);

	ret = unlock_mutex(mutex, 123, &count);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);

	ret = wait_any(fd, 1, &mutex, 456, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_mutex_state(mutex, 1, 456);

	ret = wait_any(fd, 1, &mutex, 456, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_mutex_state(mutex, 2, 456);

	ret = unlock_mutex(mutex, 456, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(2, count);
	check_mutex_state(mutex, 1, 456);

	ret = wait_any(fd, 1, &mutex, 123, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	owner = 0;
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_KILL, &owner);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	owner = 123;
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_KILL, &owner);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EPERM, errno);
	check_mutex_state(mutex, 1, 456);

	owner = 456;
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_KILL, &owner);
	EXPECT_EQ(0, ret);

	memset(&mutex_args, 0xcc, sizeof(mutex_args));
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_READ, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	memset(&mutex_args, 0xcc, sizeof(mutex_args));
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_READ, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	ret = wait_any(fd, 1, &mutex, 123, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, index);
	check_mutex_state(mutex, 1, 123);

	owner = 123;
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_KILL, &owner);
	EXPECT_EQ(0, ret);

	memset(&mutex_args, 0xcc, sizeof(mutex_args));
	ret = ioctl(mutex, NTSYNC_IOC_MUTEX_READ, &mutex_args);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, mutex_args.count);
	EXPECT_EQ(0, mutex_args.owner);

	ret = wait_any(fd, 1, &mutex, 123, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(0, index);
	check_mutex_state(mutex, 1, 123);

	close(mutex);

	mutex_args.owner = 0;
	mutex_args.count = 0;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);
	mutex = mutex_args.mutex;
	check_mutex_state(mutex, 0, 0);

	ret = wait_any(fd, 1, &mutex, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_mutex_state(mutex, 1, 123);

	close(mutex);

	mutex_args.owner = 123;
	mutex_args.count = ~0u;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);
	mutex = mutex_args.mutex;
	check_mutex_state(mutex, ~0u, 123);

	ret = wait_any(fd, 1, &mutex, 123, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	close(mutex);

	close(fd);
}

TEST(test_wait_any)
{
	int objs[NTSYNC_MAX_WAIT_COUNT + 1], fd, ret;
	struct ntsync_mutex_args mutex_args = {0};
	struct ntsync_sem_args sem_args = {0};
	__u32 owner, index, count, i;
	struct timespec timeout;

	clock_gettime(CLOCK_MONOTONIC, &timeout);

	fd = open("/dev/ntsync", O_CLOEXEC | O_RDONLY);
	ASSERT_LE(0, fd);

	sem_args.count = 2;
	sem_args.max = 3;
	sem_args.sem = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_SEM, &sem_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, sem_args.sem);

	mutex_args.owner = 0;
	mutex_args.count = 0;
	mutex_args.mutex = 0xdeadbeef;
	ret = ioctl(fd, NTSYNC_IOC_CREATE_MUTEX, &mutex_args);
	EXPECT_EQ(0, ret);
	EXPECT_NE(0xdeadbeef, mutex_args.mutex);

	objs[0] = sem_args.sem;
	objs[1] = mutex_args.mutex;

	ret = wait_any(fd, 2, objs, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_sem_state(sem_args.sem, 1, 3);
	check_mutex_state(mutex_args.mutex, 0, 0);

	ret = wait_any(fd, 2, objs, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_sem_state(sem_args.sem, 0, 3);
	check_mutex_state(mutex_args.mutex, 0, 0);

	ret = wait_any(fd, 2, objs, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, index);
	check_sem_state(sem_args.sem, 0, 3);
	check_mutex_state(mutex_args.mutex, 1, 123);

	count = 1;
	ret = post_sem(sem_args.sem, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, count);

	ret = wait_any(fd, 2, objs, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_sem_state(sem_args.sem, 0, 3);
	check_mutex_state(mutex_args.mutex, 1, 123);

	ret = wait_any(fd, 2, objs, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, index);
	check_sem_state(sem_args.sem, 0, 3);
	check_mutex_state(mutex_args.mutex, 2, 123);

	ret = wait_any(fd, 2, objs, 456, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	owner = 123;
	ret = ioctl(mutex_args.mutex, NTSYNC_IOC_MUTEX_KILL, &owner);
	EXPECT_EQ(0, ret);

	ret = wait_any(fd, 2, objs, 456, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EOWNERDEAD, errno);
	EXPECT_EQ(1, index);

	ret = wait_any(fd, 2, objs, 456, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, index);

	/* test waiting on the same object twice */
	count = 2;
	ret = post_sem(sem_args.sem, &count);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, count);

	objs[0] = objs[1] = sem_args.sem;
	ret = wait_any(fd, 2, objs, 456, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);
	check_sem_state(sem_args.sem, 1, 3);

	ret = wait_any(fd, 0, NULL, 456, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ETIMEDOUT, errno);

	for (i = 0; i < NTSYNC_MAX_WAIT_COUNT + 1; ++i)
		objs[i] = sem_args.sem;

	ret = wait_any(fd, NTSYNC_MAX_WAIT_COUNT, objs, 123, &index);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(0, index);

	ret = wait_any(fd, NTSYNC_MAX_WAIT_COUNT + 1, objs, 123, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = wait_any(fd, -1, objs, 123, &index);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	close(sem_args.sem);
	close(mutex_args.mutex);

	close(fd);
}

TEST_HARNESS_MAIN
