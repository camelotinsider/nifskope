#version 130
#extension GL_ARB_shader_texture_lod : require

uniform sampler2D BaseMap;
uniform sampler2D NormalMap;
uniform sampler2D GlowMap;
uniform sampler2D ReflMap;
uniform sampler2D LightingMap;
uniform sampler2D GreyscaleMap;
uniform sampler2D EnvironmentMap;
uniform samplerCube CubeMap;

uniform vec4 solidColor;
uniform vec3 specColor;
uniform bool hasSpecular;
uniform float specStrength;
uniform float fresnelPower;

uniform float paletteScale;

uniform vec3 glowColor;
uniform float glowMult;

uniform float alpha;

uniform vec3 tintColor;

uniform vec2 uvScale;
uniform vec2 uvOffset;

uniform bool hasEmit;
uniform bool hasGlowMap;
uniform bool hasSoftlight;
uniform bool hasTintColor;
uniform bool hasCubeMap;
uniform bool hasSpecularMap;
uniform bool greyscaleColor;
uniform bool doubleSided;

uniform float subsurfaceRolloff;
uniform float rimPower;
uniform float backlightPower;

uniform float envReflection;

uniform bool isWireframe;
uniform bool isSkinned;
uniform mat4 worldMatrix;

in vec3 LightDir;
in vec3 ViewDir;

in vec4 A;
in vec4 C;
in vec4 D;

in mat3 btnMatrix;
in mat4 reflMatrix;

vec3 ViewDir_norm = normalize( ViewDir );
mat3 btnMatrix_norm = mat3( normalize( btnMatrix[0] ), normalize( btnMatrix[1] ), normalize( btnMatrix[2] ) );

#ifndef M_PI
	#define M_PI 3.1415926535897932384626433832795
#endif

#define FLT_EPSILON 1.192092896e-07F // smallest such that 1.0 + FLT_EPSILON != 1.0

float G1V(float NdotV, float k)
{
	return 1.0 / (NdotV * (1.0 - k) + k);
}

float LightingFuncGGX_REF0(float NdotL, float NdotH, float NdotV, float LdotH, float roughness, float F0)
{
	float alpha = roughness * roughness;
	float F, D, vis;
	// D
	float alphaSqr = alpha * alpha;
	float denom = NdotH * NdotH * (alphaSqr - 1.0) + 1.0;
	D = alphaSqr / (M_PI * denom * denom);
	// F
	float LdotH5 = pow(1.0 - LdotH, 5);
	F = F0 + (1.0 - F0) * LdotH5;

	// V
	float k = alpha/2.0;
	vis = G1V(NdotL, k) * G1V(NdotV, k);

	float specular = NdotL * D * F * vis;
	return specular;
}

vec3 fresnel_n(float NdotV, vec3 f0)
{
	vec4	a = vec4(-1.11050116, 2.79595384, -2.68545268, 1.0);
	float	f = ((a.r * NdotV + a.g) * NdotV + a.b) * NdotV + a.a;
	return f0 + ((vec3(1.0) - f0) * f * f);
}

float fresnel_w(float NdotV)
{
	vec4	a = vec4(-1.30214688, 3.32294874, -2.87825095, 1.0);
	float	f = ((a.r * NdotV + a.g) * NdotV + a.b) * NdotV + a.a;
	return f * f;
}

vec3 LightingFuncGGX_REF(float NdotL, float NdotH, float NdotV, float LdotH, float roughness, vec3 F0)
{
	float alpha = roughness * roughness;
	// D (GGX normal distribution)
	float alphaSqr = alpha * alpha;
	float denom = NdotH * NdotH * (alphaSqr - 1.0) + 1.0;
	float D = alphaSqr / (denom * denom);
	// no pi because BRDF -> lighting
	// F (Fresnel term)
	vec3 F = fresnel_n(LdotH, F0);
	// G (remapped hotness, see Unreal Shading)
	float	k = (alpha + 2 * roughness + 1) / 8.0;
	float	G = NdotL / (mix(NdotL, 1, k) * mix(NdotV, 1, k));

	return D * F * G / 4.0;
}

