/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hexagonfs.h"

int main(void)
{
	char path[] = "./hexagonfs-test.XXXXXX";
	char value[256], created[256], link[256];
	char buf[4] = { 0 };
	struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD] = { 0 };
	struct hexagonfs_dirent ro = { "ro", &hexagonfs_mapped_ops, { .phys = path } };
	struct hexagonfs_dirent rw = { "rw", &hexagonfs_writable_ops, { .phys = path } };
	struct hexagonfs_dirent *entries[] = { &ro, &rw, NULL };
	struct hexagonfs_dirent tree = { "/", &hexagonfs_virt_dir_ops, { .dir = entries } };
	int root, dir, child, dup, fd;

	assert(mkdtemp(path));
	snprintf(value, sizeof(value), "%s/value", path);
	snprintf(created, sizeof(created), "%s/created", path);
	snprintf(link, sizeof(link), "%s/link", path);
	fd = open(value, O_WRONLY | O_CREAT, 0600);
	assert(fd >= 0 && write(fd, "abc", 3) == 3);
	close(fd);
	assert(symlink("/etc/passwd", link) == 0);

	root = hexagonfs_open_root(fds, &tree);
	assert(root == 0);
	dup = hexagonfs_openat(fds, root, root, "/");
	assert(dup > root && hexagonfs_close(fds, dup) == 0);
	assert(hexagonfs_openat(fds, root, -1, "value") == -EBADF);
	assert(hexagonfs_openat(fds, root, root, "/rw/missing/value") == -ENOENT);
	assert(hexagonfs_openat(fds, root, root, "/rw/link") == -ELOOP);
	assert(hexagonfs_openat_flags(fds, root, root, "/ro/value", O_WRONLY) == -EROFS);
	assert(hexagonfs_openat_flags(fds, root, root, "/rw/../ro/value", O_WRONLY) == -EROFS);

	fd = hexagonfs_openat_flags(fds, root, root, "/rw/created", O_RDWR | O_CREAT | O_TRUNC);
	assert(fd >= 0);
	assert(hexagonfs_write(fds, fd, 3, "new") == 3);
	assert(hexagonfs_flush(fds, fd) == 0);
	assert(hexagonfs_lseek(fds, fd, 0, SEEK_SET) == 0);
	assert(hexagonfs_read(fds, fd, 3, buf) == 3 && !strcmp(buf, "new"));
	assert(hexagonfs_close(fds, fd) == 0);

	for (int i = 0; i < HEXAGONFS_MAX_FD - 1; i++)
		assert(hexagonfs_openat(fds, root, root, "/rw/value") == i + 1);
	assert(hexagonfs_openat(fds, root, root, "/rw/value") == -EMFILE);
	for (int i = 1; i < HEXAGONFS_MAX_FD; i++)
		assert(hexagonfs_close(fds, i) == 0);

	dir = hexagonfs_openat(fds, root, root, "/rw/");
	child = hexagonfs_openat(fds, root, dir, "./value");
	assert(dir >= 0 && child >= 0);
	assert(hexagonfs_close(fds, dir) == 0);
	assert(hexagonfs_close(fds, root) == 0);
	memset(buf, 0, sizeof(buf));
	assert(hexagonfs_read(fds, child, 3, buf) == 3 && !strcmp(buf, "abc"));
	assert(hexagonfs_close(fds, child) == 0);
	assert(hexagonfs_close(fds, child) == -EBADF);
	for (int i = 0; i < HEXAGONFS_MAX_FD; i++)
		assert(!fds[i]);

	assert(unlink(value) == 0 && unlink(created) == 0 && unlink(link) == 0);
	assert(rmdir(path) == 0);
	puts("HexagonFS lifecycle and registry tests passed");
	return 0;
}
