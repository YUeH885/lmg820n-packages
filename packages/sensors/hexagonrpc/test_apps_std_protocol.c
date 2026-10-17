/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "apps_std.h"

static uint32_t invoke(struct fastrpc_interface *iface, uint32_t method,
                       struct fastrpc_io_buffer *in, struct fastrpc_io_buffer *out)
{
	assert(method < iface->n_procs && iface->procs[method].def);
	assert(iface->procs[method].def->msg_id == method);
	return iface->procs[method].impl(iface->data, in, out);
}

int main(void)
{
	char path[] = "./apps-std-test.XXXXXX", file[256], buf[4] = {0};
	char name[] = "/rw/rpc", env[] = "ADSP_LIBRARY_PATH", mode[] = "wb";
	struct hexagonfs_dirent adsp = {"adsp", &hexagonfs_mapped_ops, {.phys = path}};
	struct hexagonfs_dirent *qcom_entries[] = {&adsp, NULL};
	struct hexagonfs_dirent qcom = {"qcom", &hexagonfs_virt_dir_ops, {.dir = qcom_entries}};
	struct hexagonfs_dirent *lib_entries[] = {&qcom, NULL};
	struct hexagonfs_dirent lib = {"lib", &hexagonfs_virt_dir_ops, {.dir = lib_entries}};
	struct hexagonfs_dirent *usr_entries[] = {&lib, NULL};
	struct hexagonfs_dirent usr = {"usr", &hexagonfs_virt_dir_ops, {.dir = usr_entries}};
	struct hexagonfs_dirent rw = {"rw", &hexagonfs_writable_ops, {.phys = path}};
	struct hexagonfs_dirent *entries[] = {&usr, &rw, NULL};
	struct hexagonfs_dirent root = {"/", &hexagonfs_virt_dir_ops, {.dir = entries}};
	struct fastrpc_interface *iface;
	struct fastrpc_io_buffer in[5] = {0}, out[2] = {0};
	uint32_t fd, result[2], read_request[2];
	uint64_t dir;

	assert(mkdtemp(path));
	iface = fastrpc_apps_std_init(&root);
	assert(iface);
	in[1] = (struct fastrpc_io_buffer){.p = env, .s = sizeof(env)};
	in[3] = (struct fastrpc_io_buffer){.p = name, .s = sizeof(name)};
	in[4] = (struct fastrpc_io_buffer){.p = mode, .s = sizeof(mode)};
	out[0] = (struct fastrpc_io_buffer){.p = &fd, .s = sizeof(fd)};
	assert(invoke(iface, 19, in, out) == 0);
	assert(fd >= APPS_STD_FILE_BASE && fd < APPS_STD_FILE_BASE + HEXAGONFS_MAX_FD);
	in[0] = (struct fastrpc_io_buffer){.p = &fd, .s = sizeof(fd)};
	in[1] = (struct fastrpc_io_buffer){.p = "rpc", .s = 3};
	out[0] = (struct fastrpc_io_buffer){.p = result, .s = sizeof(result)};
	assert(invoke(iface, 5, in, out) == 0 && result[0] == 3);
	assert(invoke(iface, 2, in, out) == 0);
	assert(invoke(iface, 3, in, out) == 0);

	memcpy(mode, "rb", 3);
	in[1] = (struct fastrpc_io_buffer){.p = env, .s = sizeof(env)};
	out[0] = (struct fastrpc_io_buffer){.p = &fd, .s = sizeof(fd)};
	assert(invoke(iface, 19, in, out) == 0);
	read_request[0] = fd;
	read_request[1] = 3;
	in[0] = (struct fastrpc_io_buffer){.p = read_request, .s = sizeof(read_request)};
	out[0] = (struct fastrpc_io_buffer){.p = result, .s = sizeof(result)};
	out[1] = (struct fastrpc_io_buffer){.p = buf, .s = 3};
	assert(invoke(iface, 4, in, out) == 0 && result[0] == 3 && !strcmp(buf, "rpc"));
	in[0] = (struct fastrpc_io_buffer){.p = &fd, .s = sizeof(fd)};
	assert(invoke(iface, 3, in, out) == 0);
	/* Firmware stdio handles must not address the virtual root or search paths. */
	for (uint32_t reserved = 0; reserved < 3; reserved++) {
		in[0] = (struct fastrpc_io_buffer){.p = &reserved, .s = sizeof(reserved)};
		assert(invoke(iface, 3, in, out) != 0);
		assert(invoke(iface, 2, in, out) != 0);
	}
	in[1] = (struct fastrpc_io_buffer){.p = "/rw/rpc", .s = 8};
	in[2] = (struct fastrpc_io_buffer){.p = "/rw/renamed", .s = 12};
	assert(invoke(iface, 33, in, out) == 0);
	in[1] = in[2];
	in[2] = (struct fastrpc_io_buffer){.p = "/usr/lib/qcom/adsp/rpc", .s = 23};
	assert(invoke(iface, 33, in, out) != 0);
	in[2] = (struct fastrpc_io_buffer){.p = "/rw/..", .s = 7};
	assert(invoke(iface, 33, in, out) != 0);

	/* opendir has only two input buffers; this matches 0x1a020100. */
	struct fastrpc_io_buffer dir_in[2] = {{0}, {.p = "/rw/", .s = 5}};
	out[0] = (struct fastrpc_io_buffer){.p = &dir, .s = sizeof(dir)};
	assert(invoke(iface, 26, dir_in, out) == 0);
	in[0] = (struct fastrpc_io_buffer){.p = &dir, .s = sizeof(dir)};
	assert(invoke(iface, 27, in, out) == 0);
	fastrpc_apps_std_deinit(iface);
	snprintf(file, sizeof(file), "%s/renamed", path);
	assert(unlink(file) == 0 && rmdir(path) == 0);
	puts("apps_std RPC open, write, flush, read and directory tests passed");
	return 0;
}
