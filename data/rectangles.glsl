#extension GL_OES_EGL_image_external : require
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform samplerExternalOES vTexture;
/*
 * Draws the outlines of up to MAX_RECTS rectangles over the image.
 *
 * vRects[i] = (x1, y1, x2, y2) in normalised image coordinates: (0,0) is
 * the first pixel of the image and (1,1) the last one, the same convention
 * as the JSON detections of the Hailo plugins. A rectangle with x2 <= x1 or
 * y2 <= y1 is not drawn, so the entries never set (all zeros) are ignored.
 *
 * Each entry is a separate "vec4" control of the gpu program, named
 * "vRects[0]" to "vRects[9]". The values must be written as reals
 * (0.0, not 0).
 */
#define MAX_RECTS 10
uniform vec4 vRects[MAX_RECTS];
uniform vec4 vColor;      // outline colour (default: opaque red)
uniform float vThickness; // outline width in pixels (default: 2)
uniform int vMono;        // 1: monochrome source (R16), display the red channel as grey
varying vec2 texUV;
varying vec2 resolution;

void main()
{
	vec4 color = texture2D(vTexture, texUV);
	if (vMono != 0)
		color = vec4(vec3(color.r), 1.0);

	vec4 outline = (vColor.a > 0.0) ? vColor : vec4(1.0, 0.0, 0.0, 1.0);
	vec2 border = ((vThickness > 0.0) ? vThickness : 2.0) / resolution;
	for (int i = 0; i < MAX_RECTS; i++)
	{
		vec4 rect = vRects[i];
		if (rect.z <= rect.x || rect.w <= rect.y)
			continue;
		bool inside = all(greaterThanEqual(texUV, rect.xy)) &&
				all(lessThanEqual(texUV, rect.zw));
		bool core = all(greaterThanEqual(texUV, rect.xy + border)) &&
				all(lessThanEqual(texUV, rect.zw - border));
		if (inside && !core)
			color = vec4(mix(color.rgb, outline.rgb, outline.a), 1.0);
	}
	gl_FragColor = color;
}
