float hash( vec2 p ) {
	float h = dot(p,vec2(127.1,311.7));
    return fract(sin(h)*43758.5453123);
}
float noise( in vec2 p ) {
    vec2 i = floor( p );
    vec2 f = fract( p );
	vec2 u = f*f*(3.0-2.0*f);
    return -1.0+2.0*mix( mix( hash( i + vec2(0.0,0.0) ),
                     hash( i + vec2(1.0,0.0) ), u.x),
                mix( hash( i + vec2(0.0,1.0) ),
                     hash( i + vec2(1.0,1.0) ), u.x), u.y);
}
#define PI 3.1415
void mainImage( out vec4 fragColor, in vec2 fragCoord ){
    float i = iTime;

    vec2 uv = fragCoord / iResolution.xy;
    vec4 c = texture(iChannel0,uv);

    vec2 p = mod(uv,vec2(1.0)/iResolution.xy);
    uv-=.5;
    float r = 0.0*.05;
    mat2 m = mat2(1,sin(uv.x*r+i),sin(uv.y*r+i),1);
    //vec4 n = texture(iChannel1,uv*m- p);
	vec4 n = vec4(noise(uv*m-p));

    uv+=.5;
    float d = length(fragCoord.xy-iResolution.xy*.5)*0.0*.0001;
    c.rgb = sin(cos(mod(c.rgb,n.rgb)*PI*2.0+d)*PI*2.0+i+2.0*d);

    fragColor = c;
}
