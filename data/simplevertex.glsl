attribute vec3 vPosition;
uniform vec4 vResolution;
varying vec2 texUV;
varying vec2 resolution;

void main (void)
{
	texUV = (vec2(0.5, 0.5) + vPosition.xy / 2.0);
	resolution = vResolution.zw;
	gl_Position = vec4(vPosition,1.);
}
