attribute vec3 vPosition;
uniform vec4 vResolution;
uniform int iFrame;
uniform float iTime;
varying vec2 fragCoord;
varying vec2 iResolution;
varying vec2 iMouse;

void main (void)
{
	fragCoord = (vec2(0.5, 0.5) - vPosition.xy / 2.0) * vResolution.xy;
	iResolution = vResolution.xy;
	iMouse = iResolution / 2.0;
	gl_Position = vec4(vPosition,1.);
}
