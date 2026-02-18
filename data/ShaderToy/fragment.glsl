#if GL_ES
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES iChannel0;
#else
uniform sampler2D iChannel0;
#endif
varying vec2 fragCoord;
varying vec2 iResolution;
uniform float iTime;
uniform float fFrame;
int iFrame;
varying vec2 iMouse;

#if GL_ES && __VERSION__ < 330
vec4 texture(samplerExternalOES tex, vec2 uv)
{
	return texture2D(tex, uv);
}
#endif

void mainImage(out vec4 fragColor, in vec2 fragCoord);

void main() {
	iFrame = int(fFrame);
	mainImage(gl_FragColor, fragCoord);
}
