/*
 * test-ctf-writer.c
 *
 * CTF Writer test
 *
 * Copyright 2013 - 2015 - Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <babeltrace/ctf-writer/writer.h>
#include <babeltrace/ctf-writer/clock.h>
#include <babeltrace/ctf-writer/stream.h>
#include <babeltrace/ctf-writer/event.h>
#include <babeltrace/ctf-writer/event-types.h>
#include <babeltrace/ctf-writer/event-fields.h>
#include <babeltrace/ctf-ir/stream-class.h>
#include <babeltrace/ref.h>
#include <babeltrace/ctf/events.h>
#include <babeltrace/values.h>
#include <unistd.h>
#include <babeltrace/compat/stdlib.h>
#include <stdio.h>
#include <sys/utsname.h>
#include <babeltrace/compat/limits.h>
#include <babeltrace/compat/stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <babeltrace/compat/dirent.h>
#include "tap/tap.h"
#include <math.h>
#include <float.h>
#include <sys/stat.h>

#define METADATA_LINE_SIZE 512
#define SEQUENCE_TEST_LENGTH 10
#define ARRAY_TEST_LENGTH 5
#define PACKET_RESIZE_TEST_LENGTH 100000

#define DEFAULT_CLOCK_FREQ 1000000000
#define DEFAULT_CLOCK_PRECISION 1
#define DEFAULT_CLOCK_OFFSET 0
#define DEFAULT_CLOCK_OFFSET_S 0
#define DEFAULT_CLOCK_IS_ABSOLUTE 0
#define DEFAULT_CLOCK_TIME 0

static uint64_t current_time = 42;

/* Return 1 if uuids match, zero if different. */
int uuid_match(const unsigned char *uuid_a, const unsigned char *uuid_b)
{
	int ret = 0;
	int i;

	if (!uuid_a || !uuid_b) {
		goto end;
	}

	for (i = 0; i < 16; i++) {
		if (uuid_a[i] != uuid_b[i]) {
			goto end;
		}
	}

	ret = 1;
end:
	return ret;
}

void validate_metadata(char *parser_path, char *metadata_path)
{
	int ret = 0;
	char parser_output_path[] = "/tmp/parser_output_XXXXXX";
	int parser_output_fd = -1, metadata_fd = -1;

	if (!metadata_path) {
		ret = -1;
		goto result;
	}

	parser_output_fd = mkstemp(parser_output_path);
	metadata_fd = open(metadata_path, O_RDONLY);

	unlink(parser_output_path);

	if (parser_output_fd == -1 || metadata_fd == -1) {
		diag("Failed create temporary files for metadata parsing.");
		ret = -1;
		goto result;
	}

	pid_t pid = fork();
	if (pid) {
		int status = 0;
		waitpid(pid, &status, 0);
		ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	} else {
		/* ctf-parser-test expects a metadata string on stdin. */
		ret = dup2(metadata_fd, STDIN_FILENO);
		if (ret < 0) {
			perror("# dup2 metadata_fd to STDIN");
			goto result;
		}

		ret = dup2(parser_output_fd, STDOUT_FILENO);
		if (ret < 0) {
			perror("# dup2 parser_output_fd to STDOUT");
			goto result;
		}

		ret = dup2(parser_output_fd, STDERR_FILENO);
		if (ret < 0) {
			perror("# dup2 parser_output_fd to STDERR");
			goto result;
		}

		execl(parser_path, "ctf-parser-test", NULL);
		perror("# Could not launch the ctf metadata parser process");
		exit(-1);
	}
result:
	ok(ret == 0, "Metadata string is valid");

	if (ret && metadata_fd >= 0 && parser_output_fd >= 0) {
		char *line;
		size_t len = METADATA_LINE_SIZE;
		FILE *metadata_fp = NULL, *parser_output_fp = NULL;

		metadata_fp = fdopen(metadata_fd, "r");
		if (!metadata_fp) {
			perror("fdopen on metadata_fd");
			goto close_fp;
		}
		metadata_fd = -1;

		parser_output_fp = fdopen(parser_output_fd, "r");
		if (!parser_output_fp) {
			perror("fdopen on parser_output_fd");
			goto close_fp;
		}
		parser_output_fd = -1;

		line = malloc(len);
		if (!line) {
			diag("malloc failure");
		}

		rewind(metadata_fp);

		/* Output the metadata and parser output as diagnostic */
		while (bt_getline(&line, &len, metadata_fp) > 0) {
			fprintf(stderr, "# %s", line);
		}

		rewind(parser_output_fp);
		while (bt_getline(&line, &len, parser_output_fp) > 0) {
			fprintf(stderr, "# %s", line);
		}

		free(line);
close_fp:
		if (metadata_fp) {
			if (fclose(metadata_fp)) {
				diag("fclose failure");
			}
		}
		if (parser_output_fp) {
			if (fclose(parser_output_fp)) {
				diag("fclose failure");
			}
		}
	}

	if (parser_output_fd >= 0) {
		if (close(parser_output_fd)) {
			diag("close error");
		}
	}
	if (metadata_fd >= 0) {
		if (close(metadata_fd)) {
			diag("close error");
		}
	}
}

void validate_trace(char *parser_path, char *trace_path)
{
	int ret = 0;
	char babeltrace_output_path[] = "/tmp/babeltrace_output_XXXXXX";
	int babeltrace_output_fd = -1;

	if (!trace_path) {
		ret = -1;
		goto result;
	}

	babeltrace_output_fd = mkstemp(babeltrace_output_path);
	unlink(babeltrace_output_path);

	if (babeltrace_output_fd == -1) {
		diag("Failed to create a temporary file for trace parsing.");
		ret = -1;
		goto result;
	}

	pid_t pid = fork();
	if (pid) {
		int status = 0;
		waitpid(pid, &status, 0);
		ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	} else {
		ret = dup2(babeltrace_output_fd, STDOUT_FILENO);
		if (ret < 0) {
			perror("# dup2 babeltrace_output_fd to STDOUT");
			goto result;
		}

		ret = dup2(babeltrace_output_fd, STDERR_FILENO);
		if (ret < 0) {
			perror("# dup2 babeltrace_output_fd to STDERR");
			goto result;
		}

		execl(parser_path, "babeltrace", trace_path, NULL);
		perror("# Could not launch the babeltrace process");
		exit(-1);
	}
result:
	ok(ret == 0, "Babeltrace could read the resulting trace");

	if (ret && babeltrace_output_fd >= 0) {
		char *line;
		size_t len = METADATA_LINE_SIZE;
		FILE *babeltrace_output_fp = NULL;

		babeltrace_output_fp = fdopen(babeltrace_output_fd, "r");
		if (!babeltrace_output_fp) {
			perror("fdopen on babeltrace_output_fd");
			goto close_fp;
		}
		babeltrace_output_fd = -1;

		line = malloc(len);
		if (!line) {
			diag("malloc error");
		}
		rewind(babeltrace_output_fp);
		while (bt_getline(&line, &len, babeltrace_output_fp) > 0) {
			diag("%s", line);
		}

		free(line);
close_fp:
		if (babeltrace_output_fp) {
			if (fclose(babeltrace_output_fp)) {
				diag("fclose error");
			}
		}
	}

	if (babeltrace_output_fd >= 0) {
		if (close(babeltrace_output_fd)) {
			diag("close error");
		}
	}
}

void event_copy_tests(struct bt_ctf_event *event)
{
	struct bt_ctf_event *copy;
	struct bt_ctf_event_class *orig_event_class;
	struct bt_ctf_event_class *copy_event_class;
	struct bt_ctf_stream *orig_stream;
	struct bt_ctf_stream *copy_stream;
	struct bt_ctf_field *orig_field;
	struct bt_ctf_field *copy_field;

	/* copy */
	ok(!bt_ctf_event_copy(NULL),
		"bt_ctf_event_copy handles NULL correctly");
	copy = bt_ctf_event_copy(event);
	ok(copy, "bt_ctf_event_copy returns a valid pointer");

	/* validate event class */
	orig_event_class = bt_ctf_event_get_class(event);
	assert(orig_event_class);
	copy_event_class = bt_ctf_event_get_class(copy);
	ok(orig_event_class == copy_event_class,
		"original and copied events share the same event class pointer");
	bt_put(orig_event_class);
	bt_put(copy_event_class);

	/* validate stream */
	orig_stream = bt_ctf_event_get_stream(event);
	copy_stream = bt_ctf_event_get_stream(copy);

	if (!orig_stream) {
		ok(!copy_stream, "original and copied events have no stream");
	} else {
		ok(orig_stream == copy_stream,
			"original and copied events share the same stream pointer");
	}
	bt_put(orig_stream);
	bt_put(copy_stream);

	/* header */
	orig_field = bt_ctf_event_get_header(event);
	copy_field = bt_ctf_event_get_header(copy);

	if (!orig_field) {
		ok(!copy_field, "original and copied events have no header");
	} else {
		ok(orig_field != copy_field,
			"original and copied events headers are different pointers");
	}

	bt_put(orig_field);
	bt_put(copy_field);

	/* context */
	orig_field = bt_ctf_event_get_event_context(event);
	copy_field = bt_ctf_event_get_event_context(copy);

	if (!orig_field) {
		ok(!copy_field, "original and copied events have no context");
	} else {
		ok(orig_field != copy_field,
			"original and copied events contexts are different pointers");
	}

	bt_put(orig_field);
	bt_put(copy_field);

	/* payload */
	orig_field = bt_ctf_event_get_payload_field(event);
	copy_field = bt_ctf_event_get_payload_field(copy);

	if (!orig_field) {
		ok(!copy_field, "original and copied events have no payload");
	} else {
		ok(orig_field != copy_field,
			"original and copied events payloads are different pointers");
	}

	bt_put(orig_field);
	bt_put(copy_field);

	bt_put(copy);
}

void append_simple_event(struct bt_ctf_stream_class *stream_class,
		struct bt_ctf_stream *stream, struct bt_ctf_clock *clock)
{
	/* Create and add a simple event class */
	struct bt_ctf_event_class *simple_event_class =
		bt_ctf_event_class_create("Simple Event");
	struct bt_ctf_field_type *uint_12_type =
		bt_ctf_field_type_integer_create(12);
	struct bt_ctf_field_type *int_64_type =
		bt_ctf_field_type_integer_create(64);
	struct bt_ctf_field_type *float_type =
		bt_ctf_field_type_floating_point_create();
	struct bt_ctf_field_type *enum_type;
	struct bt_ctf_field_type *enum_type_unsigned =
		bt_ctf_field_type_enumeration_create(uint_12_type);
	struct bt_ctf_field_type *event_context_type =
		bt_ctf_field_type_structure_create();
	struct bt_ctf_field_type *returned_type;
	struct bt_ctf_event *simple_event;
	struct bt_ctf_field *integer_field;
	struct bt_ctf_field *float_field;
	struct bt_ctf_field *enum_field;
	struct bt_ctf_field *enum_field_unsigned;
	struct bt_ctf_field *enum_container_field;
	const char *mapping_name_test = "truie";
	const double double_test_value = 3.1415;
	struct bt_ctf_field *enum_container_field_unsigned;
	const char *mapping_name_negative_test = "negative_value";
	const char *ret_char;
	double ret_double;
	int64_t ret_range_start_int64_t, ret_range_end_int64_t;
	uint64_t ret_range_start_uint64_t, ret_range_end_uint64_t;
	struct bt_ctf_clock *ret_clock;
	struct bt_ctf_event_class *ret_event_class;
	struct bt_ctf_field *packet_context;
	struct bt_ctf_field *packet_context_field;
	struct bt_ctf_field *stream_event_context;
	struct bt_ctf_field *stream_event_context_field;
	struct bt_ctf_field *event_context;
	struct bt_ctf_field *event_context_field;

	ok(uint_12_type, "Create an unsigned integer type");

	bt_ctf_field_type_integer_set_signed(int_64_type, 1);
	ok(int_64_type, "Create a signed integer type");
	enum_type = bt_ctf_field_type_enumeration_create(int_64_type);

	returned_type = bt_ctf_field_type_enumeration_get_container_type(enum_type);
	ok(returned_type == int_64_type, "bt_ctf_field_type_enumeration_get_container_type returns the right type");
	ok(!bt_ctf_field_type_enumeration_get_container_type(NULL), "bt_ctf_field_type_enumeration_get_container_type handles NULL correctly");
	ok(!bt_ctf_field_type_enumeration_create(enum_type),
		"bt_ctf_field_enumeration_type_create rejects non-integer container field types");
	bt_put(returned_type);

	bt_ctf_field_type_set_alignment(float_type, 32);
	ok(bt_ctf_field_type_get_alignment(NULL) < 0,
		"bt_ctf_field_type_get_alignment handles NULL correctly");
	ok(bt_ctf_field_type_get_alignment(float_type) == 32,
		"bt_ctf_field_type_get_alignment returns a correct value");

	ok(bt_ctf_field_type_floating_point_set_exponent_digits(float_type, 11) == 0,
		"Set a floating point type's exponent digit count");
	ok(bt_ctf_field_type_floating_point_set_mantissa_digits(float_type, 53) == 0,
		"Set a floating point type's mantissa digit count");

	ok(bt_ctf_field_type_floating_point_get_exponent_digits(NULL) < 0,
		"bt_ctf_field_type_floating_point_get_exponent_digits handles NULL properly");
	ok(bt_ctf_field_type_floating_point_get_mantissa_digits(NULL) < 0,
		"bt_ctf_field_type_floating_point_get_mantissa_digits handles NULL properly");
	ok(bt_ctf_field_type_floating_point_get_exponent_digits(float_type) == 11,
		"bt_ctf_field_type_floating_point_get_exponent_digits returns the correct value");
	ok(bt_ctf_field_type_floating_point_get_mantissa_digits(float_type) == 53,
		"bt_ctf_field_type_floating_point_get_mantissa_digits returns the correct value");

	ok(bt_ctf_field_type_enumeration_add_mapping(enum_type,
		mapping_name_negative_test, -12345, 0) == 0,
		"bt_ctf_field_type_enumeration_add_mapping accepts negative enumeration mappings");
	ok(bt_ctf_field_type_enumeration_add_mapping(enum_type,
		"escaping; \"test\"", 1, 1) == 0,
		"bt_ctf_field_type_enumeration_add_mapping accepts enumeration mapping strings containing quotes");
	ok(bt_ctf_field_type_enumeration_add_mapping(enum_type,
		"\tanother \'escaping\'\n test\"", 2, 4) == 0,
		"bt_ctf_field_type_enumeration_add_mapping accepts enumeration mapping strings containing special characters");
	ok(bt_ctf_field_type_enumeration_add_mapping(enum_type,
		"event clock int float", 5, 22) == 0,
		"Accept enumeration mapping strings containing reserved keywords");
	bt_ctf_field_type_enumeration_add_mapping(enum_type, mapping_name_test,
		42, 42);
	ok(bt_ctf_field_type_enumeration_add_mapping(enum_type, mapping_name_test,
		43, 51), "bt_ctf_field_type_enumeration_add_mapping rejects duplicate mapping names");
	ok(bt_ctf_field_type_enumeration_add_mapping(enum_type, "something",
		-500, -400), "bt_ctf_field_type_enumeration_add_mapping rejects overlapping enum entries");
	ok(bt_ctf_field_type_enumeration_add_mapping(enum_type, mapping_name_test,
		-54, -55), "bt_ctf_field_type_enumeration_add_mapping rejects mapping where end < start");
	bt_ctf_field_type_enumeration_add_mapping(enum_type, "another entry", -42000, -13000);

	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_value(NULL, -42) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_value handles a NULL field type correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_value(enum_type, 1000000) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_value handles invalid values correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_value(enum_type, -55) == 1,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_value returns the correct index");

	ok(bt_ctf_event_class_add_field(simple_event_class, enum_type,
		"enum_field") == 0, "Add signed enumeration field to event");

