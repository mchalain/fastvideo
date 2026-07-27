#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES vTexture;
varying vec2 texUV;
varying vec2 resolution;

void main() {
	float color = texture2D(vTexture, texUV).r;
	gl_FragColor = vec4(vec3(color), 1.0);
}
