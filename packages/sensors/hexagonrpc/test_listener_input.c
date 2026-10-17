/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../hexagonrpcd/listener.c"

static char payload[2048];
static char *encoded;
static size_t encoded_size;
static int fetch_error;
static int invalid_length;
static int incomplete;
static unsigned int fetches;

int fastrpc2(const struct fastrpc_function_def_interp2 *def,
            int fd, uint32_t handle, ...)
{
	va_list args;
	uint32_t *required;
	uint32_t size;
	void *buffer;

	assert(fd == 17 && handle == ADSP_LISTENER_HANDLE);
	va_start(args, handle);
	if (def->msg_id == 4) {
		assert(va_arg(args, uint32_t) == 0);
		assert(va_arg(args, uint32_t) == 0);
		assert(va_arg(args, uint32_t) == 0);
		(void)va_arg(args, void *);
		*va_arg(args, uint32_t *) = 123;
		*va_arg(args, uint32_t *) = 1;
		*va_arg(args, uint32_t *) = REMOTE_SCALARS_MAKE(5, 2, 1);
		required = va_arg(args, uint32_t *);
		size = va_arg(args, uint32_t);
		buffer = va_arg(args, void *);
		*required = incomplete ? 32 : encoded_size;
		memcpy(buffer, encoded, size < *required ? size : *required);
	} else {
		assert(def->msg_id == 5);
		assert(def->in_nums == 2 && def->out_nums == 1 && def->out_bufs == 1);
		assert(va_arg(args, uint32_t) == 123);
		assert(va_arg(args, uint32_t) == 256);
		required = va_arg(args, uint32_t *);
		size = va_arg(args, uint32_t);
		buffer = va_arg(args, void *);
		assert(size == encoded_size - 256);
		*required = encoded_size + invalid_length;
		memcpy(buffer, encoded + 256, size);
		fetches++;
	}
	va_end(args);
	return def->msg_id == 5 ? fetch_error : 0;
}

static int receive(struct fastrpc_io_buffer **decoded)
{
	uint32_t rctx = 0, handle = 0, sc = 0;
	return return_for_next_invoke(17, 0, &rctx, &handle, &sc, NULL, decoded);
}

static uint32_t rename_invoke(void *data, const struct fastrpc_io_buffer *in,
                              struct fastrpc_io_buffer *out)
{
	assert(!strcmp(in[1].p, "old") && !strcmp(in[2].p, "new"));
	assert(out == NULL);
	return 0;
}

static void test_extended_method(void)
{
	const struct fastrpc_function_def_interp2 def = {33, 1, 2, 0, 0};
	struct fastrpc_function_impl methods[34] = {0};
	struct fastrpc_interface iface = {.n_procs = 34, .procs = methods};
	struct fastrpc_interface *ifaces[] = {&iface};
	uint32_t numbers[] = {33, 4, 4}, result;
	struct fastrpc_io_buffer inputs[] = {
		{sizeof(numbers), numbers}, {4, "old"}, {4, "new"},
	};
	struct fastrpc_io_buffer *returned = NULL;

	methods[33] = (struct fastrpc_function_impl){.def = &def, .impl = rename_invoke};
	assert(invoke_requested_procedure(1, ifaces, 0, 0x1f030000,
	                                 &result, inputs, &returned) == 0);
	assert(result == 0 && returned == NULL);
	numbers[0] = 34;
	assert(invoke_requested_procedure(1, ifaces, 0, 0x1f030000,
	                                 &result, inputs, &returned) != 0);
	assert(result == AEE_EUNSUPPORTED && returned == NULL);
}

int main(void)
{
	uint32_t numbers[] = {7, sizeof(payload)};
	struct fastrpc_io_buffer inputs[] = {
		{sizeof(numbers), numbers}, {sizeof(payload), payload},
	};
	struct fastrpc_io_buffer *decoded = NULL;

	memset(payload, 'x', sizeof(payload));
	encoded_size = outbufs_calculate_size(2, inputs);
	encoded = malloc(encoded_size);
	assert(encoded);
	outbufs_encode(2, inputs, encoded);
	assert(receive(&decoded) == 0);
	assert(fetches == 1);
	assert(decoded[0].s == sizeof(numbers));
	assert(!memcmp(decoded[0].p, numbers, sizeof(numbers)));
	assert(decoded[1].s == sizeof(payload));
	assert(!memcmp(decoded[1].p, payload, sizeof(payload)));
	iobuf_free(2, decoded);
	decoded = NULL;
	fetch_error = -EIO;
	assert(receive(&decoded) == -EIO && decoded == NULL);
	fetch_error = 0;
	invalid_length = 1;
	assert(receive(&decoded) == -1 && decoded == NULL);
	invalid_length = 0;
	incomplete = 1;
	assert(receive(&decoded) == -1 && decoded == NULL);
	free(encoded);
	test_extended_method();
	puts("Listener input retrieval and failure cleanup tests passed");
	return 0;
}