	ok(bt_ctf_field_type_enumeration_get_mapping(NULL, 0, &ret_char,
		&ret_range_start_int64_t, &ret_range_end_int64_t) < 0,
		"bt_ctf_field_type_enumeration_get_mapping handles a NULL enumeration correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping(enum_type, 0, NULL,
		&ret_range_start_int64_t, &ret_range_end_int64_t) < 0,
		"bt_ctf_field_type_enumeration_get_mapping handles a NULL string correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping(enum_type, 0, &ret_char,
		NULL, &ret_range_end_int64_t) < 0,
		"bt_ctf_field_type_enumeration_get_mapping handles a NULL start correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping(enum_type, 0, &ret_char,
		&ret_range_start_int64_t, NULL) < 0,
		"bt_ctf_field_type_enumeration_get_mapping handles a NULL end correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping(enum_type, 5, &ret_char,
		&ret_range_start_int64_t, &ret_range_end_int64_t) == 0,
		"bt_ctf_field_type_enumeration_get_mapping returns a value");
	ok(!strcmp(ret_char, mapping_name_test),
		"bt_ctf_field_type_enumeration_get_mapping returns a correct mapping name");
	ok(ret_range_start_int64_t == 42,
		"bt_ctf_field_type_enumeration_get_mapping returns a correct mapping start");
	ok(ret_range_end_int64_t == 42,
		"bt_ctf_field_type_enumeration_get_mapping returns a correct mapping end");

	ok(bt_ctf_field_type_enumeration_add_mapping_unsigned(enum_type_unsigned,
		"escaping; \"test\"", 0, 0) == 0,
		"bt_ctf_field_type_enumeration_add_mapping_unsigned accepts enumeration mapping strings containing quotes");
	ok(bt_ctf_field_type_enumeration_add_mapping_unsigned(enum_type_unsigned,
		"\tanother \'escaping\'\n test\"", 1, 4) == 0,
		"bt_ctf_field_type_enumeration_add_mapping_unsigned accepts enumeration mapping strings containing special characters");
	ok(bt_ctf_field_type_enumeration_add_mapping_unsigned(enum_type_unsigned,
		"event clock int float", 5, 22) == 0,
		"bt_ctf_field_type_enumeration_add_mapping_unsigned accepts enumeration mapping strings containing reserved keywords");
	bt_ctf_field_type_enumeration_add_mapping_unsigned(enum_type_unsigned, mapping_name_test,
		42, 42);
	ok(bt_ctf_field_type_enumeration_add_mapping_unsigned(enum_type_unsigned, mapping_name_test,
		43, 51), "bt_ctf_field_type_enumeration_add_mapping_unsigned rejects duplicate mapping names");
	ok(bt_ctf_field_type_enumeration_add_mapping_unsigned(enum_type_unsigned, "something",
		7, 8), "bt_ctf_field_type_enumeration_add_mapping_unsigned rejects overlapping enum entries");
	ok(bt_ctf_field_type_enumeration_add_mapping_unsigned(enum_type_unsigned, mapping_name_test,
		55, 54), "bt_ctf_field_type_enumeration_add_mapping_unsigned rejects mapping where end < start");
	ok(bt_ctf_event_class_add_field(simple_event_class, enum_type_unsigned,
		"enum_field_unsigned") == 0, "Add unsigned enumeration field to event");

	ok(bt_ctf_field_type_enumeration_get_mapping_count(NULL) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_count handles NULL correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_count(enum_type_unsigned) == 4,
		"bt_ctf_field_type_enumeration_get_mapping_count returns the correct value");

	ok(bt_ctf_field_type_enumeration_get_mapping_unsigned(NULL, 0, &ret_char,
		&ret_range_start_uint64_t, &ret_range_end_uint64_t) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_unsigned handles a NULL enumeration correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_unsigned(enum_type_unsigned, 0, NULL,
		&ret_range_start_uint64_t, &ret_range_end_uint64_t) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_unsigned handles a NULL string correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_unsigned(enum_type_unsigned, 0, &ret_char,
		NULL, &ret_range_end_uint64_t) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_unsigned handles a NULL start correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_unsigned(enum_type_unsigned, 0, &ret_char,
		&ret_range_start_uint64_t, NULL) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_unsigned handles a NULL end correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_unsigned(enum_type_unsigned, 3, &ret_char,
		&ret_range_start_uint64_t, &ret_range_end_uint64_t) == 0,
		"bt_ctf_field_type_enumeration_get_mapping_unsigned returns a value");
	ok(!strcmp(ret_char, mapping_name_test),
		"bt_ctf_field_type_enumeration_get_mapping_unsigned returns a correct mapping name");
	ok(ret_range_start_uint64_t == 42,
		"bt_ctf_field_type_enumeration_get_mapping_unsigned returns a correct mapping start");
	ok(ret_range_end_uint64_t == 42,
		"bt_ctf_field_type_enumeration_get_mapping_unsigned returns a correct mapping end");

	bt_ctf_event_class_add_field(simple_event_class, uint_12_type,
		"integer_field");
	bt_ctf_event_class_add_field(simple_event_class, float_type,
		"float_field");

	assert(!bt_ctf_event_class_set_id(simple_event_class, 13));

	/* Set an event context type which will contain a single integer*/
	ok(!bt_ctf_field_type_structure_add_field(event_context_type, uint_12_type,
		"event_specific_context"),
		"Add event specific context field");
	ok(bt_ctf_event_class_get_context_type(NULL) == NULL,
		"bt_ctf_event_class_get_context_type handles NULL correctly");
	ok(bt_ctf_event_class_get_context_type(simple_event_class) == NULL,
		"bt_ctf_event_class_get_context_type returns NULL when no event context type is set");

	ok(bt_ctf_event_class_set_context_type(simple_event_class, NULL) < 0,
		"bt_ctf_event_class_set_context_type handles a NULL context type correctly");
	ok(bt_ctf_event_class_set_context_type(NULL, event_context_type) < 0,
		"bt_ctf_event_class_set_context_type handles a NULL event class correctly");
	ok(!bt_ctf_event_class_set_context_type(simple_event_class, event_context_type),
		"Set an event class' context type successfully");
	returned_type = bt_ctf_event_class_get_context_type(simple_event_class);
	ok(returned_type == event_context_type,
		"bt_ctf_event_class_get_context_type returns the appropriate type");
	bt_put(returned_type);

	bt_ctf_stream_class_add_event_class(stream_class, simple_event_class);

	ok(bt_ctf_stream_class_get_event_class_count(NULL) < 0,
		"bt_ctf_stream_class_get_event_class_count handles NULL correctly");
	ok(bt_ctf_stream_class_get_event_class_count(stream_class) == 1,
		"bt_ctf_stream_class_get_event_class_count returns a correct number of event classes");
	ok(bt_ctf_stream_class_get_event_class(NULL, 0) == NULL,
		"bt_ctf_stream_class_get_event_class handles NULL correctly");
	ok(bt_ctf_stream_class_get_event_class(stream_class, 8724) == NULL,
		"bt_ctf_stream_class_get_event_class handles invalid indexes correctly");
	ret_event_class = bt_ctf_stream_class_get_event_class(stream_class, 0);
	ok(ret_event_class == simple_event_class,
		"bt_ctf_stream_class_get_event_class returns the correct event class");
	bt_put(ret_event_class);
	ok(!bt_ctf_stream_class_get_event_class_by_id(NULL, 0),
		"bt_ctf_stream_class_get_event_class_by_id handles NULL correctly");
	ok(!bt_ctf_stream_class_get_event_class_by_id(stream_class, 2),
		"bt_ctf_stream_class_get_event_class_by_id returns NULL when the requested ID doesn't exist");
	ret_event_class =
		bt_ctf_stream_class_get_event_class_by_id(stream_class, 13);
	ok(ret_event_class == simple_event_class,
		"bt_ctf_stream_class_get_event_class_by_id returns a correct event class");
	bt_put(ret_event_class);

	ok(bt_ctf_stream_class_get_event_class_by_name(NULL, "some event name") == NULL,
		"bt_ctf_stream_class_get_event_class_by_name handles a NULL stream class correctly");
	ok(bt_ctf_stream_class_get_event_class_by_name(stream_class, NULL) == NULL,
		"bt_ctf_stream_class_get_event_class_by_name handles a NULL event class name correctly");
	ok(bt_ctf_stream_class_get_event_class_by_name(stream_class, "some event name") == NULL,
		"bt_ctf_stream_class_get_event_class_by_name handles non-existing event class names correctly");
	ret_event_class = bt_ctf_stream_class_get_event_class_by_name(stream_class, "Simple Event");
	ok(ret_event_class == simple_event_class,
		"bt_ctf_stream_class_get_event_class_by_name returns a correct event class");
	bt_put(ret_event_class);

	simple_event = bt_ctf_event_create(simple_event_class);
	ok(simple_event,
		"Instantiate an event containing a single integer field");

	ok(bt_ctf_event_get_clock(NULL) == NULL,
		"bt_ctf_event_get_clock handles NULL correctly");
	ret_clock = bt_ctf_event_get_clock(simple_event);
	ok(ret_clock == clock,
		"bt_ctf_event_get_clock returns a correct clock");
	bt_put(clock);

	integer_field = bt_ctf_field_create(uint_12_type);
	bt_ctf_field_unsigned_integer_set_value(integer_field, 42);
	ok(bt_ctf_event_set_payload(simple_event, "integer_field",
		integer_field) == 0, "Use bt_ctf_event_set_payload to set a manually allocated field");

	float_field = bt_ctf_event_get_payload(simple_event, "float_field");
	ok(bt_ctf_field_floating_point_get_value(float_field, &ret_double),
		"bt_ctf_field_floating_point_get_value fails on an unset float field");
	bt_ctf_field_floating_point_set_value(float_field, double_test_value);
	ok(bt_ctf_field_floating_point_get_value(NULL, &ret_double),
		"bt_ctf_field_floating_point_get_value properly handles a NULL field");
	ok(bt_ctf_field_floating_point_get_value(float_field, NULL),
		"bt_ctf_field_floating_point_get_value properly handles a NULL return value pointer");
	ok(!bt_ctf_field_floating_point_get_value(float_field, &ret_double),
		"bt_ctf_field_floating_point_get_value returns a double value");
	ok(fabs(ret_double - double_test_value) <= DBL_EPSILON,
		"bt_ctf_field_floating_point_get_value returns a correct value");

	enum_field = bt_ctf_field_create(enum_type);
	ret_char = bt_ctf_field_enumeration_get_mapping_name(NULL);
	ok(!ret_char, "bt_ctf_field_enumeration_get_mapping_name handles NULL correctly");
	ret_char = bt_ctf_field_enumeration_get_mapping_name(enum_field);
	ok(!ret_char, "bt_ctf_field_enumeration_get_mapping_name returns NULL if the enumeration's container field is unset");
	enum_container_field = bt_ctf_field_enumeration_get_container(
		enum_field);
	ok(bt_ctf_field_signed_integer_set_value(
		enum_container_field, -42) == 0,
		"Set signed enumeration container value");
	ret_char = bt_ctf_field_enumeration_get_mapping_name(enum_field);
	ok(!strcmp(ret_char, mapping_name_negative_test),
		"bt_ctf_field_enumeration_get_mapping_name returns the correct mapping name with an signed container");
	bt_ctf_event_set_payload(simple_event, "enum_field", enum_field);

	enum_field_unsigned = bt_ctf_field_create(enum_type_unsigned);
	enum_container_field_unsigned = bt_ctf_field_enumeration_get_container(
		enum_field_unsigned);
	ok(bt_ctf_field_unsigned_integer_set_value(
		enum_container_field_unsigned, 42) == 0,
		"Set unsigned enumeration container value");
	bt_ctf_event_set_payload(simple_event, "enum_field_unsigned",
		enum_field_unsigned);
	ret_char = bt_ctf_field_enumeration_get_mapping_name(enum_field_unsigned);
	ok(ret_char && !strcmp(ret_char, mapping_name_test),
		"bt_ctf_field_enumeration_get_mapping_name returns the correct mapping name with an unsigned container");

	ok(bt_ctf_clock_set_time(clock, current_time) == 0, "Set clock time");

	/* Populate stream event context */
	stream_event_context = bt_ctf_stream_get_event_context(stream);
	stream_event_context_field = bt_ctf_field_structure_get_field(
		stream_event_context, "common_event_context");
	bt_ctf_field_unsigned_integer_set_value(stream_event_context_field, 42);

	/* Populate the event's context */
	ok(bt_ctf_event_get_event_context(NULL) == NULL,
		"bt_ctf_event_get_event_context handles NULL correctly");
	event_context = bt_ctf_event_get_event_context(simple_event);
	ok(event_context,
		"bt_ctf_event_get_event_context returns a field");
	returned_type = bt_ctf_field_get_type(event_context);
	ok(returned_type == event_context_type,
		"bt_ctf_event_get_event_context returns a field of the appropriate type");
	event_context_field = bt_ctf_field_structure_get_field(event_context,
		"event_specific_context");
	ok(!bt_ctf_field_unsigned_integer_set_value(event_context_field, 1234),
		"Successfully set an event context's value");
	ok(bt_ctf_event_set_event_context(NULL, event_context) < 0,
		"bt_ctf_event_set_event_context handles a NULL event correctly");
	ok(bt_ctf_event_set_event_context(simple_event, NULL) < 0,
		"bt_ctf_event_set_event_context handles a NULL event context correctly");
	ok(bt_ctf_event_set_event_context(simple_event, event_context_field) < 0,
		"bt_ctf_event_set_event_context rejects a context of the wrong type");
	ok(!bt_ctf_event_set_event_context(simple_event, event_context),
		"Set an event context successfully");

	event_copy_tests(simple_event);
	ok(bt_ctf_stream_append_event(stream, simple_event) == 0,
		"Append simple event to trace stream");

	ok(bt_ctf_stream_get_packet_context(NULL) == NULL,
		"bt_ctf_stream_get_packet_context handles NULL correctly");
	packet_context = bt_ctf_stream_get_packet_context(stream);
	ok(packet_context,
		"bt_ctf_stream_get_packet_context returns a packet context");

	packet_context_field = bt_ctf_field_structure_get_field(packet_context,
		"packet_size");
	ok(packet_context_field,
		"Packet context contains the default packet_size field.");
	bt_put(packet_context_field);
	packet_context_field = bt_ctf_field_structure_get_field(packet_context,
		"custom_packet_context_field");
	ok(bt_ctf_field_unsigned_integer_set_value(packet_context_field, 8) == 0,
		"Custom packet context field value successfully set.");

	ok(bt_ctf_stream_set_packet_context(NULL, packet_context_field) < 0,
		"bt_ctf_stream_set_packet_context handles a NULL stream correctly");
	ok(bt_ctf_stream_set_packet_context(stream, NULL) < 0,
		"bt_ctf_stream_set_packet_context handles a NULL packet context correctly");
	ok(bt_ctf_stream_set_packet_context(stream, packet_context) == 0,
		"Successfully set a stream's packet context");

	ok(bt_ctf_stream_flush(stream) == 0,
		"Flush trace stream with one event");

	bt_put(simple_event_class);
	bt_put(simple_event);
	bt_put(uint_12_type);
	bt_put(int_64_type);
	bt_put(float_type);
	bt_put(enum_type);
	bt_put(enum_type_unsigned);
	bt_put(returned_type);
	bt_put(event_context_type);
	bt_put(integer_field);
	bt_put(float_field);
	bt_put(enum_field);
	bt_put(enum_field_unsigned);
	bt_put(enum_container_field);
	bt_put(enum_container_field_unsigned);
	bt_put(packet_context);
	bt_put(packet_context_field);
	bt_put(stream_event_context);
	bt_put(stream_event_context_field);
	bt_put(event_context);
	bt_put(event_context_field);
}

void append_complex_event(struct bt_ctf_stream_class *stream_class,
		struct bt_ctf_stream *stream, struct bt_ctf_clock *clock)
{
	struct event_class_attrs_counts {
		int id;
		int name;
		int loglevel;
		int modelemfuri;
		int unknown;
	} attrs_count;

	int i;
	int ret;
	int64_t int64_value;
	struct event_class_attrs_counts ;
	const char *complex_test_event_string = "Complex Test Event";
	const char *test_string_1 = "Test ";
	const char *test_string_2 = "string ";
	const char *test_string_3 = "abcdefghi";
	const char *test_string_4 = "abcd\0efg\0hi";
	const char *test_string_cat = "Test string abcdeabcd";
	struct bt_ctf_field_type *uint_35_type =
		bt_ctf_field_type_integer_create(35);
	struct bt_ctf_field_type *int_16_type =
		bt_ctf_field_type_integer_create(16);
	struct bt_ctf_field_type *uint_3_type =
		bt_ctf_field_type_integer_create(3);
	struct bt_ctf_field_type *enum_variant_type =
		bt_ctf_field_type_enumeration_create(uint_3_type);
	struct bt_ctf_field_type *variant_type =
		bt_ctf_field_type_variant_create(enum_variant_type,
			"variant_selector");
	struct bt_ctf_field_type *string_type =
		bt_ctf_field_type_string_create();
	struct bt_ctf_field_type *sequence_type;
	struct bt_ctf_field_type *array_type;
	struct bt_ctf_field_type *inner_structure_type =
		bt_ctf_field_type_structure_create();
	struct bt_ctf_field_type *complex_structure_type =
		bt_ctf_field_type_structure_create();
	struct bt_ctf_field_type *ret_field_type;
	struct bt_ctf_event_class *event_class;
	struct bt_ctf_event *event;
	struct bt_ctf_field *uint_35_field, *int_16_field, *a_string_field,
		*inner_structure_field, *complex_structure_field,
		*a_sequence_field, *enum_variant_field, *enum_container_field,
		*variant_field, *an_array_field, *ret_field;
	uint64_t ret_unsigned_int;
	int64_t ret_signed_int;
	const char *ret_string;
	struct bt_ctf_stream_class *ret_stream_class;
	struct bt_ctf_event_class *ret_event_class;
	struct bt_ctf_field *packet_context, *packet_context_field;
	struct bt_value *obj;

	ok(bt_ctf_field_type_set_alignment(int_16_type, 0),
		"bt_ctf_field_type_set_alignment handles 0-alignment correctly");
	ok(bt_ctf_field_type_set_alignment(int_16_type, 3),
		"bt_ctf_field_type_set_alignment handles wrong alignment correctly (3)");
	ok(bt_ctf_field_type_set_alignment(int_16_type, 24),
		"bt_ctf_field_type_set_alignment handles wrong alignment correctly (24)");
	ok(!bt_ctf_field_type_set_alignment(int_16_type, 4),
		"bt_ctf_field_type_set_alignment handles correct alignment correctly (4)");
	bt_ctf_field_type_set_alignment(int_16_type, 32);
	bt_ctf_field_type_integer_set_signed(int_16_type, 1);
	bt_ctf_field_type_integer_set_base(uint_35_type,
		BT_CTF_INTEGER_BASE_HEXADECIMAL);

	array_type = bt_ctf_field_type_array_create(int_16_type, ARRAY_TEST_LENGTH);
	sequence_type = bt_ctf_field_type_sequence_create(int_16_type,
		"seq_len");

	ok(bt_ctf_field_type_array_get_element_type(NULL) == NULL,
		"bt_ctf_field_type_array_get_element_type handles NULL correctly");
	ret_field_type = bt_ctf_field_type_array_get_element_type(
		array_type);
	ok(ret_field_type == int_16_type,
		"bt_ctf_field_type_array_get_element_type returns the correct type");
	bt_put(ret_field_type);

	ok(bt_ctf_field_type_array_get_length(NULL) < 0,
		"bt_ctf_field_type_array_get_length handles NULL correctly");
	ok(bt_ctf_field_type_array_get_length(array_type) == ARRAY_TEST_LENGTH,
		"bt_ctf_field_type_array_get_length returns the correct length");

	bt_ctf_field_type_structure_add_field(inner_structure_type,
		uint_35_type, "seq_len");
	bt_ctf_field_type_structure_add_field(inner_structure_type,
		sequence_type, "a_sequence");
	bt_ctf_field_type_structure_add_field(inner_structure_type,
		array_type, "an_array");

	bt_ctf_field_type_enumeration_add_mapping(enum_variant_type,
		"UINT3_TYPE", 0, 0);
	bt_ctf_field_type_enumeration_add_mapping(enum_variant_type,
		"INT16_TYPE", 1, 1);
	bt_ctf_field_type_enumeration_add_mapping(enum_variant_type,
		"UINT35_TYPE", 2, 7);

	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_name(NULL,
		"INT16_TYPE") < 0,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_name handles a NULL field type correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_name(
		enum_variant_type, NULL) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_name handles a NULL name correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_name(
		enum_variant_type, "INT16_TYPE") == 1,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_name returns the correct index");

	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_unsigned_value(NULL, 1) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_unsigned_value handles a NULL field type correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_unsigned_value(enum_variant_type, -42) < 0,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_unsigned_value handles invalid values correctly");
	ok(bt_ctf_field_type_enumeration_get_mapping_index_by_unsigned_value(enum_variant_type, 5) == 2,
		"bt_ctf_field_type_enumeration_get_mapping_index_by_unsigned_value returns the correct index");

	ok(bt_ctf_field_type_variant_add_field(variant_type, uint_3_type,
		"An unknown entry"), "Reject a variant field based on an unknown tag value");
	ok(bt_ctf_field_type_variant_add_field(variant_type, uint_3_type,
		"UINT3_TYPE") == 0, "Add a field to a variant");
	bt_ctf_field_type_variant_add_field(variant_type, int_16_type,
		"INT16_TYPE");
	bt_ctf_field_type_variant_add_field(variant_type, uint_35_type,
		"UINT35_TYPE");

	ok(bt_ctf_field_type_variant_get_tag_type(NULL) == NULL,
		"bt_ctf_field_type_variant_get_tag_type handles NULL correctly");
	ret_field_type = bt_ctf_field_type_variant_get_tag_type(variant_type);
	ok(ret_field_type == enum_variant_type,
		"bt_ctf_field_type_variant_get_tag_type returns a correct tag type");
	bt_put(ret_field_type);

