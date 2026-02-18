#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES vTexture;
varying vec2 texUV;
varying vec2 resolution;

const vec3 coef = vec3(2.3, 0.5, 1.8);
const vec4 bayer = vec4(0.0, 1.0, 2.0, 3.0);

int mymod(in int A, in int B)
{
	return A - (A / B) * B;
}

void main() {
	vec3 col = vec3(0.0);
	vec3 p0 = texture2D(vTexture, texUV).rgb;
	vec3 p1 = texture2D(vTexture, texUV + vec2(1.0,0.0) / resolution.xy).rgb;
	vec3 p2 = texture2D(vTexture, texUV + vec2(0.0,1.0) / resolution.xy).rgb;
	vec3 p3 = texture2D(vTexture, texUV + vec2(1.0,1.0) / resolution.xy).rgb;
	int pos = mymod(int(texUV.x * resolution.x), 2) * 2 + mymod(int(texUV.y * resolution.y), 2);

	if (pos == int(bayer.r))
			col = vec3(p3.r * coef.r, (p1.r + p2.r) * coef.g, p0.r * coef.b);
	if (pos == int(bayer.g))
			col = vec3(p1.r * coef.r, (p0.r + p3.r) * coef.g, p2.r * coef.b);
	if (pos == int(bayer.a))
			col = vec3(p0.r * coef.r, (p1.r + p2.r) * coef.g, p3.r * coef.b);
	if (pos == int(bayer.b))
			col = vec3(p2.r * coef.r, (p0.r + p3.r) * coef.g, p1.r * coef.b);
	gl_FragColor = vec4(col, 1.0);
}
