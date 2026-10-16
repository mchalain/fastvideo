# Writing a convert library

[← Back to README](../README.md)

The [passthrough](modules.md#passthrough) module can apply a CPU pixel
conversion provided by a separate shared library. Such a library must define
and fill a `Convert_t` structure (see [`spassthrough.h`](../src/spassthrough.h)):

```c
Convert_t convert_rgba =
{
	.name = "bayer2rgb",
	.copy = 1,
	.fourcc_in = 0,
	.fourcc_out = FOURCC('A','B','2','4'),
	.ops =
	{
		.create = convert_create,
		.convert = convert_convert,
		.destroy = convert_destroy,
	},
};
```

It must also register this structure when it is loaded, from a constructor
function:

```c
#include <dlfcn.h>

static void __attribute__ ((constructor)) convert_init()
{
	spassthrough_convert_append_t _spassthrough_convert_append = NULL;
	void *hdl = dlopen("libfastvideo.so", RTLD_NOW);
	if (hdl != NULL)
		_spassthrough_convert_append = dlsym(hdl, "spassthrough_convert_append");
	if (_spassthrough_convert_append)
	{
		_spassthrough_convert_append(&convert_rgba);
	}
}
```

The converter is then selected by name in the JSON configuration, with the
`convert` and `library` entries of a [passthrough](modules.md#passthrough)
object. The `sconvert_*.c` files in [`src/`](../src) are working examples.