	ok(bt_ctf_field_type_variant_get_tag_name(NULL) == NULL,
		"bt_ctf_field_type_variant_get_tag_name handles NULL correctly");
	ret_string = bt_ctf_field_type_variant_get_tag_name(variant_type);
	ok(ret_string ? !strcmp(ret_string, "variant_selector") : 0,
		"bt_ctf_field_type_variant_get_tag_name returns the correct variant tag name");
	ok(bt_ctf_field_type_variant_get_field_type_by_name(NULL,
		"INT16_TYPE") == NULL,
		"bt_ctf_field_type_variant_get_field_type_by_name handles a NULL variant_type correctly");
	ok(bt_ctf_field_type_variant_get_field_type_by_name(variant_type,
		NULL) == NULL,
		"bt_ctf_field_type_variant_get_field_type_by_name handles a NULL field name correctly");
	ret_field_type = bt_ctf_field_type_variant_get_field_type_by_name(
		variant_type, "INT16_TYPE");
	ok(ret_field_type == int_16_type,
		"bt_ctf_field_type_variant_get_field_type_by_name returns a correct field type");
	bt_put(ret_field_type);

	ok(bt_ctf_field_type_variant_get_field_count(NULL) < 0,
		"bt_ctf_field_type_variant_get_field_count handles NULL correctly");
	ok(bt_ctf_field_type_variant_get_field_count(variant_type) == 3,
		"bt_ctf_field_type_variant_get_field_count returns the correct count");

	ok(bt_ctf_field_type_variant_get_field(NULL, &ret_string, &ret_field_type, 0) < 0,
		"bt_ctf_field_type_variant_get_field handles a NULL type correctly");
	ok(bt_ctf_field_type_variant_get_field(variant_type, NULL, &ret_field_type, 0) == 0,
		"bt_ctf_field_type_variant_get_field handles a NULL field name correctly");
	bt_put(ret_field_type);
	ok(bt_ctf_field_type_variant_get_field(variant_type, &ret_string, NULL, 0) == 0,
		"bt_ctf_field_type_variant_get_field handles a NULL field type correctly");
	ok(bt_ctf_field_type_variant_get_field(variant_type, &ret_string, &ret_field_type, 200) < 0,
		"bt_ctf_field_type_variant_get_field handles an invalid index correctly");
	ok(bt_ctf_field_type_variant_get_field(variant_type, &ret_string, &ret_field_type, 1) == 0,
		"bt_ctf_field_type_variant_get_field returns a field");
	ok(!strcmp("INT16_TYPE", ret_string),
		"bt_ctf_field_type_variant_get_field returns a correct field name");
	ok(ret_field_type == int_16_type,
		"bt_ctf_field_type_variant_get_field returns a correct field type");
	bt_put(ret_field_type);

	bt_ctf_field_type_structure_add_field(complex_structure_type,
		enum_variant_type, "variant_selector");
	bt_ctf_field_type_structure_add_field(complex_structure_type,
		string_type, "a_string");
	bt_ctf_field_type_structure_add_field(complex_structure_type,
		variant_type, "variant_value");
	bt_ctf_field_type_structure_add_field(complex_structure_type,
		inner_structure_type, "inner_structure");

	ok(bt_ctf_event_class_create("clock") == NULL,
		"Reject creation of an event class with an illegal name");
	event_class = bt_ctf_event_class_create(complex_test_event_string);
	ok(event_class, "Create an event class");
	ok(bt_ctf_event_class_add_field(event_class, uint_35_type, ""),
		"Reject addition of a field with an empty name to an event");
	ok(bt_ctf_event_class_add_field(event_class, NULL, "an_integer"),
		"Reject addition of a field with a NULL type to an event");
	ok(bt_ctf_event_class_add_field(event_class, uint_35_type,
		"int"),
		"Reject addition of a type with an illegal name to an event");
	ok(bt_ctf_event_class_add_field(event_class, uint_35_type,
		"uint_35") == 0,
		"Add field of type unsigned integer to an event");
	ok(bt_ctf_event_class_add_field(event_class, int_16_type,
		"int_16") == 0, "Add field of type signed integer to an event");
	ok(bt_ctf_event_class_add_field(event_class, complex_structure_type,
		"complex_structure") == 0,
		"Add composite structure to an event");

	ok(bt_ctf_event_class_get_name(NULL) == NULL,
		"bt_ctf_event_class_get_name handles NULL correctly");
	ret_string = bt_ctf_event_class_get_name(event_class);
	ok(!strcmp(ret_string, complex_test_event_string),
		"bt_ctf_event_class_get_name returns a correct name");
	ok(bt_ctf_event_class_get_id(event_class) < 0,
		"bt_ctf_event_class_get_id returns a negative value when not set");
	ok(bt_ctf_event_class_get_id(NULL) < 0,
		"bt_ctf_event_class_get_id handles NULL correctly");
	ok(bt_ctf_event_class_set_id(NULL, 42) < 0,
		"bt_ctf_event_class_set_id handles NULL correctly");
	ok(bt_ctf_event_class_set_id(event_class, 42) == 0,
		"Set an event class' id");
	ok(bt_ctf_event_class_get_id(event_class) == 42,
		"bt_ctf_event_class_get_id returns the correct value");

	/* Test event class attributes */
	obj = bt_value_integer_create_init(15);
	assert(obj);
	ok(bt_ctf_event_class_set_attribute(NULL, "id", obj),
		"bt_ctf_event_class_set_attribute handles a NULL event class correctly");
	ok(bt_ctf_event_class_set_attribute(event_class, NULL, obj),
		"bt_ctf_event_class_set_attribute handles a NULL name correctly");
	ok(bt_ctf_event_class_set_attribute(event_class, "id", NULL),
		"bt_ctf_event_class_set_attribute handles a NULL value correctly");
	assert(!bt_value_integer_set(obj, -3));
	ok(bt_ctf_event_class_set_attribute(event_class, "id", obj),
		"bt_ctf_event_class_set_attribute fails with a negative \"id\" attribute");
	assert(!bt_value_integer_set(obj, 11));
	ret = bt_ctf_event_class_set_attribute(event_class, "id", obj);
	ok(!ret && bt_ctf_event_class_get_id(event_class) == 11,
		"bt_ctf_event_class_set_attribute succeeds in replacing the existing \"id\" attribute");
	ret = bt_ctf_event_class_set_attribute(event_class, "name", obj);
	ret &= bt_ctf_event_class_set_attribute(event_class, "model.emf.uri", obj);
	ok(ret,
		"bt_ctf_event_class_set_attribute cannot set \"name\" or \"model.emf.uri\" to an integer value");
	BT_PUT(obj);

	obj = bt_value_integer_create_init(5);
	assert(obj);
	ok(!bt_ctf_event_class_set_attribute(event_class, "loglevel", obj),
		"bt_ctf_event_class_set_attribute succeeds in setting the \"loglevel\" attribute");
	BT_PUT(obj);
	ok(!bt_ctf_event_class_get_attribute_value_by_name(NULL, "loglevel"),
		"bt_ctf_event_class_get_attribute_value_by_name handles a NULL event class correctly");
	ok(!bt_ctf_event_class_get_attribute_value_by_name(event_class, NULL),
		"bt_ctf_event_class_get_attribute_value_by_name handles a NULL name correctly");
	ok(!bt_ctf_event_class_get_attribute_value_by_name(event_class, "meow"),
		"bt_ctf_event_class_get_attribute_value_by_name fails with a non-existing attribute name");
	obj = bt_ctf_event_class_get_attribute_value_by_name(event_class,
		"loglevel");
	int64_value = 0;
	ret = bt_value_integer_get(obj, &int64_value);
	ok(obj && !ret && int64_value == 5,
		"bt_ctf_event_class_get_attribute_value_by_name returns the correct value");
	BT_PUT(obj);

	obj = bt_value_string_create_init("nu name");
	assert(obj);
	assert(!bt_ctf_event_class_set_attribute(event_class, "name", obj));
	ret_string = bt_ctf_event_class_get_name(event_class);
	ok(!strcmp(ret_string, "nu name"),
		"bt_ctf_event_class_set_attribute succeeds in replacing the existing \"name\" attribute");
	ret = bt_ctf_event_class_set_attribute(event_class, "id", obj);
	ret &= bt_ctf_event_class_set_attribute(event_class, "loglevel", obj);
	ok(ret,
		"bt_ctf_event_class_set_attribute cannot set \"id\" or \"loglevel\" to a string value");
	BT_PUT(obj);
	obj = bt_value_string_create_init("http://kernel.org/");
	assert(obj);
	assert(!bt_ctf_event_class_set_attribute(event_class, "model.emf.uri", obj));
	BT_PUT(obj);

	ok(bt_ctf_event_class_get_attribute_count(NULL),
		"bt_ctf_event_class_get_attribute_count handles a NULL event class");
	ok(bt_ctf_event_class_get_attribute_count(event_class) == 4,
		"bt_ctf_event_class_get_attribute_count returns the correct count");
	ok(!bt_ctf_event_class_get_attribute_name(NULL, 0),
		"bt_ctf_event_class_get_attribute_name handles a NULL event class correctly");
	ok(!bt_ctf_event_class_get_attribute_name(event_class, 4),
		"bt_ctf_event_class_get_attribute_name handles a too large index correctly");
	ok(!bt_ctf_event_class_get_attribute_value(NULL, 0),
		"bt_ctf_event_class_get_attribute_value handles a NULL event class correctly");
	ok(!bt_ctf_event_class_get_attribute_value(event_class, 4),
		"bt_ctf_event_class_get_attribute_value handles a too large index correctly");

	memset(&attrs_count, 0, sizeof(attrs_count));

	for (i = 0; i < 4; ++i) {
		ret_string = bt_ctf_event_class_get_attribute_name(event_class,
			i);
		obj = bt_ctf_event_class_get_attribute_value(event_class, i);
		assert(ret_string && obj);

		if (!strcmp(ret_string, "id")) {
			attrs_count.id++;
			ok(bt_value_is_integer(obj),
				"bt_ctf_event_class_get_attribute_value returns the correct type (\"%s\")",
				ret_string);
		} else if (!strcmp(ret_string, "name")) {
			attrs_count.name++;
			ok(bt_value_is_string(obj),
				"bt_ctf_event_class_get_attribute_value returns the correct type (\"%s\")",
				ret_string);
		} else if (!strcmp(ret_string, "loglevel")) {
			attrs_count.loglevel++;
			ok(bt_value_is_integer(obj),
				"bt_ctf_event_class_get_attribute_value returns the correct type (\"%s\")",
				ret_string);
		} else if (!strcmp(ret_string, "model.emf.uri")) {
			attrs_count.modelemfuri++;
			ok(bt_value_is_string(obj),
				"bt_ctf_event_class_get_attribute_value returns the correct type (\"%s\")",
				ret_string);
		} else {
			attrs_count.unknown++;
		}

		BT_PUT(obj);
	}

	ok(attrs_count.unknown == 0, "event class has no unknown attributes");
	ok(attrs_count.id == 1 && attrs_count.name == 1 &&
		attrs_count.loglevel == 1 && attrs_count.modelemfuri == 1,
		"event class has one instance of each known attribute");

	/* Add event class to the stream class */
	ok(bt_ctf_stream_class_add_event_class(stream_class, NULL),
		"Reject addition of NULL event class to a stream class");
	ok(bt_ctf_stream_class_add_event_class(stream_class,
		event_class) == 0, "Add an event class to stream class");

	ok(bt_ctf_event_class_get_stream_class(NULL) == NULL,
		"bt_ctf_event_class_get_stream_class handles NULL correctly");
	ret_stream_class = bt_ctf_event_class_get_stream_class(event_class);
	ok(ret_stream_class == stream_class,
		"bt_ctf_event_class_get_stream_class returns the correct stream class");
	bt_put(ret_stream_class);

	ok(bt_ctf_event_class_get_field_count(NULL) < 0,
		"bt_ctf_event_class_get_field_count handles NULL correctly");
	ok(bt_ctf_event_class_get_field_count(event_class) == 3,
		"bt_ctf_event_class_get_field_count returns a correct value");

	ok(bt_ctf_event_class_get_field(NULL, &ret_string,
		&ret_field_type, 0) < 0,
		"bt_ctf_event_class_get_field handles a NULL event class correctly");
	ok(bt_ctf_event_class_get_field(event_class, NULL,
		&ret_field_type, 0) == 0,
		"bt_ctf_event_class_get_field handles a NULL field name correctly");
	bt_put(ret_field_type);
	ok(bt_ctf_event_class_get_field(event_class, &ret_string,
		NULL, 0) == 0,
		"bt_ctf_event_class_get_field handles a NULL field type correctly");
	ok(bt_ctf_event_class_get_field(event_class, &ret_string,
		&ret_field_type, 42) < 0,
		"bt_ctf_event_class_get_field handles an invalid index correctly");
	ok(bt_ctf_event_class_get_field(event_class, &ret_string,
		&ret_field_type, 0) == 0,
		"bt_ctf_event_class_get_field returns a field");
	ok(ret_field_type == uint_35_type,
		"bt_ctf_event_class_get_field returns a correct field type");
	bt_put(ret_field_type);
	ok(!strcmp(ret_string, "uint_35"),
		"bt_ctf_event_class_get_field returns a correct field name");
	ok(bt_ctf_event_class_get_field_by_name(NULL, "") == NULL,
		"bt_ctf_event_class_get_field_by_name handles a NULL event class correctly");
	ok(bt_ctf_event_class_get_field_by_name(event_class, NULL) == NULL,
		"bt_ctf_event_class_get_field_by_name handles a NULL field name correctly");
	ok(bt_ctf_event_class_get_field_by_name(event_class, "truie") == NULL,
		"bt_ctf_event_class_get_field_by_name handles an invalid field name correctly");
	ret_field_type = bt_ctf_event_class_get_field_by_name(event_class,
		"complex_structure");
	ok(ret_field_type == complex_structure_type,
		"bt_ctf_event_class_get_field_by_name returns a correct field type");
	bt_put(ret_field_type);

	event = bt_ctf_event_create(event_class);
	ok(event, "Instanciate a complex event");

	ok(bt_ctf_event_get_class(NULL) == NULL,
		"bt_ctf_event_get_class handles NULL correctly");
	ret_event_class = bt_ctf_event_get_class(event);
	ok(ret_event_class == event_class,
		"bt_ctf_event_get_class returns the correct event class");
	bt_put(ret_event_class);

	uint_35_field = bt_ctf_event_get_payload(event, "uint_35");
	if (!uint_35_field) {
		printf("uint_35_field is NULL\n");
	}

	ok(uint_35_field, "Use bt_ctf_event_get_payload to get a field instance ");
	bt_ctf_field_unsigned_integer_set_value(uint_35_field, 0x0DDF00D);
	ok(bt_ctf_field_unsigned_integer_get_value(NULL, &ret_unsigned_int) < 0,
		"bt_ctf_field_unsigned_integer_get_value properly properly handles a NULL field.");
	ok(bt_ctf_field_unsigned_integer_get_value(uint_35_field, NULL) < 0,
		"bt_ctf_field_unsigned_integer_get_value properly handles a NULL return value");
	ok(bt_ctf_field_unsigned_integer_get_value(uint_35_field,
		&ret_unsigned_int) == 0,
		"bt_ctf_field_unsigned_integer_get_value succeeds after setting a value");
	ok(ret_unsigned_int == 0x0DDF00D,
		"bt_ctf_field_unsigned_integer_get_value returns the correct value");
	ok(bt_ctf_field_signed_integer_get_value(uint_35_field,
		&ret_signed_int) < 0,
		"bt_ctf_field_signed_integer_get_value fails on an unsigned field");
	bt_put(uint_35_field);

	int_16_field = bt_ctf_event_get_payload(event, "int_16");
	bt_ctf_field_signed_integer_set_value(int_16_field, -12345);
	ok(bt_ctf_field_signed_integer_get_value(NULL, &ret_signed_int) < 0,
		"bt_ctf_field_signed_integer_get_value properly handles a NULL field");
	ok(bt_ctf_field_signed_integer_get_value(int_16_field, NULL) < 0,
		"bt_ctf_field_signed_integer_get_value properly handles a NULL return value");
	ok(bt_ctf_field_signed_integer_get_value(int_16_field,
		&ret_signed_int) == 0,
		"bt_ctf_field_signed_integer_get_value succeeds after setting a value");
	ok(ret_signed_int == -12345,
		"bt_ctf_field_signed_integer_get_value returns the correct value");
	ok(bt_ctf_field_unsigned_integer_get_value(int_16_field,
		&ret_unsigned_int) < 0,
		"bt_ctf_field_unsigned_integer_get_value fails on a signed field");
	bt_put(int_16_field);

	complex_structure_field = bt_ctf_event_get_payload(event,
		"complex_structure");

	ok(bt_ctf_field_structure_get_field_by_index(NULL, 0) == NULL,
		"bt_ctf_field_structure_get_field_by_index handles NULL correctly");
	ok(bt_ctf_field_structure_get_field_by_index(NULL, 9) == NULL,
		"bt_ctf_field_structure_get_field_by_index handles an invalid index correctly");
	inner_structure_field = bt_ctf_field_structure_get_field_by_index(
		complex_structure_field, 3);
	ret_field_type = bt_ctf_field_get_type(inner_structure_field);
	bt_put(inner_structure_field);
	ok(ret_field_type == inner_structure_type,
		"bt_ctf_field_structure_get_field_by_index returns a correct field");
	bt_put(ret_field_type);

	inner_structure_field = bt_ctf_field_structure_get_field(
		complex_structure_field, "inner_structure");
	a_string_field = bt_ctf_field_structure_get_field(
		complex_structure_field, "a_string");
	enum_variant_field = bt_ctf_field_structure_get_field(
		complex_structure_field, "variant_selector");
	variant_field = bt_ctf_field_structure_get_field(
		complex_structure_field, "variant_value");
	uint_35_field = bt_ctf_field_structure_get_field(
		inner_structure_field, "seq_len");
	a_sequence_field = bt_ctf_field_structure_get_field(
		inner_structure_field, "a_sequence");
	an_array_field = bt_ctf_field_structure_get_field(
		inner_structure_field, "an_array");

