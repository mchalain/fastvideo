#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES vTexture;
uniform sampler2D lTexture;
varying vec2 texUV;
varying vec2 resolution;

void main() {
	vec4 base = texture2D(lTexture, texUV);
	vec4 camera = texture2D(vTexture, texUV);
	gl_FragColor = base * 0.5 + camera * 0.5;
}
