vec3 coef = vec3(1.0, 0.5, 1.0);
const vec4 bayer = vec4(0.0, 1.0, 2.0, 3.0);

vec3 pixel(in vec2 uv)
{
	return texture(iChannel0, uv).rgb;
}

int mymod(in int A, in int B)
{
	return A - (A / B) * B;
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
	vec3 col = vec3(0.0);
	vec2 uv = fragCoord/iResolution.xy;
	vec2 scale = vec2(1.0) / iResolution.xy;
	int iCoordX = int(fragCoord.x);
	int iCoordY = int(fragCoord.y);
	int pos = mymod(iCoordX, 2) * 2 + mymod(iCoordY, 2);

	vec3 p0 = pixel(uv);
	vec3 p1;
	vec3 p2;
	vec3 p3;
	if (uv.x < 1.0)
		p1 = pixel(uv + scale * vec2(1.0,0.0));
	else
		p1 = pixel(uv + scale * vec2(-1.0,0.0));
	if (uv.y < 1.0)
		p2 = pixel(uv + scale * vec2(0.0,1.0));
	else
		p2 = pixel(uv + scale * vec2(0.0,-1.0));
	if (uv.x < 1.0 && uv.y < 1.0)
		p3 = pixel(uv + scale * vec2(1.0,1.0));
	else
		p3 = pixel(uv + scale * vec2(-1.0,-1.0));

	if (pos == int(bayer.b))
			col = vec3(p3.r * coef.r, (p1.r + p2.r) * coef.g, p0.r * coef.b);
	if (pos == int(bayer.g))
			col = vec3(p1.r * coef.r, (p0.r + p3.r) * coef.g, p2.r * coef.b);
	if (pos == int(bayer.a))
			col = vec3(p2.r * coef.r, (p0.r + p3.r) * coef.g, p1.r * coef.b);
	if (pos == int(bayer.r))
			col = vec3(p0.r * coef.r, (p1.r + p2.r) * coef.g, p3.r * coef.b);
	fragColor = vec4(col,1.0);
}