	enum_container_field = bt_ctf_field_enumeration_get_container(
		enum_variant_field);
	bt_ctf_field_unsigned_integer_set_value(enum_container_field, 1);
	int_16_field = bt_ctf_field_variant_get_field(variant_field,
		enum_variant_field);
	bt_ctf_field_signed_integer_set_value(int_16_field, -200);
	bt_put(int_16_field);
	ok(!bt_ctf_field_string_get_value(a_string_field),
		"bt_ctf_field_string_get_value returns NULL on an unset field");
	bt_ctf_field_string_set_value(a_string_field,
		test_string_1);
	ok(!bt_ctf_field_string_get_value(NULL),
		"bt_ctf_field_string_get_value correctly handles NULL");
	ok(bt_ctf_field_string_append(NULL, "yeah"),
		"bt_ctf_field_string_append correctly handles a NULL string field");
	ok(bt_ctf_field_string_append(a_string_field, NULL),
		"bt_ctf_field_string_append correctly handles a NULL string value");
	ok(!bt_ctf_field_string_append(a_string_field, test_string_2),
		"bt_ctf_field_string_append succeeds");
	ok(bt_ctf_field_string_append_len(NULL, "oh noes", 3),
		"bt_ctf_field_string_append_len correctly handles a NULL string field");
	ok(bt_ctf_field_string_append_len(a_string_field, NULL, 3),
		"bt_ctf_field_string_append_len correctly handles a NULL string value");
	ok(!bt_ctf_field_string_append_len(a_string_field, test_string_3, 5),
		"bt_ctf_field_string_append_len succeeds (append 5 characters)");
	ok(!bt_ctf_field_string_append_len(a_string_field, test_string_4, 10),
		"bt_ctf_field_string_append_len succeeds (append 4 characters)");
	ok(!bt_ctf_field_string_append_len(a_string_field, &test_string_4[4], 3),
		"bt_ctf_field_string_append_len succeeds (append 0 characters)");
	ok(!bt_ctf_field_string_append_len(a_string_field, test_string_3, 0),
		"bt_ctf_field_string_append_len succeeds (append 0 characters)");

	ret_string = bt_ctf_field_string_get_value(a_string_field);
	ok(ret_string, "bt_ctf_field_string_get_value returns a string");
	ok(ret_string ? !strcmp(ret_string, test_string_cat) : 0,
		"bt_ctf_field_string_get_value returns a correct value");
	bt_ctf_field_unsigned_integer_set_value(uint_35_field,
		SEQUENCE_TEST_LENGTH);

	ok(bt_ctf_field_type_variant_get_field_type_from_tag(NULL,
		enum_container_field) == NULL,
		"bt_ctf_field_type_variant_get_field_type_from_tag handles a NULL variant type correctly");
	ok(bt_ctf_field_type_variant_get_field_type_from_tag(variant_type,
		NULL) == NULL,
		"bt_ctf_field_type_variant_get_field_type_from_tag handles a NULL tag correctly");
	ret_field_type = bt_ctf_field_type_variant_get_field_type_from_tag(
		variant_type, enum_variant_field);
	ok(ret_field_type == int_16_type,
		"bt_ctf_field_type_variant_get_field_type_from_tag returns the correct field type");

	ok(bt_ctf_field_sequence_get_length(a_sequence_field) == NULL,
		"bt_ctf_field_sequence_get_length returns NULL when length is unset");
	ok(bt_ctf_field_sequence_set_length(a_sequence_field,
		uint_35_field) == 0, "Set a sequence field's length");
	ret_field = bt_ctf_field_sequence_get_length(a_sequence_field);
	ok(ret_field == uint_35_field,
		"bt_ctf_field_sequence_get_length returns the correct length field");
	ok(bt_ctf_field_sequence_get_length(NULL) == NULL,
		"bt_ctf_field_sequence_get_length properly handles NULL");

	for (i = 0; i < SEQUENCE_TEST_LENGTH; i++) {
		int_16_field = bt_ctf_field_sequence_get_field(
			a_sequence_field, i);
		bt_ctf_field_signed_integer_set_value(int_16_field, 4 - i);
		bt_put(int_16_field);
	}

	for (i = 0; i < ARRAY_TEST_LENGTH; i++) {
		int_16_field = bt_ctf_field_array_get_field(
			an_array_field, i);
		bt_ctf_field_signed_integer_set_value(int_16_field, i);
		bt_put(int_16_field);
	}

	bt_ctf_clock_set_time(clock, ++current_time);
	ok(bt_ctf_stream_append_event(stream, event) == 0,
		"Append a complex event to a stream");

	/*
	 * Populate the custom packet context field with a dummy value
	 * otherwise flush will fail.
	 */
	packet_context = bt_ctf_stream_get_packet_context(stream);
	packet_context_field = bt_ctf_field_structure_get_field(packet_context,
		"custom_packet_context_field");
	bt_ctf_field_unsigned_integer_set_value(packet_context_field, 1);

	ok(bt_ctf_stream_flush(stream) == 0,
		"Flush a stream containing a complex event");

	bt_put(uint_35_field);
	bt_put(a_string_field);
	bt_put(inner_structure_field);
	bt_put(complex_structure_field);
	bt_put(a_sequence_field);
	bt_put(an_array_field);
	bt_put(enum_variant_field);
	bt_put(enum_container_field);
	bt_put(variant_field);
	bt_put(ret_field);
	bt_put(packet_context_field);
	bt_put(packet_context);
	bt_put(uint_35_type);
	bt_put(int_16_type);
	bt_put(string_type);
	bt_put(sequence_type);
	bt_put(array_type);
	bt_put(inner_structure_type);
	bt_put(complex_structure_type);
	bt_put(uint_3_type);
	bt_put(enum_variant_type);
	bt_put(variant_type);
	bt_put(ret_field_type);
	bt_put(event_class);
	bt_put(event);
}

static void field_copy_tests_validate_same_type(struct bt_ctf_field *field,
	struct bt_ctf_field_type *expected_type, const char *name)
{
	struct bt_ctf_field_type *copy_type;

	copy_type = bt_ctf_field_get_type(field);
	ok(copy_type == expected_type,
		"bt_ctf_field_copy does not copy the type (%s)", name);
	bt_put(copy_type);
}

static void field_copy_tests_validate_diff_ptrs(struct bt_ctf_field *field_a,
	struct bt_ctf_field *field_b, const char *name)
{
	ok(field_a != field_b,
		"bt_ctf_field_copy creates different pointers (%s)", name);
}

void field_copy_tests()
{
	struct bt_ctf_field_type *len_type = NULL;
	struct bt_ctf_field_type *fp_type = NULL;
	struct bt_ctf_field_type *s_type = NULL;
	struct bt_ctf_field_type *e_int_type = NULL;
	struct bt_ctf_field_type *e_type = NULL;
	struct bt_ctf_field_type *v_type = NULL;
	struct bt_ctf_field_type *v_label1_type = NULL;
	struct bt_ctf_field_type *v_label1_array_type = NULL;
	struct bt_ctf_field_type *v_label2_type = NULL;
	struct bt_ctf_field_type *v_label2_seq_type = NULL;
	struct bt_ctf_field_type *strct_type = NULL;
	struct bt_ctf_field *len = NULL;
	struct bt_ctf_field *fp = NULL;
	struct bt_ctf_field *s = NULL;
	struct bt_ctf_field *e_int = NULL;
	struct bt_ctf_field *e = NULL;
	struct bt_ctf_field *v = NULL;
	struct bt_ctf_field *v_selected = NULL;
	struct bt_ctf_field *v_selected_cur = NULL;
	struct bt_ctf_field *v_selected_0 = NULL;
	struct bt_ctf_field *v_selected_1 = NULL;
	struct bt_ctf_field *v_selected_2 = NULL;
	struct bt_ctf_field *v_selected_3 = NULL;
	struct bt_ctf_field *v_selected_4 = NULL;
	struct bt_ctf_field *v_selected_5 = NULL;
	struct bt_ctf_field *v_selected_6 = NULL;
	struct bt_ctf_field *a = NULL;
	struct bt_ctf_field *a_0 = NULL;
	struct bt_ctf_field *a_1 = NULL;
	struct bt_ctf_field *a_2 = NULL;
	struct bt_ctf_field *a_3 = NULL;
	struct bt_ctf_field *a_4 = NULL;
	struct bt_ctf_field *strct = NULL;
	struct bt_ctf_field *len_copy = NULL;
	struct bt_ctf_field *fp_copy = NULL;
	struct bt_ctf_field *s_copy = NULL;
	struct bt_ctf_field *e_int_copy = NULL;
	struct bt_ctf_field *e_copy = NULL;
	struct bt_ctf_field *v_copy = NULL;
	struct bt_ctf_field *v_selected_copy = NULL;
	struct bt_ctf_field *v_selected_copy_len = NULL;
	struct bt_ctf_field *v_selected_0_copy = NULL;
	struct bt_ctf_field *v_selected_1_copy = NULL;
	struct bt_ctf_field *v_selected_2_copy = NULL;
	struct bt_ctf_field *v_selected_3_copy = NULL;
	struct bt_ctf_field *v_selected_4_copy = NULL;
	struct bt_ctf_field *v_selected_5_copy = NULL;
	struct bt_ctf_field *v_selected_6_copy = NULL;
	struct bt_ctf_field *a_copy = NULL;
	struct bt_ctf_field *a_0_copy = NULL;
	struct bt_ctf_field *a_1_copy = NULL;
	struct bt_ctf_field *a_2_copy = NULL;
	struct bt_ctf_field *a_3_copy = NULL;
	struct bt_ctf_field *a_4_copy = NULL;
	struct bt_ctf_field *strct_copy = NULL;
	uint64_t uint64_t_val;
	const char *str_val;
	double double_val;
	int ret;

	/* create len type */
	len_type = bt_ctf_field_type_integer_create(32);
	assert(len_type);

	/* create fp type */
	fp_type = bt_ctf_field_type_floating_point_create();
	assert(fp_type);

	/* create s type */
	s_type = bt_ctf_field_type_string_create();
	assert(s_type);

	/* create e_int type */
	e_int_type = bt_ctf_field_type_integer_create(8);
	assert(e_int_type);

	/* create e type */
	e_type = bt_ctf_field_type_enumeration_create(e_int_type);
	assert(e_type);
	ret = bt_ctf_field_type_enumeration_add_mapping(e_type, "LABEL1",
		10, 15);
	assert(!ret);
	ret = bt_ctf_field_type_enumeration_add_mapping(e_type, "LABEL2",
		23, 23);
	assert(!ret);

	/* create v_label1 type */
	v_label1_type = bt_ctf_field_type_string_create();
	assert(v_label1_type);

	/* create v_label1_array type */
	v_label1_array_type = bt_ctf_field_type_array_create(v_label1_type, 5);
	assert(v_label1_array_type);

	/* create v_label2 type */
	v_label2_type = bt_ctf_field_type_integer_create(16);
	assert(v_label2_type);

	/* create v_label2_seq type */
	v_label2_seq_type = bt_ctf_field_type_sequence_create(v_label2_type,
		"len");
	assert(v_label2_seq_type);

	/* create v type */
	v_type = bt_ctf_field_type_variant_create(e_type, "e");
	assert(v_type);
	ret = bt_ctf_field_type_variant_add_field(v_type, v_label1_array_type,
		"LABEL1");
	assert(!ret);
	ret = bt_ctf_field_type_variant_add_field(v_type, v_label2_seq_type,
		"LABEL2");
	assert(!ret);

	/* create strct type */
	strct_type = bt_ctf_field_type_structure_create();
	assert(strct_type);
	ret = bt_ctf_field_type_structure_add_field(strct_type, len_type,
		"len");
	assert(!ret);
	ret = bt_ctf_field_type_structure_add_field(strct_type, fp_type, "fp");
	assert(!ret);
	ret = bt_ctf_field_type_structure_add_field(strct_type, s_type, "s");
	assert(!ret);
	ret = bt_ctf_field_type_structure_add_field(strct_type, e_type, "e");
	assert(!ret);
	ret = bt_ctf_field_type_structure_add_field(strct_type, v_type, "v");
	assert(!ret);
	ret = bt_ctf_field_type_structure_add_field(strct_type,
		v_label1_array_type, "a");
	assert(!ret);

	/* create strct */
	strct = bt_ctf_field_create(strct_type);
	assert(strct);

	/* get len field */
	len = bt_ctf_field_structure_get_field(strct, "len");
	assert(len);

	/* get fp field */
	fp = bt_ctf_field_structure_get_field(strct, "fp");
	assert(fp);

	/* get s field */
	s = bt_ctf_field_structure_get_field(strct, "s");
	assert(s);

	/* get e field */
	e = bt_ctf_field_structure_get_field(strct, "e");
	assert(e);

	/* get e_int (underlying integer) */
	e_int = bt_ctf_field_enumeration_get_container(e);
	assert(e_int);

	/* get v field */
	v = bt_ctf_field_structure_get_field(strct, "v");
	assert(v);

	/* get a field */
	a = bt_ctf_field_structure_get_field(strct, "a");
	assert(a);

	/* set len field */
	ret = bt_ctf_field_unsigned_integer_set_value(len, 7);
	assert(!ret);

	/* set fp field */
	ret = bt_ctf_field_floating_point_set_value(fp, 3.14);
	assert(!ret);

	/* set s field */
	ret = bt_ctf_field_string_set_value(s, "btbt");
	assert(!ret);

	/* set e field (LABEL2) */
	ret = bt_ctf_field_unsigned_integer_set_value(e_int, 23);
	assert(!ret);

	/* set v field */
	v_selected = bt_ctf_field_variant_get_field(v, e);
	assert(v_selected);
	ok(!bt_ctf_field_variant_get_current_field(NULL),
		"bt_ctf_field_variant_get_current_field handles NULL correctly");
	v_selected_cur = bt_ctf_field_variant_get_current_field(v);
	ok(v_selected_cur == v_selected,
		"bt_ctf_field_variant_get_current_field returns the current field");
	bt_put(v_selected_cur);

	/* set selected v field */
	ret = bt_ctf_field_sequence_set_length(v_selected, len);
	assert(!ret);
	v_selected_0 = bt_ctf_field_sequence_get_field(v_selected, 0);
	assert(v_selected_0);
	ret = bt_ctf_field_unsigned_integer_set_value(v_selected_0, 7);
	assert(!ret);
	v_selected_1 = bt_ctf_field_sequence_get_field(v_selected, 1);
	assert(v_selected_1);
	ret = bt_ctf_field_unsigned_integer_set_value(v_selected_1, 6);
	assert(!ret);
	v_selected_2 = bt_ctf_field_sequence_get_field(v_selected, 2);
	assert(v_selected_2);
	ret = bt_ctf_field_unsigned_integer_set_value(v_selected_2, 5);
	assert(!ret);
	v_selected_3 = bt_ctf_field_sequence_get_field(v_selected, 3);
	assert(v_selected_3);
	ret = bt_ctf_field_unsigned_integer_set_value(v_selected_3, 4);
	assert(!ret);
	v_selected_4 = bt_ctf_field_sequence_get_field(v_selected, 4);
	assert(v_selected_4);
	ret = bt_ctf_field_unsigned_integer_set_value(v_selected_4, 3);
	assert(!ret);
	v_selected_5 = bt_ctf_field_sequence_get_field(v_selected, 5);
	assert(v_selected_5);
	ret = bt_ctf_field_unsigned_integer_set_value(v_selected_5, 2);
	assert(!ret);
	v_selected_6 = bt_ctf_field_sequence_get_field(v_selected, 6);
	assert(v_selected_6);
	ret = bt_ctf_field_unsigned_integer_set_value(v_selected_6, 1);
	assert(!ret);

	/* set a field */
	a_0 = bt_ctf_field_array_get_field(a, 0);
	assert(a_0);
	ret = bt_ctf_field_string_set_value(a_0, "a_0");
	assert(!ret);
	a_1 = bt_ctf_field_array_get_field(a, 1);
	assert(a_1);
	ret = bt_ctf_field_string_set_value(a_1, "a_1");
	assert(!ret);
	a_2 = bt_ctf_field_array_get_field(a, 2);
	assert(a_2);
	ret = bt_ctf_field_string_set_value(a_2, "a_2");
	assert(!ret);
	a_3 = bt_ctf_field_array_get_field(a, 3);
	assert(a_3);
	ret = bt_ctf_field_string_set_value(a_3, "a_3");
	assert(!ret);
	a_4 = bt_ctf_field_array_get_field(a, 4);
	assert(a_4);
	ret = bt_ctf_field_string_set_value(a_4, "a_4");
	assert(!ret);

	/* create copy of strct */
	ok(!bt_ctf_field_copy(NULL),
		"bt_ctf_field_copy handles NULL correctly");
	strct_copy = bt_ctf_field_copy(strct);
	ok(strct_copy,
		"bt_ctf_field_copy returns a valid pointer");

	/* get all copied fields */
	len_copy = bt_ctf_field_structure_get_field(strct_copy, "len");
	assert(len_copy);
	fp_copy = bt_ctf_field_structure_get_field(strct_copy, "fp");
	assert(fp_copy);
	s_copy = bt_ctf_field_structure_get_field(strct_copy, "s");
	assert(s_copy);
	e_copy = bt_ctf_field_structure_get_field(strct_copy, "e");
	assert(e_copy);
	e_int_copy = bt_ctf_field_enumeration_get_container(e_copy);
	assert(e_int_copy);
	v_copy = bt_ctf_field_structure_get_field(strct_copy, "v");
	assert(v_copy);
	v_selected_copy = bt_ctf_field_variant_get_field(v_copy, e_copy);
	assert(v_selected_copy);
	v_selected_0_copy = bt_ctf_field_sequence_get_field(v_selected_copy, 0);
	assert(v_selected_0_copy);
	v_selected_1_copy = bt_ctf_field_sequence_get_field(v_selected_copy, 1);
	assert(v_selected_1_copy);
	v_selected_2_copy = bt_ctf_field_sequence_get_field(v_selected_copy, 2);
	assert(v_selected_2_copy);
	v_selected_3_copy = bt_ctf_field_sequence_get_field(v_selected_copy, 3);
	assert(v_selected_3_copy);
	v_selected_4_copy = bt_ctf_field_sequence_get_field(v_selected_copy, 4);
	assert(v_selected_4_copy);
	v_selected_5_copy = bt_ctf_field_sequence_get_field(v_selected_copy, 5);
	assert(v_selected_5_copy);
	v_selected_6_copy = bt_ctf_field_sequence_get_field(v_selected_copy, 6);
	assert(v_selected_6_copy);
	ok(!bt_ctf_field_sequence_get_field(v_selected_copy, 7),
		"sequence field copy is not too large");
	a_copy = bt_ctf_field_structure_get_field(strct_copy, "a");
	assert(a_copy);
	a_0_copy = bt_ctf_field_array_get_field(a_copy, 0);
	assert(a_0_copy);
	a_1_copy = bt_ctf_field_array_get_field(a_copy, 1);
	assert(a_1_copy);
	a_2_copy = bt_ctf_field_array_get_field(a_copy, 2);
	assert(a_2_copy);
	a_3_copy = bt_ctf_field_array_get_field(a_copy, 3);
	assert(a_3_copy);
	a_4_copy = bt_ctf_field_array_get_field(a_copy, 4);
	assert(a_4_copy);
	ok(!bt_ctf_field_array_get_field(v_selected_copy, 5),
		"array field copy is not too large");

	/* make sure copied fields are different pointers */
	field_copy_tests_validate_diff_ptrs(strct_copy, strct, "strct");
	field_copy_tests_validate_diff_ptrs(len_copy, len, "len");
	field_copy_tests_validate_diff_ptrs(fp_copy, fp, "fp");
	field_copy_tests_validate_diff_ptrs(s_copy, s, "s");
	field_copy_tests_validate_diff_ptrs(e_int_copy, e_int, "e_int");
	field_copy_tests_validate_diff_ptrs(e_copy, e, "e");
	field_copy_tests_validate_diff_ptrs(v_copy, v, "v");
	field_copy_tests_validate_diff_ptrs(v_selected_copy, v_selected,
		"v_selected");
	field_copy_tests_validate_diff_ptrs(v_selected_0_copy, v_selected_0,
		"v_selected_0");
	field_copy_tests_validate_diff_ptrs(v_selected_1_copy, v_selected_1,
		"v_selected_1");
	field_copy_tests_validate_diff_ptrs(v_selected_2_copy, v_selected_2,
		"v_selected_2");
	field_copy_tests_validate_diff_ptrs(v_selected_3_copy, v_selected_3,
		"v_selected_3");
	field_copy_tests_validate_diff_ptrs(v_selected_4_copy, v_selected_4,
		"v_selected_4");
	field_copy_tests_validate_diff_ptrs(v_selected_5_copy, v_selected_5,
		"v_selected_5");
	field_copy_tests_validate_diff_ptrs(v_selected_6_copy, v_selected_6,
		"v_selected_6");
	field_copy_tests_validate_diff_ptrs(a_copy, a, "a");
	field_copy_tests_validate_diff_ptrs(a_0_copy, a_0, "a_0");
	field_copy_tests_validate_diff_ptrs(a_1_copy, a_1, "a_1");
	field_copy_tests_validate_diff_ptrs(a_2_copy, a_2, "a_2");
	field_copy_tests_validate_diff_ptrs(a_3_copy, a_3, "a_3");
	field_copy_tests_validate_diff_ptrs(a_4_copy, a_4, "a_4");

	/* make sure copied fields share the same types */
	field_copy_tests_validate_same_type(strct_copy, strct_type, "strct");
	field_copy_tests_validate_same_type(len_copy, len_type, "len");
	field_copy_tests_validate_same_type(fp_copy, fp_type, "fp");
	field_copy_tests_validate_same_type(e_int_copy, e_int_type, "e_int");
	field_copy_tests_validate_same_type(e_copy, e_type, "e");
	field_copy_tests_validate_same_type(v_copy, v_type, "v");
	field_copy_tests_validate_same_type(v_selected_copy, v_label2_seq_type,
		"v_selected");
	field_copy_tests_validate_same_type(v_selected_0_copy, v_label2_type,
		"v_selected_0");
	field_copy_tests_validate_same_type(v_selected_1_copy, v_label2_type,
		"v_selected_1");
	field_copy_tests_validate_same_type(v_selected_2_copy, v_label2_type,
		"v_selected_2");
	field_copy_tests_validate_same_type(v_selected_3_copy, v_label2_type,
		"v_selected_3");
	field_copy_tests_validate_same_type(v_selected_4_copy, v_label2_type,
		"v_selected_4");
	field_copy_tests_validate_same_type(v_selected_5_copy, v_label2_type,
		"v_selected_5");
	field_copy_tests_validate_same_type(v_selected_6_copy, v_label2_type,
		"v_selected_6");
	field_copy_tests_validate_same_type(a_copy, v_label1_array_type, "a");
	field_copy_tests_validate_same_type(a_0_copy, v_label1_type, "a_0");
	field_copy_tests_validate_same_type(a_1_copy, v_label1_type, "a_1");
	field_copy_tests_validate_same_type(a_2_copy, v_label1_type, "a_2");
	field_copy_tests_validate_same_type(a_3_copy, v_label1_type, "a_3");
	field_copy_tests_validate_same_type(a_4_copy, v_label1_type, "a_4");

	/* validate len copy */
	ret = bt_ctf_field_unsigned_integer_get_value(len_copy, &uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 7,
		"bt_ctf_field_copy creates a valid integer field copy");

	/* validate fp copy */
	ret = bt_ctf_field_floating_point_get_value(fp_copy, &double_val);
	assert(!ret);
	ok(double_val == 3.14,
		"bt_ctf_field_copy creates a valid floating point number field copy");

	/* validate s copy */
	str_val = bt_ctf_field_string_get_value(s_copy);
	ok(str_val && !strcmp(str_val, "btbt"),
		"bt_ctf_field_copy creates a valid string field copy");

	/* validate e_int copy */
	ret = bt_ctf_field_unsigned_integer_get_value(e_int_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 23,
		"bt_ctf_field_copy creates a valid enum's integer field copy");

	/* validate e copy */
	str_val = bt_ctf_field_enumeration_get_mapping_name(e_copy);
	ok(str_val && !strcmp(str_val, "LABEL2"),
		"bt_ctf_field_copy creates a valid enum field copy");

	/* validate v_selected copy */
	v_selected_copy_len = bt_ctf_field_sequence_get_length(v_selected);
	assert(v_selected_copy_len);
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_copy_len,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 7,
		"bt_ctf_field_copy creates a sequence field copy with the proper length");
	bt_put(v_selected_copy_len);
	v_selected_copy_len = NULL;

	/* validate v_selected copy fields */
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_0_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 7,
		"bt_ctf_field_copy creates a valid sequence field element copy (v_selected_0)");
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_1_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 6,
		"bt_ctf_field_copy creates a valid sequence field element copy (v_selected_1)");
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_2_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 5,
		"bt_ctf_field_copy creates a valid sequence field element copy (v_selected_2)");
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_3_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 4,
		"bt_ctf_field_copy creates a valid sequence field element copy (v_selected_3)");
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_4_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 3,
		"bt_ctf_field_copy creates a valid sequence field element copy (v_selected_4)");
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_5_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 2,
		"bt_ctf_field_copy creates a valid sequence field element copy (v_selected_5)");
	ret = bt_ctf_field_unsigned_integer_get_value(v_selected_6_copy,
		&uint64_t_val);
	assert(!ret);
	ok(uint64_t_val == 1,
		"bt_ctf_field_copy creates a valid sequence field element copy (v_selected_6)");

	/* validate a copy fields */
	str_val = bt_ctf_field_string_get_value(a_0_copy);
	ok(str_val && !strcmp(str_val, "a_0"),
		"bt_ctf_field_copy creates a valid array field element copy (a_0)");
	str_val = bt_ctf_field_string_get_value(a_1_copy);
	ok(str_val && !strcmp(str_val, "a_1"),
		"bt_ctf_field_copy creates a valid array field element copy (a_1)");
	str_val = bt_ctf_field_string_get_value(a_2_copy);
	ok(str_val && !strcmp(str_val, "a_2"),
		"bt_ctf_field_copy creates a valid array field element copy (a_2)");
	str_val = bt_ctf_field_string_get_value(a_3_copy);
	ok(str_val && !strcmp(str_val, "a_3"),
		"bt_ctf_field_copy creates a valid array field element copy (a_3)");
	str_val = bt_ctf_field_string_get_value(a_4_copy);
	ok(str_val && !strcmp(str_val, "a_4"),
		"bt_ctf_field_copy creates a valid array field element copy (a_4)");

	/* put everything */
	bt_put(len_type);
	bt_put(fp_type);
	bt_put(s_type);
	bt_put(e_int_type);
	bt_put(e_type);
	bt_put(v_type);
	bt_put(v_label1_type);
	bt_put(v_label1_array_type);
	bt_put(v_label2_type);
	bt_put(v_label2_seq_type);
	bt_put(strct_type);
	bt_put(len);
	bt_put(fp);
	bt_put(s);
	bt_put(e_int);
	bt_put(e);
	bt_put(v);
	bt_put(v_selected);
	bt_put(v_selected_0);
	bt_put(v_selected_1);
	bt_put(v_selected_2);
	bt_put(v_selected_3);
	bt_put(v_selected_4);
	bt_put(v_selected_5);
	bt_put(v_selected_6);
	bt_put(a);
	bt_put(a_0);
	bt_put(a_1);
	bt_put(a_2);
	bt_put(a_3);
	bt_put(a_4);
	bt_put(strct);
	bt_put(len_copy);
	bt_put(fp_copy);
	bt_put(s_copy);
	bt_put(e_int_copy);
	bt_put(e_copy);
	bt_put(v_copy);
	bt_put(v_selected_copy);
	bt_put(v_selected_0_copy);
	bt_put(v_selected_1_copy);
	bt_put(v_selected_2_copy);
	bt_put(v_selected_3_copy);
	bt_put(v_selected_4_copy);
	bt_put(v_selected_5_copy);
	bt_put(v_selected_6_copy);
	bt_put(a_copy);
	bt_put(a_0_copy);
	bt_put(a_1_copy);
	bt_put(a_2_copy);
	bt_put(a_3_copy);
	bt_put(a_4_copy);
	bt_put(strct_copy);
}

