#if GL_ES
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES vTexture;
#else
uniform sampler2D vTexture;
#endif
varying vec2 texUV;
varying vec2 resolution;

vec4 pixel(vec2 pos) {
	vec2 uv = vec2(pos.x / resolution.x, pos.y / resolution.y);
	return texture2D(vTexture, uv);
}

vec4 run(vec2 pos);
vec4 run_color(vec3 rgb);

void main() {
	vec2 pos = vec2(texUV.x * resolution.x, texUV.y * resolution.y);
	gl_FragColor = run_color(run(pos).rgb);
}
