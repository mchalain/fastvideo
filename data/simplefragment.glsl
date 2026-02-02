#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES vTexture;
varying vec2 texUV;
varying vec2 resolution;

void main() {
	gl_FragColor = texture2D(vTexture, texUV);
}