void type_field_tests()
{
	struct bt_ctf_field *uint_12;
	struct bt_ctf_field *int_16;
	struct bt_ctf_field *string;
	struct bt_ctf_field *enumeration;
	struct bt_ctf_field_type *composite_structure_type;
	struct bt_ctf_field_type *structure_seq_type;
	struct bt_ctf_field_type *string_type;
	struct bt_ctf_field_type *sequence_type;
	struct bt_ctf_field_type *uint_8_type;
	struct bt_ctf_field_type *int_16_type;
	struct bt_ctf_field_type *uint_12_type =
		bt_ctf_field_type_integer_create(12);
	struct bt_ctf_field_type *enumeration_type;
	struct bt_ctf_field_type *enumeration_sequence_type;
	struct bt_ctf_field_type *enumeration_array_type;
	struct bt_ctf_field_type *returned_type;
	const char *ret_string;

	returned_type = bt_ctf_field_get_type(NULL);
	ok(!returned_type, "bt_ctf_field_get_type handles NULL correctly");

	ok(uint_12_type, "Create an unsigned integer type");
	ok(bt_ctf_field_type_integer_set_base(uint_12_type,
		BT_CTF_INTEGER_BASE_BINARY) == 0,
		"Set integer type's base as binary");
	ok(bt_ctf_field_type_integer_set_base(uint_12_type,
		BT_CTF_INTEGER_BASE_DECIMAL) == 0,
		"Set integer type's base as decimal");
	ok(bt_ctf_field_type_integer_set_base(uint_12_type,
		BT_CTF_INTEGER_BASE_UNKNOWN),
		"Reject integer type's base set as unknown");
	ok(bt_ctf_field_type_integer_set_base(uint_12_type,
		BT_CTF_INTEGER_BASE_OCTAL) == 0,
		"Set integer type's base as octal");
	ok(bt_ctf_field_type_integer_set_base(uint_12_type,
		BT_CTF_INTEGER_BASE_HEXADECIMAL) == 0,
		"Set integer type's base as hexadecimal");
	ok(bt_ctf_field_type_integer_set_base(uint_12_type, 457417),
		"Reject unknown integer base value");
	ok(bt_ctf_field_type_integer_set_signed(uint_12_type, 952835) == 0,
		"Set integer type signedness to signed");
	ok(bt_ctf_field_type_integer_set_signed(uint_12_type, 0) == 0,
		"Set integer type signedness to unsigned");
	ok(bt_ctf_field_type_integer_get_size(NULL) < 0,
		"bt_ctf_field_type_integer_get_size handles NULL correctly");
	ok(bt_ctf_field_type_integer_get_size(uint_12_type) == 12,
		"bt_ctf_field_type_integer_get_size returns a correct value");
	ok(bt_ctf_field_type_integer_get_signed(NULL) < 0,
		"bt_ctf_field_type_integer_get_signed handles NULL correctly");
	ok(bt_ctf_field_type_integer_get_signed(uint_12_type) == 0,
		"bt_ctf_field_type_integer_get_signed returns a correct value for unsigned types");

	ok(bt_ctf_field_type_set_byte_order(NULL,
		BT_CTF_BYTE_ORDER_LITTLE_ENDIAN) < 0,
		"bt_ctf_field_type_set_byte_order handles NULL correctly");
	ok(bt_ctf_field_type_set_byte_order(uint_12_type,
		(enum bt_ctf_byte_order) 42) < 0,
		"bt_ctf_field_type_set_byte_order rejects invalid values");
	ok(bt_ctf_field_type_set_byte_order(uint_12_type,
		BT_CTF_BYTE_ORDER_LITTLE_ENDIAN) == 0,
		"Set an integer's byte order to little endian");
	ok(bt_ctf_field_type_set_byte_order(uint_12_type,
		BT_CTF_BYTE_ORDER_BIG_ENDIAN) == 0,
		"Set an integer's byte order to big endian");
	ok(bt_ctf_field_type_get_byte_order(uint_12_type) ==
		BT_CTF_BYTE_ORDER_BIG_ENDIAN,
		"bt_ctf_field_type_get_byte_order returns a correct value");
	ok(bt_ctf_field_type_get_byte_order(NULL) ==
		BT_CTF_BYTE_ORDER_UNKNOWN,
		"bt_ctf_field_type_get_byte_order handles NULL correctly");

	ok(bt_ctf_field_type_get_type_id(NULL) ==
		CTF_TYPE_UNKNOWN,
		"bt_ctf_field_type_get_type_id handles NULL correctly");
	ok(bt_ctf_field_type_get_type_id(uint_12_type) ==
		CTF_TYPE_INTEGER,
		"bt_ctf_field_type_get_type_id returns a correct value with an integer type");

	ok(bt_ctf_field_type_integer_get_base(NULL) ==
		BT_CTF_INTEGER_BASE_UNKNOWN,
		"bt_ctf_field_type_integer_get_base handles NULL correctly");
	ok(bt_ctf_field_type_integer_get_base(uint_12_type) ==
		BT_CTF_INTEGER_BASE_HEXADECIMAL,
		"bt_ctf_field_type_integer_get_base returns a correct value");

	ok(bt_ctf_field_type_integer_set_encoding(NULL, CTF_STRING_ASCII) < 0,
		"bt_ctf_field_type_integer_set_encoding handles NULL correctly");
	ok(bt_ctf_field_type_integer_set_encoding(uint_12_type,
		(enum ctf_string_encoding) 123) < 0,
		"bt_ctf_field_type_integer_set_encoding handles invalid encodings correctly");
	ok(bt_ctf_field_type_integer_set_encoding(uint_12_type,
		CTF_STRING_UTF8) == 0,
		"Set integer type encoding to UTF8");
	ok(bt_ctf_field_type_integer_get_encoding(NULL) == CTF_STRING_UNKNOWN,
		"bt_ctf_field_type_integer_get_encoding handles NULL correctly");
	ok(bt_ctf_field_type_integer_get_encoding(uint_12_type) == CTF_STRING_UTF8,
		"bt_ctf_field_type_integer_get_encoding returns a correct value");

	int_16_type = bt_ctf_field_type_integer_create(16);
	bt_ctf_field_type_integer_set_signed(int_16_type, 1);
	ok(bt_ctf_field_type_integer_get_signed(int_16_type) == 1,
		"bt_ctf_field_type_integer_get_signed returns a correct value for signed types");
	uint_8_type = bt_ctf_field_type_integer_create(8);
	sequence_type =
		bt_ctf_field_type_sequence_create(int_16_type, "seq_len");
	ok(sequence_type, "Create a sequence of int16_t type");
	ok(bt_ctf_field_type_get_type_id(sequence_type) ==
		CTF_TYPE_SEQUENCE,
		"bt_ctf_field_type_get_type_id returns a correct value with a sequence type");

	ok(bt_ctf_field_type_sequence_get_length_field_name(NULL) == NULL,
		"bt_ctf_field_type_sequence_get_length_field_name handles NULL correctly");
	ret_string = bt_ctf_field_type_sequence_get_length_field_name(
		sequence_type);
	ok(!strcmp(ret_string, "seq_len"),
		"bt_ctf_field_type_sequence_get_length_field_name returns the correct value");
	ok(bt_ctf_field_type_sequence_get_element_type(NULL) == NULL,
		"bt_ctf_field_type_sequence_get_element_type handles NULL correctly");
	returned_type = bt_ctf_field_type_sequence_get_element_type(
		sequence_type);
	ok(returned_type == int_16_type,
		"bt_ctf_field_type_sequence_get_element_type returns the correct type");
	bt_put(returned_type);

	string_type = bt_ctf_field_type_string_create();
	ok(string_type, "Create a string type");
	ok(bt_ctf_field_type_string_set_encoding(string_type,
		CTF_STRING_NONE),
		"Reject invalid \"None\" string encoding");
	ok(bt_ctf_field_type_string_set_encoding(string_type,
		42),
		"Reject invalid string encoding");
	ok(bt_ctf_field_type_string_set_encoding(string_type,
		CTF_STRING_ASCII) == 0,
		"Set string encoding to ASCII");

	ok(bt_ctf_field_type_string_get_encoding(NULL) ==
		CTF_STRING_UNKNOWN,
		"bt_ctf_field_type_string_get_encoding handles NULL correctly");
	ok(bt_ctf_field_type_string_get_encoding(string_type) ==
		CTF_STRING_ASCII,
		"bt_ctf_field_type_string_get_encoding returns the correct value");

	structure_seq_type = bt_ctf_field_type_structure_create();
	ok(bt_ctf_field_type_get_type_id(structure_seq_type) ==
		CTF_TYPE_STRUCT,
		"bt_ctf_field_type_get_type_id returns a correct value with a structure type");
	ok(structure_seq_type, "Create a structure type");
	ok(bt_ctf_field_type_structure_add_field(structure_seq_type,
		uint_8_type, "seq_len") == 0,
		"Add a uint8_t type to a structure");
	ok(bt_ctf_field_type_structure_add_field(structure_seq_type,
		sequence_type, "a_sequence") == 0,
		"Add a sequence type to a structure");

	ok(bt_ctf_field_type_structure_get_field_count(NULL) < 0,
		"bt_ctf_field_type_structure_get_field_count handles NULL correctly");
	ok(bt_ctf_field_type_structure_get_field_count(structure_seq_type) == 2,
		"bt_ctf_field_type_structure_get_field_count returns a correct value");

	ok(bt_ctf_field_type_structure_get_field(NULL,
		&ret_string, &returned_type, 1) < 0,
		"bt_ctf_field_type_structure_get_field handles a NULL type correctly");
	ok(bt_ctf_field_type_structure_get_field(structure_seq_type,
		NULL, &returned_type, 1) == 0,
		"bt_ctf_field_type_structure_get_field handles a NULL name correctly");
	bt_put(returned_type);
	ok(bt_ctf_field_type_structure_get_field(structure_seq_type,
		&ret_string, NULL, 1) == 0,
		"bt_ctf_field_type_structure_get_field handles a NULL return type correctly");
	ok(bt_ctf_field_type_structure_get_field(structure_seq_type,
		&ret_string, &returned_type, 10) < 0,
		"bt_ctf_field_type_structure_get_field handles an invalid index correctly");
	ok(bt_ctf_field_type_structure_get_field(structure_seq_type,
		&ret_string, &returned_type, 1) == 0,
		"bt_ctf_field_type_structure_get_field returns a field");
	ok(!strcmp(ret_string, "a_sequence"),
		"bt_ctf_field_type_structure_get_field returns a correct field name");
	ok(returned_type == sequence_type,
		"bt_ctf_field_type_structure_get_field returns a correct field type");
	bt_put(returned_type);

	ok(bt_ctf_field_type_structure_get_field_type_by_name(NULL, "a_sequence") == NULL,
		"bt_ctf_field_type_structure_get_field_type_by_name handles a NULL structure correctly");
	ok(bt_ctf_field_type_structure_get_field_type_by_name(structure_seq_type, NULL) == NULL,
		"bt_ctf_field_type_structure_get_field_type_by_name handles a NULL field name correctly");
	returned_type = bt_ctf_field_type_structure_get_field_type_by_name(
		structure_seq_type, "a_sequence");
	ok(returned_type == sequence_type,
		"bt_ctf_field_type_structure_get_field_type_by_name returns the correct field type");
	bt_put(returned_type);

	composite_structure_type = bt_ctf_field_type_structure_create();
	ok(bt_ctf_field_type_structure_add_field(composite_structure_type,
		string_type, "a_string") == 0,
		"Add a string type to a structure");
	ok(bt_ctf_field_type_structure_add_field(composite_structure_type,
		structure_seq_type, "inner_structure") == 0,
		"Add a structure type to a structure");

	ok(bt_ctf_field_type_structure_get_field_type_by_name(
		NULL, "a_sequence") == NULL,
		"bt_ctf_field_type_structure_get_field_type_by_name handles a NULL field correctly");
	ok(bt_ctf_field_type_structure_get_field_type_by_name(
		structure_seq_type, NULL) == NULL,
		"bt_ctf_field_type_structure_get_field_type_by_name handles a NULL field name correctly");
	returned_type = bt_ctf_field_type_structure_get_field_type_by_name(
		structure_seq_type, "a_sequence");
	ok(returned_type == sequence_type,
		"bt_ctf_field_type_structure_get_field_type_by_name returns a correct type");
	bt_put(returned_type);

	int_16 = bt_ctf_field_create(int_16_type);
	ok(int_16, "Instanciate a signed 16-bit integer");
	uint_12 = bt_ctf_field_create(uint_12_type);
	ok(uint_12, "Instanciate an unsigned 12-bit integer");
	returned_type = bt_ctf_field_get_type(int_16);
	ok(returned_type == int_16_type,
		"bt_ctf_field_get_type returns the correct type");

	/* Can't modify types after instanciating them */
	ok(bt_ctf_field_type_integer_set_base(uint_12_type,
		BT_CTF_INTEGER_BASE_DECIMAL),
		"Check an integer type' base can't be modified after instanciation");
	ok(bt_ctf_field_type_integer_set_signed(uint_12_type, 0),
		"Check an integer type's signedness can't be modified after instanciation");

	/* Check signed property is checked */
	ok(bt_ctf_field_signed_integer_set_value(uint_12, -52),
		"Check bt_ctf_field_signed_integer_set_value is not allowed on an unsigned integer");
	ok(bt_ctf_field_unsigned_integer_set_value(int_16, 42),
		"Check bt_ctf_field_unsigned_integer_set_value is not allowed on a signed integer");

	/* Check overflows are properly tested for */
	ok(bt_ctf_field_signed_integer_set_value(int_16, -32768) == 0,
		"Check -32768 is allowed for a signed 16-bit integer");
	ok(bt_ctf_field_signed_integer_set_value(int_16, 32767) == 0,
		"Check 32767 is allowed for a signed 16-bit integer");
	ok(bt_ctf_field_signed_integer_set_value(int_16, 32768),
		"Check 32768 is not allowed for a signed 16-bit integer");
	ok(bt_ctf_field_signed_integer_set_value(int_16, -32769),
		"Check -32769 is not allowed for a signed 16-bit integer");
	ok(bt_ctf_field_signed_integer_set_value(int_16, -42) == 0,
		"Check -42 is allowed for a signed 16-bit integer");

	ok(bt_ctf_field_unsigned_integer_set_value(uint_12, 4095) == 0,
		"Check 4095 is allowed for an unsigned 12-bit integer");
	ok(bt_ctf_field_unsigned_integer_set_value(uint_12, 4096),
		"Check 4096 is not allowed for a unsigned 12-bit integer");
	ok(bt_ctf_field_unsigned_integer_set_value(uint_12, 0) == 0,
		"Check 0 is allowed for an unsigned 12-bit integer");

	string = bt_ctf_field_create(string_type);
	ok(string, "Instanciate a string field");
	ok(bt_ctf_field_string_set_value(string, "A value") == 0,
		"Set a string's value");

	enumeration_type = bt_ctf_field_type_enumeration_create(uint_12_type);
	ok(enumeration_type,
		"Create an enumeration type with an unsigned 12-bit integer as container");
	enumeration_sequence_type = bt_ctf_field_type_sequence_create(
		enumeration_type, "count");
	ok(!enumeration_sequence_type,
		"Check enumeration types are validated when creating a sequence");
	enumeration_array_type = bt_ctf_field_type_array_create(
		enumeration_type, 10);
	ok(!enumeration_array_type,
		"Check enumeration types are validated when creating an array");
	ok(bt_ctf_field_type_structure_add_field(composite_structure_type,
		enumeration_type, "enumeration"),
		"Check enumeration types are validated when adding them as structure members");
	enumeration = bt_ctf_field_create(enumeration_type);
	ok(!enumeration,
		"Check enumeration types are validated before instantiation");

	bt_put(string);
	bt_put(uint_12);
	bt_put(int_16);
	bt_put(enumeration);
	bt_put(composite_structure_type);
	bt_put(structure_seq_type);
	bt_put(string_type);
	bt_put(sequence_type);
	bt_put(uint_8_type);
	bt_put(int_16_type);
	bt_put(uint_12_type);
	bt_put(enumeration_type);
	bt_put(enumeration_sequence_type);
	bt_put(enumeration_array_type);
	bt_put(returned_type);
}