float OrenNayar(vec3 L, vec3 V, vec3 N, float roughness, float NdotL)
{
	//float NdotL = dot(N, L);
	float NdotV = dot(N, V);
	float LdotV = dot(L, V);

	float rough2 = roughness * roughness;

	float A = 1.0 - 0.5 * (rough2 / (rough2 + 0.57));
	float B = 0.45 * (rough2 / (rough2 + 0.09));

	float a = min( NdotV, NdotL );
	float b = max( NdotV, NdotL );
	b = (sign(b) == 0.0) ? FLT_EPSILON : sign(b) * max( 0.01, abs(b) ); // For fudging the smoothness of C
	float C = sqrt( (1.0 - a * a) * (1.0 - b * b) ) / b;

	float gamma = LdotV - NdotL * NdotV;
	float L1 = A + B * max( gamma, FLT_EPSILON ) * C;

	return L1 * max( NdotL, FLT_EPSILON );
}

float OrenNayarFull(vec3 L, vec3 V, vec3 N, float roughness, float NdotL0)
{
	float	NdotV = max(dot(N, V), FLT_EPSILON);

	float	angleVN = acos(NdotV);
	float	angleLN = acos(NdotL0);

	float	alpha = max(angleVN, angleLN);
	float	beta = min(angleVN, angleLN);
	float	gamma = 0.0;
	//gamma = dot(L, V) - NdotL0 * NdotV;
	if ( beta > 0.005 )
		gamma = dot(normalize(cross(L, N)), normalize(cross(V, N)));

	float roughnessSquared = roughness * roughness;
	float roughnessSquared9 = (roughnessSquared / (roughnessSquared + 0.09));

	// C1, C2, and C3
	float C1 = 1.0 - 0.5 * (roughnessSquared / (roughnessSquared + 0.33));
	float C2 = 0.45 * roughnessSquared9;

	if( gamma >= 0.0 ) {
		C2 *= sin(alpha);
	} else {
		C2 *= (sin(alpha) - pow((2.0 * beta) / M_PI, 3.0));
	}

	float powValue = (4.0 * alpha * beta) / (M_PI * M_PI);
	float C3 = 0.125 * roughnessSquared9 * powValue * powValue;

	// Avoid asymptote at pi/2
	float asym = M_PI / 2.0;
	float lim1 = asym + 0.005;
	float lim2 = asym - 0.005;

	float ab2 = (alpha + beta) / 2.0;

	if ( beta >= asym && beta < lim1 )
		beta = lim1;
	else if ( beta < asym && beta >= lim2 )
		beta = lim2;

	if ( ab2 >= asym && ab2 < lim1 )
		ab2 = lim1;
	else if ( ab2 < asym && ab2 >= lim2 )
		ab2 = lim2;

	// Reflection
	float A = gamma * C2 * tan(beta);
	float B = (1.0 - abs(gamma)) * C3 * tan(ab2);

	float L1 = NdotL0 * (C1 + A + B);

	// Interreflection
	float twoBetaPi = 2.0 * beta / M_PI;
	float L2 = 0.17 * NdotL0 * (roughnessSquared / (roughnessSquared + 0.13)) * (1.0 - gamma * twoBetaPi * twoBetaPi);

	return L1 + L2;
}

vec3 fresnelSchlickRoughness(float NdotV, vec3 F0, float roughness)
{
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
}

vec3 tonemap(vec3 x)
{
	float _A = 0.15;
	float _B = 0.50;
	float _C = 0.10;
	float _D = 0.20;
	float _E = 0.02;
	float _F = 0.30;

	return ((x*(_A*x+_C*_B)+_D*_E)/(x*(_A*x+_B)+_D*_F))-_E/_F;
}

vec4 colorLookup(float x, float y)
{
	return texture2D(GreyscaleMap, vec2(clamp(x, 0.0, 1.0), clamp(y, 0.0, 1.0)));
}

