#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

/*
 * Convert a text dump of a binary buffer (one byte per value, values
 * separated by commas and/or white spaces) into the raw binary buffer.
 * This is the format of data/rp1_fe_config_<sensor>.csv, a struct pisp_fe_config
 * to queue on the rp1-cfe-fe_config node (see docs/rp1-fe-config.md).
 *
 * Values may be decimal (236), hexadecimal (0xec) or octal (0354), but
 * each one must fit into a byte (0..255).
 */

#define DEFAULT_SIZE 632 // sizeof(struct pisp_fe_config)

static void help(const char *name)
{
	fprintf(stderr, "Usage: %s [OPTION]... [INPUT]\n", name);
	fprintf(stderr, "Convert a CSV byte dump into a binary file.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h            display this message\n");
	fprintf(stderr, "  -o <file>     output file (default: stdout)\n");
	fprintf(stderr, "  -s <size>     expected number of bytes, 0 to disable the check (default: %d)\n", DEFAULT_SIZE);
	fprintf(stderr, "\n");
	fprintf(stderr, "INPUT is the CSV file to read (default: stdin)\n");
}

/*
 * Read the next value from the stream.
 * Returns 1 on success, 0 at the end of the stream, -1 on error.
 */
static int readvalue(FILE *in, long *value, unsigned int *line)
{
	char token[32];
	size_t len = 0;
	int c;

	/// skip separators
	while ((c = fgetc(in)) != EOF && (isspace(c) || c == ','))
	{
		if (c == '\n')
			(*line)++;
	}
	if (c == EOF)
		return 0;

	while (c != EOF && !isspace(c) && c != ',')
	{
		if (len >= sizeof(token) - 1)
		{
			fprintf(stderr, "csv2pisp_fe_config: line %u: value too long\n", *line);
			return -1;
		}
		token[len++] = c;
		c = fgetc(in);
	}
	token[len] = '\0';

	char *end = NULL;
	errno = 0;
	*value = strtol(token, &end, 0);
	if (errno != 0 || end == token || *end != '\0')
	{
		fprintf(stderr, "csv2pisp_fe_config: line %u: '%s' is not a number\n", *line, token);
		return -1;
	}
	if (*value < 0 || *value > 255)
	{
		fprintf(stderr, "csv2pisp_fe_config: line %u: %ld is not a byte value (0..255)\n", *line, *value);
		return -1;
	}
	if (c == '\n')
		(*line)++;
	return 1;
}

int main(int argc, char * const argv[])
{
	const char *output = NULL;
	long expected = DEFAULT_SIZE;
	int opt;

	while ((opt = getopt(argc, argv, "ho:s:")) != -1)
	{
		switch (opt)
		{
			case 'h':
				help(argv[0]);
				return 0;
			case 'o':
				output = optarg;
			break;
			case 's':
				expected = strtol(optarg, NULL, 0);
			break;
			default:
				help(argv[0]);
				return -1;
		}
	}

	FILE *in = stdin;
	if (optind < argc)
	{
		in = fopen(argv[optind], "r");
		if (in == NULL)
		{
			fprintf(stderr, "csv2pisp_fe_config: %s: %s\n", argv[optind], strerror(errno));
			return -1;
		}
	}

	size_t size = 0;
	size_t length = 1024;
	unsigned char *buffer = malloc(length);
	unsigned int line = 1;
	long value;
	int ret;
	while (buffer && (ret = readvalue(in, &value, &line)) > 0)
	{
		if (size == length)
		{
			length *= 2;
			unsigned char *tmp = realloc(buffer, length);
			if (tmp == NULL)
				free(buffer);
			buffer = tmp;
			if (buffer == NULL)
				break;
		}
		buffer[size++] = value;
	}
	if (in != stdin)
		fclose(in);
	if (buffer == NULL)
	{
		fprintf(stderr, "csv2pisp_fe_config: out of memory\n");
		return -1;
	}
	if (ret < 0)
	{
		free(buffer);
		return -1;
	}
	if (expected > 0 && size != (size_t)expected)
	{
		fprintf(stderr, "csv2pisp_fe_config: %zu bytes read, %ld expected (use -s to change)\n", size, expected);
		free(buffer);
		return -1;
	}

	FILE *out = stdout;
	if (output != NULL)
	{
		out = fopen(output, "wb");
		if (out == NULL)
		{
			fprintf(stderr, "csv2pisp_fe_config: %s: %s\n", output, strerror(errno));
			free(buffer);
			return -1;
		}
	}
	ret = 0;
	if (fwrite(buffer, 1, size, out) != size)
	{
		fprintf(stderr, "csv2pisp_fe_config: write error: %s\n", strerror(errno));
		ret = -1;
	}
	if (out != stdout && fclose(out) != 0)
	{
		fprintf(stderr, "csv2pisp_fe_config: %s: %s\n", output, strerror(errno));
		ret = -1;
	}
	free(buffer);
	return ret;
}