void packet_resize_test(struct bt_ctf_stream_class *stream_class,
		struct bt_ctf_stream *stream, struct bt_ctf_clock *clock)
{
	/*
	 * Append enough events to force the underlying packet to be resized.
	 * Also tests that a new event can be declared after a stream has been
	 * instantiated and used/flushed.
	 */
	int ret = 0;
	int i;
	struct bt_ctf_event_class *event_class = bt_ctf_event_class_create(
		"Spammy_Event");
	struct bt_ctf_field_type *integer_type =
		bt_ctf_field_type_integer_create(17);
	struct bt_ctf_field_type *string_type =
		bt_ctf_field_type_string_create();
	struct bt_ctf_event *event = NULL;
	struct bt_ctf_field *ret_field = NULL;
	struct bt_ctf_field_type *ret_field_type = NULL;
	uint64_t ret_uint64;
	int events_appended = 0;
	struct bt_ctf_field *packet_context = NULL,
		*packet_context_field = NULL, *event_context = NULL;

	ret |= bt_ctf_event_class_add_field(event_class, integer_type,
		"field_1");
	ret |= bt_ctf_event_class_add_field(event_class, string_type,
		"a_string");
	ret |= bt_ctf_stream_class_add_event_class(stream_class, event_class);
	ok(ret == 0, "Add a new event class to a stream class after writing an event");
	if (ret) {
		goto end;
	}

	event = bt_ctf_event_create(event_class);
	ret_field = bt_ctf_event_get_payload_by_index(event, 0);
	ret_field_type = bt_ctf_field_get_type(ret_field);
	ok(ret_field_type == integer_type,
		"bt_ctf_event_get_payload_by_index returns a correct field");
	bt_put(ret_field_type);
	bt_put(ret_field);

	ok(bt_ctf_event_get_payload_by_index(NULL, 0) == NULL,
		"bt_ctf_event_get_payload_by_index handles NULL correctly");
	ok(bt_ctf_event_get_payload_by_index(event, 4) == NULL,
		"bt_ctf_event_get_payload_by_index handles an invalid index correctly");
	bt_put(event);

	ok(bt_ctf_stream_get_event_context(NULL) == NULL,
		"bt_ctf_stream_get_event_context handles NULL correctly");
	event_context = bt_ctf_stream_get_event_context(stream);
	ok(event_context,
		"bt_ctf_stream_get_event_context returns a stream event context");
	ok(bt_ctf_stream_set_event_context(NULL, event_context) < 0,
		"bt_ctf_stream_set_event_context handles a NULL stream correctly");
	ok(bt_ctf_stream_set_event_context(stream, NULL) < 0,
		"bt_ctf_stream_set_event_context handles a NULL stream event context correctly");
	ok(!bt_ctf_stream_set_event_context(stream, event_context),
		"bt_ctf_stream_set_event_context correctly set a stream event context");
	ret_field = bt_ctf_field_create(integer_type);
	ok(bt_ctf_stream_set_event_context(stream, ret_field) < 0,
		"bt_ctf_stream_set_event_context rejects an event context of incorrect type");
	bt_put(ret_field);

	for (i = 0; i < PACKET_RESIZE_TEST_LENGTH; i++) {
		event = bt_ctf_event_create(event_class);
		struct bt_ctf_field *integer =
			bt_ctf_field_create(integer_type);
		struct bt_ctf_field *string =
			bt_ctf_field_create(string_type);

		ret |= bt_ctf_clock_set_time(clock, ++current_time);
		ret |= bt_ctf_field_unsigned_integer_set_value(integer, i);
		ret |= bt_ctf_event_set_payload(event, "field_1",
			integer);
		bt_put(integer);
		ret |= bt_ctf_field_string_set_value(string, "This is a test");
		ret |= bt_ctf_event_set_payload(event, "a_string",
			string);
		bt_put(string);

		/* Populate stream event context */
		integer = bt_ctf_field_structure_get_field(event_context,
			"common_event_context");
		ret |= bt_ctf_field_unsigned_integer_set_value(integer,
			i % 42);
		bt_put(integer);

		ret |= bt_ctf_stream_append_event(stream, event);
		bt_put(event);

		if (ret) {
			break;
		}
	}

	events_appended = !!(i == PACKET_RESIZE_TEST_LENGTH);
	ok(bt_ctf_stream_get_discarded_events_count(NULL, &ret_uint64) < 0,
		"bt_ctf_stream_get_discarded_events_count handles a NULL stream correctly");
	ok(bt_ctf_stream_get_discarded_events_count(stream, NULL) < 0,
		"bt_ctf_stream_get_discarded_events_count handles a NULL return pointer correctly");
	ret = bt_ctf_stream_get_discarded_events_count(stream, &ret_uint64);
	ok(ret == 0 && ret_uint64 == 0,
		"bt_ctf_stream_get_discarded_events_count returns a correct number of discarded events when none were discarded");
	bt_ctf_stream_append_discarded_events(stream, 1000);
	ret = bt_ctf_stream_get_discarded_events_count(stream, &ret_uint64);
	ok(ret == 0 && ret_uint64 == 1000,
		"bt_ctf_stream_get_discarded_events_count returns a correct number of discarded events when some were discarded");

end:
	ok(events_appended, "Append 100 000 events to a stream");

	/*
	 * Populate the custom packet context field with a dummy value
	 * otherwise flush will fail.
	 */
	packet_context = bt_ctf_stream_get_packet_context(stream);
	packet_context_field = bt_ctf_field_structure_get_field(packet_context,
		"custom_packet_context_field");
	bt_ctf_field_unsigned_integer_set_value(packet_context_field, 2);

	ok(bt_ctf_stream_flush(stream) == 0,
		"Flush a stream that forces a packet resize");
	ret = bt_ctf_stream_get_discarded_events_count(stream, &ret_uint64);
	ok(ret == 0 && ret_uint64 == 1000,
		"bt_ctf_stream_get_discarded_events_count returns a correct number of discarded events after a flush");
	bt_put(integer_type);
	bt_put(string_type);
	bt_put(packet_context);
	bt_put(packet_context_field);
	bt_put(event_context);
	bt_put(event_class);
}

void test_empty_stream(struct bt_ctf_writer *writer)
{
	int ret = 0;
	struct bt_ctf_trace *trace = NULL, *ret_trace = NULL;
	struct bt_ctf_stream_class *stream_class = NULL;
	struct bt_ctf_stream *stream = NULL;

	trace = bt_ctf_writer_get_trace(writer);
	if (!trace) {
		diag("Failed to get trace from writer");
		ret = -1;
		goto end;
	}

	stream_class = bt_ctf_stream_class_create("empty_stream");
	if (!stream_class) {
		diag("Failed to create stream class");
		ret = -1;
		goto end;
	}

	ok(bt_ctf_stream_class_get_trace(NULL) == NULL,
		"bt_ctf_stream_class_get_trace handles NULL correctly");
	ok(bt_ctf_stream_class_get_trace(stream_class) == NULL,
		"bt_ctf_stream_class_get_trace returns NULL when stream class is orphaned");

	stream = bt_ctf_writer_create_stream(writer, stream_class);
	if (!stream) {
		diag("Failed to create writer stream");
		ret = -1;
		goto end;
	}

	ret_trace = bt_ctf_stream_class_get_trace(stream_class);
	ok(ret_trace == trace,
		"bt_ctf_stream_class_get_trace returns the correct trace after a stream has been created");
end:
	ok(ret == 0,
		"Created a stream class with default attributes and an empty stream");
	bt_put(trace);
	bt_put(ret_trace);
	bt_put(stream);
	bt_put(stream_class);
}

void test_custom_event_header_stream(struct bt_ctf_writer *writer)
{
	int i, ret;
	struct bt_ctf_trace *trace = NULL;
	struct bt_ctf_clock *clock = NULL;
	struct bt_ctf_stream_class *stream_class = NULL;
	struct bt_ctf_stream *stream = NULL;
	struct bt_ctf_field_type *integer_type = NULL,
		*sequence_type = NULL, *event_header_type = NULL;
	struct bt_ctf_field *integer = NULL, *sequence = NULL,
		*event_header = NULL, *packet_header = NULL;
	struct bt_ctf_event_class *event_class = NULL;
	struct bt_ctf_event *event = NULL;

	trace = bt_ctf_writer_get_trace(writer);
	if (!trace) {
		fail("Failed to get trace from writer");
		goto end;
	}

	clock = bt_ctf_trace_get_clock(trace, 0);
	if (!clock) {
		fail("Failed to get clock from trace");
		goto end;
	}

	stream_class = bt_ctf_stream_class_create("custom_event_header_stream");
	if (!stream_class) {
		fail("Failed to create stream class");
		goto end;
	}

	ret = bt_ctf_stream_class_set_clock(stream_class, clock);
	if (ret) {
		fail("Failed to set stream class clock");
		goto end;
	}

	/*
	 * Customize event header to add an "seq_len" integer member
	 * which will be used as the length of a sequence in an event of this
	 * stream.
	 */
	event_header_type = bt_ctf_stream_class_get_event_header_type(
		stream_class);
	if (!event_header_type) {
		fail("Failed to get event header type");
		goto end;
	}

	integer_type = bt_ctf_field_type_integer_create(13);
	if (!integer_type) {
		fail("Failed to create length integer type");
		goto end;
	}

	ret = bt_ctf_field_type_structure_add_field(event_header_type,
		integer_type, "seq_len");
	if (ret) {
		fail("Failed to add a new field to stream event header");
		goto end;
	}

	event_class = bt_ctf_event_class_create("sequence_event");
	if (!event_class) {
		fail("Failed to create event class");
		goto end;
	}

	/*
	 * This event's payload will contain a sequence which references
	 * stream.event.header.seq_len as its length field.
	 */
	sequence_type = bt_ctf_field_type_sequence_create(integer_type,
		"stream.event.header.seq_len");
	if (!sequence_type) {
		fail("Failed to create a sequence");
		goto end;
	}

	ret = bt_ctf_event_class_add_field(event_class, sequence_type,
		"some_sequence");
	if (ret) {
		fail("Failed to add a sequence to an event class");
		goto end;
	}

	ret = bt_ctf_stream_class_add_event_class(stream_class, event_class);
	if (ret) {
		fail("Failed to add event class to stream class");
		goto end;
	}

	stream = bt_ctf_writer_create_stream(writer, stream_class);
	if (!stream) {
		fail("Failed to create stream")
		goto end;
	}

	/*
	 * We have defined a custom packet header field. We have to populate it
	 * explicitly.
	 */
	packet_header = bt_ctf_stream_get_packet_header(stream);
	if (!packet_header) {
		fail("Failed to get stream packet header");
		goto end;
	}

	integer = bt_ctf_field_structure_get_field(packet_header,
		"custom_trace_packet_header_field");
	if (!integer) {
		fail("Failed to retrieve custom_trace_packet_header_field");
		goto end;
	}

	ret = bt_ctf_field_unsigned_integer_set_value(integer, 3487);
	if (ret) {
		fail("Failed to set custom_trace_packet_header_field value");
		goto end;
	}
	bt_put(integer);

	event = bt_ctf_event_create(event_class);
	if (!event) {
		fail("Failed to create event");
		goto end;
	}

	event_header = bt_ctf_event_get_header(event);
	if (!event_header) {
		fail("Failed to get event header");
		goto end;
	}

	integer = bt_ctf_field_structure_get_field(event_header,
		"seq_len");
	if (!integer) {
		fail("Failed to get seq_len field from event header");
		goto end;
	}

	ret = bt_ctf_field_unsigned_integer_set_value(integer, 2);
	if (ret) {
		fail("Failed to set seq_len value in event header");
		goto end;
	}

	/* Populate both sequence integer fields */
	sequence = bt_ctf_event_get_payload(event, "some_sequence");
	if (!sequence) {
		fail("Failed to retrieve sequence from event");
		goto end;
	}

	ret = bt_ctf_field_sequence_set_length(sequence, integer);
	if (ret) {
		fail("Failed to set sequence length");
		goto end;
	}
	bt_put(integer);

	for (i = 0; i < 2; i++) {
		integer = bt_ctf_field_sequence_get_field(sequence, i);
		if (ret) {
			fail("Failed to retrieve sequence element");
			goto end;
		}

		ret = bt_ctf_field_unsigned_integer_set_value(integer, i);
		if (ret) {
			fail("Failed to set sequence element value");
			goto end;
		}

		bt_put(integer);
		integer = NULL;
	}

	ret = bt_ctf_stream_append_event(stream, event);
	if (ret) {
		fail("Failed to append event to stream");
		goto end;
	}

	ret = bt_ctf_stream_flush(stream);
	if (ret) {
		fail("Failed to flush custom_event_header stream");
	}
end:
	bt_put(clock);
	bt_put(trace);
	bt_put(stream);
	bt_put(stream_class);
	bt_put(event_class);
	bt_put(event);
	bt_put(integer);
	bt_put(sequence);
	bt_put(event_header);
	bt_put(packet_header);
	bt_put(sequence_type);
	bt_put(integer_type);
	bt_put(event_header_type);
}

