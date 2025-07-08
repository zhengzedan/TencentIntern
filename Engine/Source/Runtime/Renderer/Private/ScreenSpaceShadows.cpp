#include "ShadowRendering.h"
#include "PrimitiveViewRelevance.h"
#include "DepthRendering.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "HairStrands/HairStrandsRendering.h"
#include "RenderCore.h"
#include "MobileBasePassRendering.h"

#include "ScreenSpaceShadows.h"

/*
SSS Compute Shader
*/
const int32 GScreenSpaceShadowsTileSizeX = 8;
const int32 GScreenSpaceShadowsTileSizeY = 8;

class FScreenSpaceShadowsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenSpaceShadowsCS);
	SHADER_USE_PARAMETER_STRUCT(FScreenSpaceShadowsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RWShadowFactors)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		//SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
		//SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, StencilTexture)
		SHADER_PARAMETER(FVector4, LightPositionOrDirection)
		SHADER_PARAMETER(float, ContactShadowLength)
		SHADER_PARAMETER(uint32, bContactShadowLengthInWS)
		SHADER_PARAMETER(float, ContactShadowCastingIntensity)
		SHADER_PARAMETER(float, ContactShadowNonCastingIntensity)
		SHADER_PARAMETER(FIntRect, ScissorRectMinAndSize)
		SHADER_PARAMETER(uint32, DownsampleFactor)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), GScreenSpaceShadowsTileSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), GScreenSpaceShadowsTileSizeY);
		OutEnvironment.SetDefine(TEXT("FORCE_DEPTH_TEXTURE_READS"), 1);
		OutEnvironment.SetDefine(TEXT("PLATFORM_SUPPORTS_TYPED_UAV_LOAD"), (int32)RHISupports4ComponentUAVReadWrite(Parameters.Platform));
	}

	
};

IMPLEMENT_GLOBAL_SHADER(FScreenSpaceShadowsCS, "/Engine/Private/ScreenSpaceShadows.usf", "Main", SF_Compute);

void RenderScreenSpaceShadows(
	FRDGBuilder& GraphBuilder,
	FSceneTextureParameters& SceneTextureParameters,
	FViewInfo& View)
{

}