void main(void)
{
	if ( isWireframe ) {
		gl_FragColor = solidColor;
		return;
	}
	vec2 offset = gl_TexCoord[0].st * uvScale + uvOffset;

	vec4	baseMap = texture2D(BaseMap, offset);
	vec4	normalMap = texture2D(NormalMap, offset);
	vec4	lightingMap = vec4(0.25, 1.0, 0.0, 1.0);
	if ( hasSpecularMap )
		lightingMap = texture2D(LightingMap, offset);
	vec4	reflMap = texture2D(ReflMap, offset);
	vec4	glowMap = texture2D(GlowMap, offset);

	vec3 normal = normalMap.rgb;
	// Calculate missing blue channel
	normal.b = sqrt(max(1.0 - dot(normal.rg, normal.rg), 0.0));
	normal = normalize( btnMatrix_norm * normal );
	if ( !gl_FrontFacing && doubleSided )
		normal *= -1.0;

	vec3 L = normalize(LightDir);
	vec3 V = ViewDir_norm;
	vec3 R = reflect(-V, normal);
	vec3 H = normalize(L + V);

	float NdotL = dot(normal, L);
	float NdotL0 = max(NdotL, FLT_EPSILON);
	float NdotH = max(dot(normal, H), FLT_EPSILON);
	float NdotV = max(abs(dot(normal, V)), FLT_EPSILON);
	float VdotH = max(dot(V, H), FLT_EPSILON);
	float LdotH = max(dot(L, H), FLT_EPSILON);
	float NdotNegL = max(dot(normal, -L), FLT_EPSILON);

	vec3	reflectedWS = vec3(reflMatrix * (gl_ModelViewMatrixInverse * vec4(R, 0.0))) * vec3(1.0, 1.0, -1.0);
	vec3	normalWS = vec3(reflMatrix * (gl_ModelViewMatrixInverse * vec4(normal, 0.0))) * vec3(1.0, 1.0, -1.0);

	vec4 color;
	vec3 albedo = baseMap.rgb * C.rgb;
	if ( greyscaleColor ) {
		// work around incorrect format used by Fallout 76 grayscale textures
		float	g = sqrt(baseMap.g);
		vec4	luG = colorLookup(g * 1.13942692 - baseMap.g * 0.13942692, paletteScale * C.r);

		albedo = luG.rgb;
	}

	// Emissive
	vec3 emissive = vec3(0.0);
	if ( hasEmit ) {
		emissive += glowColor * glowMult;
		if ( hasGlowMap ) {
			emissive *= glowMap.rgb;
		}
	} else if ( hasGlowMap ) {
		emissive += glowMap.rgb * glowMult;
	}
	emissive *= lightingMap.a;

	vec3	f0 = max(reflMap.rgb, vec3(0.02));

	// Specular
	float	smoothness = lightingMap.r;
	float	roughness = max(1.0 - smoothness, 0.02);
	vec3	spec = LightingFuncGGX_REF(NdotL0, NdotH, NdotV, LdotH, roughness, f0) * D.rgb;

	// Diffuse
	float	diff = OrenNayarFull(L, V, normal, 1.0 - smoothness, NdotL0);
	vec3	diffuse = vec3(diff);

	// Environment
	vec3	refl = vec3(0.0);
	vec3	ambient = A.rgb;
	if ( hasCubeMap ) {
		float	m = (1.0 - smoothness) * ((1.0 - smoothness) * -4.0 + 10.0);
		refl = textureLod(CubeMap, reflectedWS, max(m, 0.0)).rgb;
		refl *= envReflection * specStrength;
		refl *= ambient;
		ambient *= textureLod(CubeMap, normalWS, 6.0).rgb;
	} else {
		ambient /= 15.0;
		refl = ambient;
	}
	vec4	envLUT = textureLod(EnvironmentMap, vec2(NdotV, 1.0 - smoothness), 0.0);
	vec3	f = mix(f0, vec3(1.0), envLUT.r);
	if (!hasSpecular) {
		albedo = max(albedo, reflMap.rgb);
		spec = vec3(0.0);
		f = vec3(0.0);
	}
	float	g = envLUT.g;
	float	ao = lightingMap.g;
	refl *= f * g * ao;
	albedo *= (vec3(1.0) - f);
	ao *= (envLUT.b * 0.5 + 0.625);

	//vec3 soft = vec3(0.0);
	//float wrap = NdotL;
	//if ( hasSoftlight || subsurfaceRolloff > 0.0 ) {
	//	wrap = (wrap + subsurfaceRolloff) / (1.0 + subsurfaceRolloff);
	//	soft = albedo * max(0.0, wrap) * smoothstep(1.0, 0.0, sqrt(diff));
	//
	//	diffuse += soft;
	//}

	//if ( hasTintColor ) {
	//	albedo *= tintColor;
	//}

	// Diffuse
	color.rgb = diffuse * albedo * D.rgb;
	// Ambient
	color.rgb += ambient * albedo * ao;
	// Specular
	color.rgb += spec;
	color.rgb += refl;

	// Emissive
	color.rgb += emissive;

	color.rgb = tonemap(color.rgb) / tonemap(vec3(1.0));
	color.a = C.a * baseMap.a;

	gl_FragColor = color;
	gl_FragColor.a *= alpha;
}