void test_instanciate_event_before_stream(struct bt_ctf_writer *writer)
{
	int ret = 0;
	struct bt_ctf_trace *trace = NULL;
	struct bt_ctf_clock *clock = NULL;
	struct bt_ctf_stream_class *stream_class = NULL;
	struct bt_ctf_stream *stream = NULL,
		*ret_stream = NULL;
	struct bt_ctf_event_class *event_class = NULL;
	struct bt_ctf_event *event = NULL;
	struct bt_ctf_field_type *integer_type = NULL;
	struct bt_ctf_field *integer = NULL;

	trace = bt_ctf_writer_get_trace(writer);
	if (!trace) {
		diag("Failed to get trace from writer");
		ret = -1;
		goto end;
	}

	clock = bt_ctf_trace_get_clock(trace, 0);
	if (!clock) {
		diag("Failed to get clock from trace");
		ret = -1;
		goto end;
	}

	stream_class = bt_ctf_stream_class_create("event_before_stream_test");
	if (!stream_class) {
		diag("Failed to create stream class");
		ret = -1;
		goto end;
	}

	ret = bt_ctf_stream_class_set_clock(stream_class, clock);
	if (ret) {
		diag("Failed to set stream class clock");
		goto end;
	}

	event_class = bt_ctf_event_class_create("some_event_class_name");
	integer_type = bt_ctf_field_type_integer_create(32);
	if (!integer_type) {
		diag("Failed to create integer field type");
		ret = -1;
		goto end;
	}

	ret = bt_ctf_event_class_add_field(event_class, integer_type,
		"integer_field");
	if (ret) {
		diag("Failed to add field to event class");
		goto end;
	}

	ret = bt_ctf_stream_class_add_event_class(stream_class,
		event_class);
	if (ret) {
		diag("Failed to add event class to stream class");
	}

	event = bt_ctf_event_create(event_class);
	if (!event) {
		diag("Failed to create event");
		ret = -1;
		goto end;
	}

	integer = bt_ctf_event_get_payload_by_index(event, 0);
	if (!integer) {
		diag("Failed to get integer field payload from event");
		ret = -1;
		goto end;
	}

	ret = bt_ctf_field_unsigned_integer_set_value(integer, 1234);
	if (ret) {
		diag("Failed to set integer field value");
		goto end;
	}

	stream = bt_ctf_writer_create_stream(writer, stream_class);
	if (!stream) {
		diag("Failed to create writer stream");
		ret = -1;
		goto end;
	}

	ok(bt_ctf_event_get_stream(NULL) == NULL,
		"bt_ctf_event_get_stream handles NULL correctly");
	ok(bt_ctf_event_get_stream(event) == NULL,
		"bt_ctf_event_get_stream returns NULL on event which has not yet been appended to a stream");

	ret = bt_ctf_stream_append_event(stream, event);
	if (ret) {
		diag("Failed to append event to stream");
		goto end;
	}

	ret_stream = bt_ctf_event_get_stream(event);
	ok(ret_stream == stream,
		"bt_ctf_event_get_stream returns an event's stream after it has been appended");
end:
	ok(ret == 0,
		"Create an event before instanciating its associated stream");
	bt_put(trace);
	bt_put(stream);
	bt_put(ret_stream);
	bt_put(stream_class);
	bt_put(event_class);
	bt_put(event);
	bt_put(integer_type);
	bt_put(integer);
	bt_put(clock);
}

void append_existing_event_class(struct bt_ctf_stream_class *stream_class)
{
	struct bt_ctf_event_class *event_class;

	event_class = bt_ctf_event_class_create("Simple Event");
	assert(event_class);
	ok(bt_ctf_stream_class_add_event_class(stream_class, event_class),
		"two event classes with the same name cannot cohabit within the same stream class");
	bt_put(event_class);

	event_class = bt_ctf_event_class_create("different name, ok");
	assert(event_class);
	assert(!bt_ctf_event_class_set_id(event_class, 11));
	ok(bt_ctf_stream_class_add_event_class(stream_class, event_class),
		"two event classes with the same ID cannot cohabit within the same stream class");
	bt_put(event_class);
}

