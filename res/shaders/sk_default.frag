#version 130

uniform sampler2D BaseMap;
uniform sampler2D NormalMap;
uniform sampler2D GlowMap;
uniform sampler2D HeightMap;
uniform sampler2D LightMask;
uniform sampler2D BacklightMap;
uniform sampler2D EnvironmentMap;
uniform samplerCube CubeMap;

uniform vec3 specColor;
uniform float specStrength;
uniform float specGlossiness;

uniform bool hasGlowMap;
uniform vec3 glowColor;
uniform float glowMult;

uniform float alpha;

uniform vec3 tintColor;

uniform bool hasHeightMap;
uniform vec2 uvScale;
uniform vec2 uvOffset;

uniform bool hasEmit;
uniform bool hasSoftlight;
uniform bool hasBacklight;
uniform bool hasRimlight;
uniform bool hasTintColor;
uniform bool hasCubeMap;
uniform bool hasEnvMask;

uniform float lightingEffect1;
uniform float lightingEffect2;

uniform float envReflection;

uniform mat4 worldMatrix;

in vec3 LightDir;
in vec3 ViewDir;

in vec4 A;
in vec4 C;
in vec4 D;

in mat3 tbnMatrix;

mat3 tbnMatrix_norm = mat3(normalize(tbnMatrix[0]), normalize(tbnMatrix[1]), normalize(tbnMatrix[2]));


vec3 tonemap(vec3 x, float y)
{
	float a = 0.15;
	float b = 0.50;
	float c = 0.10;
	float d = 0.20;
	float e = 0.02;
	float f = 0.30;

	vec3 z = x * (y * 4.22978723);
	z = (z * (a * z + b * c) + d * e) / (z * (a * z + b) + d * f) - e / f;
	return z / (y * 0.93333333);
}

vec3 toGrayscale(vec3 color)
{
	return vec3(dot(vec3(0.3, 0.59, 0.11), color));
}

void main( void )
{
	vec2 offset = gl_TexCoord[0].st * uvScale + uvOffset;

	vec3 E = normalize(ViewDir);
	
	if ( hasHeightMap ) {
		float height = texture2D( HeightMap, offset ).r;
		offset += normalize(ViewDir * tbnMatrix_norm).xy * (height * 0.08 - 0.04); 
	}

	vec4 baseMap = texture2D( BaseMap, offset );
	vec4 normalMap = texture2D( NormalMap, offset );
	vec4 glowMap = texture2D( GlowMap, offset );
	
	vec3 normal = normalize(tbnMatrix_norm * (normalMap.rgb * 2.0 - 1.0));
	if ( !gl_FrontFacing )
		normal *= -1.0;

	vec3 L = normalize(LightDir);
	vec3 R = reflect(-L, normal);
	vec3 H = normalize( L + E );
	
	float NdotL = max( dot(normal, L), 0.0 );
	float NdotH = max( dot(normal, H), 0.0 );
	float EdotN = max( dot(normal, E), 0.0 );
	float NdotNegL = max( dot(normal, -L), 0.0 );

	vec3 reflected = reflect( -E, normal );
	vec3 reflectedWS = vec3( worldMatrix * (gl_ModelViewMatrixInverse * vec4( reflected, 0.0 )) );


	vec4 color;
	vec3 albedo = baseMap.rgb * C.rgb;
	vec3 diffuse = A.rgb + (D.rgb * NdotL);


	// Environment
	if ( hasCubeMap ) {
		vec4 cube = textureCube( CubeMap, reflectedWS );
		cube.rgb *= envReflection;
		
		if ( hasEnvMask ) {
			vec4 env = texture2D( EnvironmentMap, offset );
			cube.rgb *= env.r;
		} else {
			cube.rgb *= normalMap.a;
		}
		

		albedo += cube.rgb;
	}

	// Emissive & Glow
	vec3 emissive = vec3(0.0);
	if ( hasEmit ) {
		emissive += glowColor * glowMult;
		
		if ( hasGlowMap ) {
			emissive *= glowMap.rgb;
		}
	}

	// Specular
	vec3 spec = clamp( specColor * specStrength * normalMap.a * pow(NdotH, specGlossiness), 0.0, 1.0 );
	spec *= D.rgb;

	vec3 backlight = vec3(0.0);
	if ( hasBacklight ) {
		backlight = texture2D( BacklightMap, offset ).rgb;
		backlight *= NdotNegL;
		
		emissive += backlight * D.rgb;
	}

	vec4 mask = vec4(0.0);
	if ( hasRimlight || hasSoftlight ) {
		mask = texture2D( LightMask, offset );
	}

	vec3 rim = vec3(0.0);
	if ( hasRimlight ) {
		rim = mask.rgb * pow(vec3((1.0 - EdotN)), vec3(lightingEffect2));
		rim *= smoothstep( -0.2, 1.0, dot(-L, E) );
		
		emissive += rim * D.rgb;
	}
	
	vec3 soft = vec3(0.0);
	if ( hasSoftlight ) {
		float wrap = (dot(normal, L) + lightingEffect1) / (1.0 + lightingEffect1);

		soft = max( wrap, 0.0 ) * mask.rgb * smoothstep( 1.0, 0.0, NdotL );
		soft *= sqrt( clamp( lightingEffect1, 0.0, 1.0 ) );
		
		emissive += soft * D.rgb;
	}
	
	if ( hasTintColor ) {
		albedo *= tintColor;
	}

	color.rgb = albedo * (diffuse + emissive) + spec;
	color.rgb = tonemap( color.rgb * D.a, A.a );
	color.a = C.a * baseMap.a;

	gl_FragColor = color;
	gl_FragColor.a *= alpha;
}