int main(int argc, char **argv)
{
	char trace_path[] = "/tmp/ctfwriter_XXXXXX";
	char metadata_path[sizeof(trace_path) + 9];
	const char *clock_name = "test_clock";
	const char *clock_description = "This is a test clock";
	const char *returned_clock_name;
	const char *returned_clock_description;
	const uint64_t frequency = 1123456789;
	const uint64_t offset_s = 1351530929945824323;
	const uint64_t offset = 1234567;
	const uint64_t precision = 10;
	const int is_absolute = 0xFF;
	char *metadata_string;
	struct bt_ctf_writer *writer;
	struct utsname name;
	char hostname[BABELTRACE_HOST_NAME_MAX];
	struct bt_ctf_clock *clock, *ret_clock;
	struct bt_ctf_stream_class *stream_class, *ret_stream_class;
	struct bt_ctf_stream *stream1;
	const char *ret_string;
	const unsigned char *ret_uuid;
	unsigned char tmp_uuid[16] = { 0 };
	struct bt_ctf_field_type *packet_context_type,
		*packet_context_field_type,
		*packet_header_type,
		*packet_header_field_type,
		*integer_type,
		*stream_event_context_type,
		*ret_field_type,
		*event_header_field_type;
	struct bt_ctf_field *packet_header, *packet_header_field;
	struct bt_ctf_trace *trace;
	int ret;
	int64_t ret_int64_t;
	struct bt_value *obj;

	if (argc < 3) {
		printf("Usage: tests-ctf-writer path_to_ctf_parser_test path_to_babeltrace\n");
		return -1;
	}

	plan_no_plan();

	if (!bt_mkdtemp(trace_path)) {
		perror("# perror");
	}

	strcpy(metadata_path, trace_path);
	strcat(metadata_path + sizeof(trace_path) - 1, "/metadata");

	writer = bt_ctf_writer_create(trace_path);
	ok(writer, "bt_ctf_create succeeds in creating trace with path");

	ok(!bt_ctf_writer_get_trace(NULL),
		"bt_ctf_writer_get_trace correctly handles NULL");
	trace = bt_ctf_writer_get_trace(writer);
	ok(trace,
		"bt_ctf_writer_get_trace returns a bt_ctf_trace object");
	ok(bt_ctf_trace_set_byte_order(trace, BT_CTF_BYTE_ORDER_BIG_ENDIAN) == 0,
		"Set a trace's byte order to big endian");
	ok(bt_ctf_trace_get_byte_order(trace) == BT_CTF_BYTE_ORDER_BIG_ENDIAN,
		"bt_ctf_trace_get_byte_order returns a correct endianness");

	/* Add environment context to the trace */
	ret = gethostname(hostname, sizeof(hostname));
	if (ret < 0) {
		return ret;
	}
	ok(bt_ctf_writer_add_environment_field(writer, "host", hostname) == 0,
		"Add host (%s) environment field to writer instance",
		hostname);
	ok(bt_ctf_writer_add_environment_field(NULL, "test_field",
		"test_value"),
		"bt_ctf_writer_add_environment_field error with NULL writer");
	ok(bt_ctf_writer_add_environment_field(writer, NULL,
		"test_value"),
		"bt_ctf_writer_add_environment_field error with NULL field name");
	ok(bt_ctf_writer_add_environment_field(writer, "test_field",
		NULL),
		"bt_ctf_writer_add_environment_field error with NULL field value");

	/* Test bt_ctf_trace_set_environment_field with an integer object */
	obj = bt_value_integer_create_init(23);
	assert(obj);
	ok(bt_ctf_trace_set_environment_field(NULL, "test_env_int_obj", obj),
		"bt_ctf_trace_set_environment_field handles a NULL trace correctly");
	ok(bt_ctf_trace_set_environment_field(trace, NULL, obj),
		"bt_ctf_trace_set_environment_field handles a NULL name correctly");
	ok(bt_ctf_trace_set_environment_field(trace, "test_env_int_obj", NULL),
		"bt_ctf_trace_set_environment_field handles a NULL value correctly");
	ok(!bt_ctf_trace_set_environment_field(trace, "test_env_int_obj", obj),
		"bt_ctf_trace_set_environment_field succeeds in adding an integer object");
	BT_PUT(obj);

	/* Test bt_ctf_trace_set_environment_field with a string object */
	obj = bt_value_string_create_init("the value");
	assert(obj);
	ok(!bt_ctf_trace_set_environment_field(trace, "test_env_str_obj", obj),
		"bt_ctf_trace_set_environment_field succeeds in adding a string object");
	BT_PUT(obj);

	/* Test bt_ctf_trace_set_environment_field_integer */
	ok(bt_ctf_trace_set_environment_field_integer(NULL, "test_env_int",
		-194875),
		"bt_ctf_trace_set_environment_field_integer handles a NULL trace correctly");
	ok(bt_ctf_trace_set_environment_field_integer(trace, NULL, -194875),
		"bt_ctf_trace_set_environment_field_integer handles a NULL name correctly");
	ok(!bt_ctf_trace_set_environment_field_integer(trace, "test_env_int",
		-164973),
		"bt_ctf_trace_set_environment_field_integer succeeds");

	/* Test bt_ctf_trace_set_environment_field_string */
	ok(bt_ctf_trace_set_environment_field_string(NULL, "test_env_str",
		"yeah"),
		"bt_ctf_trace_set_environment_field_string handles a NULL trace correctly");
	ok(bt_ctf_trace_set_environment_field_string(trace, NULL, "yeah"),
		"bt_ctf_trace_set_environment_field_string handles a NULL name correctly");
	ok(bt_ctf_trace_set_environment_field_string(trace, "test_env_str",
		NULL),
		"bt_ctf_trace_set_environment_field_string handles a NULL value correctly");
	ok(!bt_ctf_trace_set_environment_field_string(trace, "test_env_str",
		"oh yeah"),
		"bt_ctf_trace_set_environment_field_string succeeds");

	/* Test bt_ctf_trace_get_environment_field_count */
	ok(bt_ctf_trace_get_environment_field_count(NULL) < 0,
		"bt_ctf_trace_get_environment_field_count handles a NULL trace correctly");
	ok(bt_ctf_trace_get_environment_field_count(trace) == 5,
		"bt_ctf_trace_get_environment_field_count returns a correct number of environment fields");

	/* Test bt_ctf_trace_get_environment_field_name */
	ok(bt_ctf_trace_get_environment_field_name(NULL, 0) == NULL,
		"bt_ctf_trace_get_environment_field_name handles a NULL trace correctly");
	ok(bt_ctf_trace_get_environment_field_name(trace, -1) == NULL,
		"bt_ctf_trace_get_environment_field_name handles an invalid index correctly (negative)");
	ok(bt_ctf_trace_get_environment_field_name(trace, 5) == NULL,
		"bt_ctf_trace_get_environment_field_name handles an invalid index correctly (too large)");
	ret_string = bt_ctf_trace_get_environment_field_name(trace, 0);
	ok(ret_string && !strcmp(ret_string, "host"),
		"bt_ctf_trace_get_environment_field_name returns a correct field name");
	ret_string = bt_ctf_trace_get_environment_field_name(trace, 1);
	ok(ret_string && !strcmp(ret_string, "test_env_int_obj"),
		"bt_ctf_trace_get_environment_field_name returns a correct field name");
	ret_string = bt_ctf_trace_get_environment_field_name(trace, 2);
	ok(ret_string && !strcmp(ret_string, "test_env_str_obj"),
		"bt_ctf_trace_get_environment_field_name returns a correct field name");
	ret_string = bt_ctf_trace_get_environment_field_name(trace, 3);
	ok(ret_string && !strcmp(ret_string, "test_env_int"),
		"bt_ctf_trace_get_environment_field_name returns a correct field name");
	ret_string = bt_ctf_trace_get_environment_field_name(trace, 4);
	ok(ret_string && !strcmp(ret_string, "test_env_str"),
		"bt_ctf_trace_get_environment_field_name returns a correct field name");

	/* Test bt_ctf_trace_get_environment_field_value */
	ok(bt_ctf_trace_get_environment_field_value(NULL, 0) == NULL,
		"bt_ctf_trace_get_environment_field_value handles a NULL trace correctly");
	ok(bt_ctf_trace_get_environment_field_value(trace, -1) == NULL,
		"bt_ctf_trace_get_environment_field_value handles an invalid index correctly (negative)");
	ok(bt_ctf_trace_get_environment_field_value(trace, 5) == NULL,
		"bt_ctf_trace_get_environment_field_value handles an invalid index correctly (too large)");
	obj = bt_ctf_trace_get_environment_field_value(trace, 1);
	ret = bt_value_integer_get(obj, &ret_int64_t);
	ok(!ret && ret_int64_t == 23,
		"bt_ctf_trace_get_environment_field_value succeeds in getting an integer value");
	BT_PUT(obj);
	obj = bt_ctf_trace_get_environment_field_value(trace, 2);
	ret = bt_value_string_get(obj, &ret_string);
	ok(!ret && ret_string && !strcmp(ret_string, "the value"),
		"bt_ctf_trace_get_environment_field_value succeeds in getting a string value");
	BT_PUT(obj);

	/* Test bt_ctf_trace_get_environment_field_value_by_name */
	ok(!bt_ctf_trace_get_environment_field_value_by_name(NULL,
		"test_env_str"),
		"bt_ctf_trace_get_environment_field_value_by_name handles a NULL trace correctly");
	ok(!bt_ctf_trace_get_environment_field_value_by_name(trace, NULL),
		"bt_ctf_trace_get_environment_field_value_by_name handles a NULL name correctly");
	ok(!bt_ctf_trace_get_environment_field_value_by_name(trace, "oh oh"),
		"bt_ctf_trace_get_environment_field_value_by_name returns NULL or an unknown field name");
	obj = bt_ctf_trace_get_environment_field_value_by_name(trace,
		"test_env_str");
	ret = bt_value_string_get(obj, &ret_string);
	ok(!ret && ret_string && !strcmp(ret_string, "oh yeah"),
		"bt_ctf_trace_get_environment_field_value_by_name succeeds in getting an existing field");
	BT_PUT(obj);

	/* Test environment field replacement */
	ok(!bt_ctf_trace_set_environment_field_integer(trace, "test_env_int",
		654321),
		"bt_ctf_trace_set_environment_field_integer succeeds with an existing name");
	ok(bt_ctf_trace_get_environment_field_count(trace) == 5,
		"bt_ctf_trace_set_environment_field_integer with an existing key does not increase the environment size");
	obj = bt_ctf_trace_get_environment_field_value(trace, 3);
	ret = bt_value_integer_get(obj, &ret_int64_t);
	ok(!ret && ret_int64_t == 654321,
		"bt_ctf_trace_get_environment_field_value successfully replaces an existing field");
	BT_PUT(obj);

	/* On Solaris, uname() can return any positive value on success */
	if (uname(&name) < 0) {
		perror("uname");
		return -1;
	}

	ok(bt_ctf_writer_add_environment_field(writer, "sysname", name.sysname)
		== 0, "Add sysname (%s) environment field to writer instance",
		name.sysname);
	ok(bt_ctf_writer_add_environment_field(writer, "nodename",
		name.nodename) == 0,
		"Add nodename (%s) environment field to writer instance",
		name.nodename);
	ok(bt_ctf_writer_add_environment_field(writer, "release", name.release)
		== 0, "Add release (%s) environment field to writer instance",
		name.release);
	ok(bt_ctf_writer_add_environment_field(writer, "version", name.version)
		== 0, "Add version (%s) environment field to writer instance",
		name.version);
	ok(bt_ctf_writer_add_environment_field(writer, "machine", name.machine)
		== 0, "Add machine (%s) environment field to writer istance",
		name.machine);

	/* Define a clock and add it to the trace */
	ok(bt_ctf_clock_create("signed") == NULL,
		"Illegal clock name rejected");
	ok(bt_ctf_clock_create(NULL) == NULL, "NULL clock name rejected");
	clock = bt_ctf_clock_create(clock_name);
	ok(clock, "Clock created sucessfully");
	returned_clock_name = bt_ctf_clock_get_name(clock);
	ok(returned_clock_name, "bt_ctf_clock_get_name returns a clock name");
	ok(returned_clock_name ? !strcmp(returned_clock_name, clock_name) : 0,
		"Returned clock name is valid");

	returned_clock_description = bt_ctf_clock_get_description(clock);
	ok(!returned_clock_description, "bt_ctf_clock_get_description returns NULL on an unset description");
	ok(bt_ctf_clock_set_description(clock, clock_description) == 0,
		"Clock description set successfully");

	returned_clock_description = bt_ctf_clock_get_description(clock);
	ok(returned_clock_description,
		"bt_ctf_clock_get_description returns a description.");
	ok(returned_clock_description ?
		!strcmp(returned_clock_description, clock_description) : 0,
		"Returned clock description is valid");

	ok(bt_ctf_clock_get_frequency(clock) == DEFAULT_CLOCK_FREQ,
		"bt_ctf_clock_get_frequency returns the correct default frequency");
	ok(bt_ctf_clock_set_frequency(clock, frequency) == 0,
		"Set clock frequency");
	ok(bt_ctf_clock_get_frequency(clock) == frequency,
		"bt_ctf_clock_get_frequency returns the correct frequency once it is set");

	ok(bt_ctf_clock_get_offset_s(clock) == DEFAULT_CLOCK_OFFSET_S,
		"bt_ctf_clock_get_offset_s returns the correct default offset (in seconds)");
	ok(bt_ctf_clock_set_offset_s(clock, offset_s) == 0,
		"Set clock offset (seconds)");
	ok(bt_ctf_clock_get_offset_s(clock) == offset_s,
		"bt_ctf_clock_get_offset_s returns the correct default offset (in seconds) once it is set");

	ok(bt_ctf_clock_get_offset(clock) == DEFAULT_CLOCK_OFFSET,
		"bt_ctf_clock_get_frequency returns the correct default offset (in ticks)");
	ok(bt_ctf_clock_set_offset(clock, offset) == 0, "Set clock offset");
	ok(bt_ctf_clock_get_offset(clock) == offset,
		"bt_ctf_clock_get_frequency returns the correct default offset (in ticks) once it is set");

	ok(bt_ctf_clock_get_precision(clock) == DEFAULT_CLOCK_PRECISION,
		"bt_ctf_clock_get_precision returns the correct default precision");
	ok(bt_ctf_clock_set_precision(clock, precision) == 0,
		"Set clock precision");
	ok(bt_ctf_clock_get_precision(clock) == precision,
		"bt_ctf_clock_get_precision returns the correct precision once it is set");

	ok(bt_ctf_clock_get_is_absolute(clock) == DEFAULT_CLOCK_IS_ABSOLUTE,
		"bt_ctf_clock_get_precision returns the correct default is_absolute attribute");
	ok(bt_ctf_clock_set_is_absolute(clock, is_absolute) == 0,
		"Set clock absolute property");
	ok(bt_ctf_clock_get_is_absolute(clock) == !!is_absolute,
		"bt_ctf_clock_get_precision returns the correct is_absolute attribute once it is set");

	ok(bt_ctf_clock_get_time(clock) == DEFAULT_CLOCK_TIME,
		"bt_ctf_clock_get_time returns the correct default time");
	ok(bt_ctf_clock_set_time(clock, current_time) == 0,
		"Set clock time");
	ok(bt_ctf_clock_get_time(clock) == current_time,
		"bt_ctf_clock_get_time returns the correct time once it is set");

	ok(bt_ctf_writer_add_clock(writer, clock) == 0,
		"Add clock to writer instance");
	ok(bt_ctf_writer_add_clock(writer, clock),
		"Verify a clock can't be added twice to a writer instance");

	ok(bt_ctf_trace_get_clock_count(NULL) < 0,
		"bt_ctf_trace_get_clock_count correctly handles NULL");
	ok(bt_ctf_trace_get_clock_count(trace) == 1,
		"bt_ctf_trace_get_clock_count returns the correct number of clocks");
	ok(!bt_ctf_trace_get_clock(NULL, 0),
		"bt_ctf_trace_get_clock correctly handles NULL");
	ok(!bt_ctf_trace_get_clock(trace, -1),
		"bt_ctf_trace_get_clock correctly handles negative indexes");
	ok(!bt_ctf_trace_get_clock(trace, 1),
		"bt_ctf_trace_get_clock correctly handles out of bound accesses");
	ret_clock = bt_ctf_trace_get_clock(trace, 0);
	ok(ret_clock == clock,
		"bt_ctf_trace_get_clock returns the right clock instance");
	bt_put(ret_clock);
	ok(!bt_ctf_trace_get_clock_by_name(trace, NULL),
		"bt_ctf_trace_get_clock_by_name correctly handles NULL (trace)");
	ok(!bt_ctf_trace_get_clock_by_name(NULL, clock_name),
		"bt_ctf_trace_get_clock_by_name correctly handles NULL (clock name)");
	ok(!bt_ctf_trace_get_clock_by_name(NULL, NULL),
		"bt_ctf_trace_get_clock_by_name correctly handles NULL (both)");
	ret_clock = bt_ctf_trace_get_clock_by_name(trace, clock_name);
	ok(ret_clock == clock,
		"bt_ctf_trace_get_clock_by_name returns the right clock instance");
	bt_put(ret_clock);
	ok(!bt_ctf_trace_get_clock_by_name(trace, "random"),
		"bt_ctf_trace_get_clock_by_name fails when the requested clock doesn't exist");

	ok(!bt_ctf_clock_get_name(NULL),
		"bt_ctf_clock_get_name correctly handles NULL");
	ok(!bt_ctf_clock_get_description(NULL),
		"bt_ctf_clock_get_description correctly handles NULL");
	ok(bt_ctf_clock_get_frequency(NULL) == -1ULL,
		"bt_ctf_clock_get_frequency correctly handles NULL");
	ok(bt_ctf_clock_get_precision(NULL) == -1ULL,
		"bt_ctf_clock_get_precision correctly handles NULL");
	ok(bt_ctf_clock_get_offset_s(NULL) == -1ULL,
		"bt_ctf_clock_get_offset_s correctly handles NULL");
	ok(bt_ctf_clock_get_offset(NULL) == -1ULL,
		"bt_ctf_clock_get_offset correctly handles NULL");
	ok(bt_ctf_clock_get_is_absolute(NULL) < 0,
		"bt_ctf_clock_get_is_absolute correctly handles NULL");
	ok(bt_ctf_clock_get_time(NULL) == -1ULL,
		"bt_ctf_clock_get_time correctly handles NULL");

	ok(bt_ctf_clock_set_description(NULL, NULL) < 0,
		"bt_ctf_clock_set_description correctly handles NULL clock");
	ok(bt_ctf_clock_set_frequency(NULL, frequency) < 0,
		"bt_ctf_clock_set_frequency correctly handles NULL clock");
	ok(bt_ctf_clock_set_precision(NULL, precision) < 0,
		"bt_ctf_clock_get_precision correctly handles NULL clock");
	ok(bt_ctf_clock_set_offset_s(NULL, offset_s) < 0,
		"bt_ctf_clock_set_offset_s correctly handles NULL clock");
	ok(bt_ctf_clock_set_offset(NULL, offset) < 0,
		"bt_ctf_clock_set_offset correctly handles NULL clock");
	ok(bt_ctf_clock_set_is_absolute(NULL, is_absolute) < 0,
		"bt_ctf_clock_set_is_absolute correctly handles NULL clock");
	ok(bt_ctf_clock_set_time(NULL, current_time) < 0,
		"bt_ctf_clock_set_time correctly handles NULL clock");
	ok(bt_ctf_clock_get_uuid(NULL) == NULL,
		"bt_ctf_clock_get_uuid correctly handles NULL clock");
	ret_uuid = bt_ctf_clock_get_uuid(clock);
	ok(ret_uuid,
		"bt_ctf_clock_get_uuid returns a UUID");
	if (ret_uuid) {
		memcpy(tmp_uuid, ret_uuid, sizeof(tmp_uuid));
		/* Slightly modify UUID */
		tmp_uuid[sizeof(tmp_uuid) - 1]++;
	}

	ok(bt_ctf_clock_set_uuid(NULL, tmp_uuid) < 0,
		"bt_ctf_clock_set_uuid correctly handles a NULL clock");
	ok(bt_ctf_clock_set_uuid(clock, NULL) < 0,
		"bt_ctf_clock_set_uuid correctly handles a NULL UUID");
	ok(bt_ctf_clock_set_uuid(clock, tmp_uuid) == 0,
		"bt_ctf_clock_set_uuid sets a new uuid succesfully");
	ret_uuid = bt_ctf_clock_get_uuid(clock);
	ok(ret_uuid,
		"bt_ctf_clock_get_uuid returns a UUID after setting a new one");
	ok(uuid_match(ret_uuid, tmp_uuid),
		"bt_ctf_clock_get_uuid returns the correct UUID after setting a new one");

	/* Define a stream class */
	stream_class = bt_ctf_stream_class_create("test_stream");

	ok(bt_ctf_stream_class_get_name(NULL) == NULL,
		"bt_ctf_stream_class_get_name handles NULL correctly");
	ret_string = bt_ctf_stream_class_get_name(stream_class);
	ok(ret_string && !strcmp(ret_string, "test_stream"),
		"bt_ctf_stream_class_get_name returns a correct stream class name");

	ok(bt_ctf_stream_class_get_clock(stream_class) == NULL,
		"bt_ctf_stream_class_get_clock returns NULL when a clock was not set");
	ok(bt_ctf_stream_class_get_clock(NULL) == NULL,
		"bt_ctf_stream_class_get_clock handles NULL correctly");

	ok(stream_class, "Create stream class");
	ok(bt_ctf_stream_class_set_clock(stream_class, clock) == 0,
		"Set a stream class' clock");
	ret_clock = bt_ctf_stream_class_get_clock(stream_class);
	ok(ret_clock == clock,
		"bt_ctf_stream_class_get_clock returns a correct clock");
	bt_put(ret_clock);

	/* Test the event fields and event types APIs */
	type_field_tests();

	/* Test fields copying */
	field_copy_tests();

	ok(bt_ctf_stream_class_get_id(stream_class) < 0,
		"bt_ctf_stream_class_get_id returns an error when no id is set");
	ok(bt_ctf_stream_class_get_id(NULL) < 0,
		"bt_ctf_stream_class_get_id handles NULL correctly");
	ok(bt_ctf_stream_class_set_id(NULL, 123) < 0,
		"bt_ctf_stream_class_set_id handles NULL correctly");
	ok(bt_ctf_stream_class_set_id(stream_class, 123) == 0,
		"Set an stream class' id");
	ok(bt_ctf_stream_class_get_id(stream_class) == 123,
		"bt_ctf_stream_class_get_id returns the correct value");

	/* Validate default event header fields */
	ok(bt_ctf_stream_class_get_event_header_type(NULL) == NULL,
		"bt_ctf_stream_class_get_event_header_type handles NULL correctly");
	ret_field_type = bt_ctf_stream_class_get_event_header_type(
		stream_class);
	ok(ret_field_type,
		"bt_ctf_stream_class_get_event_header_type returns an event header type");
	ok(bt_ctf_field_type_get_type_id(ret_field_type) == CTF_TYPE_STRUCT,
		"Default event header type is a structure");
	event_header_field_type =
		bt_ctf_field_type_structure_get_field_type_by_name(
		ret_field_type, "id");
	ok(event_header_field_type,
		"Default event header type contains an \"id\" field");
	ok(bt_ctf_field_type_get_type_id(
		event_header_field_type) == CTF_TYPE_INTEGER,
		"Default event header \"id\" field is an integer");
	bt_put(event_header_field_type);
	event_header_field_type =
		bt_ctf_field_type_structure_get_field_type_by_name(
		ret_field_type, "timestamp");
	ok(event_header_field_type,
		"Default event header type contains a \"timestamp\" field");
	ok(bt_ctf_field_type_get_type_id(
		event_header_field_type) == CTF_TYPE_INTEGER,
		"Default event header \"timestamp\" field is an integer");
	bt_put(event_header_field_type);
	bt_put(ret_field_type);

	/* Add a custom trace packet header field */
	ok(bt_ctf_trace_get_packet_header_type(NULL) == NULL,
		"bt_ctf_trace_get_packet_header_type handles NULL correctly");
	packet_header_type = bt_ctf_trace_get_packet_header_type(trace);
	ok(packet_header_type,
		"bt_ctf_trace_get_packet_header_type returns a packet header");
	ok(bt_ctf_field_type_get_type_id(packet_header_type) == CTF_TYPE_STRUCT,
		"bt_ctf_trace_get_packet_header_type returns a packet header of type struct");
	ret_field_type = bt_ctf_field_type_structure_get_field_type_by_name(
		packet_header_type, "magic");
	ok(ret_field_type, "Default packet header type contains a \"magic\" field");
	bt_put(ret_field_type);
	ret_field_type = bt_ctf_field_type_structure_get_field_type_by_name(
		packet_header_type, "uuid");
	ok(ret_field_type, "Default packet header type contains a \"uuid\" field");
	bt_put(ret_field_type);
	ret_field_type = bt_ctf_field_type_structure_get_field_type_by_name(
		packet_header_type, "stream_id");
	ok(ret_field_type, "Default packet header type contains a \"stream_id\" field");
	bt_put(ret_field_type);

	packet_header_field_type = bt_ctf_field_type_integer_create(22);
	ok(!bt_ctf_field_type_structure_add_field(packet_header_type,
		packet_header_field_type, "custom_trace_packet_header_field"),
		"Added a custom trace packet header field successfully");

	ok(bt_ctf_trace_set_packet_header_type(NULL, packet_header_type) < 0,
		"bt_ctf_trace_set_packet_header_type handles a NULL trace correctly");
	ok(bt_ctf_trace_set_packet_header_type(trace, NULL) < 0,
		"bt_ctf_trace_set_packet_header_type handles a NULL packet_header_type correctly");
	ok(!bt_ctf_trace_set_packet_header_type(trace, packet_header_type),
		"Set a trace packet_header_type successfully");

	ok(bt_ctf_stream_class_get_packet_context_type(NULL) == NULL,
		"bt_ctf_stream_class_get_packet_context_type handles NULL correctly");

	/* Add a custom field to the stream class' packet context */
	packet_context_type = bt_ctf_stream_class_get_packet_context_type(stream_class);
	ok(packet_context_type,
		"bt_ctf_stream_class_get_packet_context_type returns a packet context type.");
	ok(bt_ctf_field_type_get_type_id(packet_context_type) == CTF_TYPE_STRUCT,
		"Packet context is a structure");

	ok(bt_ctf_stream_class_set_packet_context_type(NULL, packet_context_type),
		"bt_ctf_stream_class_set_packet_context_type handles a NULL stream class correctly");
	ok(bt_ctf_stream_class_set_packet_context_type(stream_class, NULL),
		"bt_ctf_stream_class_set_packet_context_type handles a NULL packet context type correctly");

	integer_type = bt_ctf_field_type_integer_create(32);
	ok(bt_ctf_stream_class_set_packet_context_type(stream_class,
		integer_type) < 0,
		"bt_ctf_stream_class_set_packet_context_type rejects a packet context that is not a structure");
	/* Create a "uint5_t" equivalent custom packet context field */
	packet_context_field_type = bt_ctf_field_type_integer_create(5);

	ret = bt_ctf_field_type_structure_add_field(packet_context_type,
		packet_context_field_type, "custom_packet_context_field");
	ok(ret == 0, "Packet context field added successfully");

	/* Define a stream event context containing a my_integer field. */
	ok(bt_ctf_stream_class_get_event_context_type(NULL) == NULL,
		"bt_ctf_stream_class_get_event_context_type handles NULL correctly");
	ok(bt_ctf_stream_class_get_event_context_type(
		stream_class) == NULL,
		"bt_ctf_stream_class_get_event_context_type returns NULL when no stream event context type was set.");
	stream_event_context_type = bt_ctf_field_type_structure_create();
	bt_ctf_field_type_structure_add_field(stream_event_context_type,
		integer_type, "common_event_context");

	ok(bt_ctf_stream_class_set_event_context_type(NULL,
		stream_event_context_type) < 0,
		"bt_ctf_stream_class_set_event_context_type handles a NULL stream_class correctly");
	ok(bt_ctf_stream_class_set_event_context_type(stream_class,
		NULL) < 0,
		"bt_ctf_stream_class_set_event_context_type handles a NULL event_context correctly");
	ok(bt_ctf_stream_class_set_event_context_type(stream_class,
		integer_type) < 0,
		"bt_ctf_stream_class_set_event_context_type validates that the event context os a structure");

	ok(bt_ctf_stream_class_set_event_context_type(
		stream_class, stream_event_context_type) == 0,
		"Set a new stream event context type");
	ret_field_type = bt_ctf_stream_class_get_event_context_type(
		stream_class);
	ok(ret_field_type == stream_event_context_type,
		"bt_ctf_stream_class_get_event_context_type returns the correct field type.");
	bt_put(ret_field_type);

	/* Instantiate a stream and append events */
	stream1 = bt_ctf_writer_create_stream(writer, stream_class);
	ok(stream1, "Instanciate a stream class from writer");

	ok(bt_ctf_stream_get_class(NULL) == NULL,
		"bt_ctf_stream_get_class correctly handles NULL");
	ret_stream_class = bt_ctf_stream_get_class(stream1);
	ok(ret_stream_class,
		"bt_ctf_stream_get_class returns a stream class");
	ok(ret_stream_class == stream_class,
		"Returned stream class is of the correct type");

	/*
	 * Try to modify the packet context type after a stream has been
	 * created.
	 */
	ret = bt_ctf_field_type_structure_add_field(packet_header_type,
		packet_header_field_type, "should_fail");
	ok(ret < 0,
		"Trace packet header type can't be modified once a stream has been instanciated");

	/*
	 * Try to modify the packet context type after a stream has been
	 * created.
	 */
	ret = bt_ctf_field_type_structure_add_field(packet_context_type,
		packet_context_field_type, "should_fail");
	ok(ret < 0,
		"Packet context type can't be modified once a stream has been instanciated");

	/*
	 * Try to modify the stream event context type after a stream has been
	 * created.
	 */
	ret = bt_ctf_field_type_structure_add_field(stream_event_context_type,
		integer_type, "should_fail");
	ok(ret < 0,
		"Stream event context type can't be modified once a stream has been instanciated");

	/* Should fail after instanciating a stream (frozen) */
	ok(bt_ctf_stream_class_set_clock(stream_class, clock),
		"Changes to a stream class that was already instantiated fail");

	/* Populate the custom packet header field only once for all tests */
	ok(bt_ctf_stream_get_packet_header(NULL) == NULL,
		"bt_ctf_stream_get_packet_header handles NULL correctly");
	packet_header = bt_ctf_stream_get_packet_header(stream1);
	ok(packet_header,
		"bt_ctf_stream_get_packet_header returns a packet header");
	ret_field_type = bt_ctf_field_get_type(packet_header);
	ok(ret_field_type == packet_header_type,
		"Stream returns a packet header of the appropriate type");
	bt_put(ret_field_type);
	packet_header_field = bt_ctf_field_structure_get_field(packet_header,
		"custom_trace_packet_header_field");
	ok(packet_header_field,
		"Packet header structure contains a custom field with the appropriate name");
	ret_field_type = bt_ctf_field_get_type(packet_header_field);
	ok(ret_field_type == packet_header_field_type,
		"Custom packet header field is of the expected type");
	ok(!bt_ctf_field_unsigned_integer_set_value(packet_header_field,
		54321), "Set custom packet header value successfully");
	ok(bt_ctf_stream_set_packet_header(stream1, NULL) < 0,
		"bt_ctf_stream_set_packet_header handles a NULL packet header correctly");
	ok(bt_ctf_stream_set_packet_header(NULL, packet_header) < 0,
		"bt_ctf_stream_set_packet_header handles a NULL stream correctly");
	ok(bt_ctf_stream_set_packet_header(stream1, packet_header_field) < 0,
		"bt_ctf_stream_set_packet_header rejects a packet header of the wrong type");
	ok(!bt_ctf_stream_set_packet_header(stream1, packet_header),
		"Successfully set a stream's packet header");

	ok(bt_ctf_writer_add_environment_field(writer, "new_field", "test") == 0,
		"Add environment field to writer after stream creation");

	test_instanciate_event_before_stream(writer);

	append_simple_event(stream_class, stream1, clock);

	packet_resize_test(stream_class, stream1, clock);

	append_complex_event(stream_class, stream1, clock);

	append_existing_event_class(stream_class);

	test_empty_stream(writer);

	test_custom_event_header_stream(writer);

	metadata_string = bt_ctf_writer_get_metadata_string(writer);
	ok(metadata_string, "Get metadata string");

	bt_ctf_writer_flush_metadata(writer);
	validate_metadata(argv[1], metadata_path);
	validate_trace(argv[2], trace_path);

	bt_put(clock);
	bt_put(ret_stream_class);
	bt_put(writer);
	bt_put(stream1);
	bt_put(packet_context_type);
	bt_put(packet_context_field_type);
	bt_put(integer_type);
	bt_put(stream_event_context_type);
	bt_put(ret_field_type);
	bt_put(packet_header_type);
	bt_put(packet_header_field_type);
	bt_put(packet_header);
	bt_put(packet_header_field);
	bt_put(trace);
	free(metadata_string);

	ok(bt_ctf_stream_class_get_trace(stream_class) == NULL,
		"bt_ctf_stream_class_get_trace returns NULL after its trace has been reclaimed");
	bt_put(stream_class);

	/* Remove all trace files and delete temporary trace directory */
	DIR *trace_dir = opendir(trace_path);
	if (!trace_dir) {
		perror("# opendir");
		return -1;
	}

	struct dirent *entry;
	while ((entry = readdir(trace_dir))) {
		struct stat st;
		char filename[PATH_MAX];

		if (snprintf(filename, sizeof(filename), "%s/%s",
					trace_path, entry->d_name) <= 0) {
			continue;
		}

		if (stat(entry->d_name, &st)) {
			continue;
		}

		if (S_ISREG(st.st_mode)) {
			unlinkat(bt_dirfd(trace_dir), entry->d_name, 0);
		}
	}

	rmdir(trace_path);
	closedir(trace_dir);
	return 0;
}
