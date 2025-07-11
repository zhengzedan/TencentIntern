// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ShadowRendering.cpp: Shadow rendering implementation
=============================================================================*/

#include "ShadowRendering.h"
#include "PrimitiveViewRelevance.h"
#include "DepthRendering.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "LightPropagationVolume.h"
#include "ScenePrivate.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "HairStrands/HairStrandsRendering.h"
#include "PixelShaderUtils.h"
#include "SceneTextureParameters.h"

#include "ScreenSpaceShadows.h"
#include "RenderGraphUtils.h"
#include "PostProcess/PostProcessWeightedSampleSum.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Directional light
static TAutoConsoleVariable<float> CVarCSMShadowDepthBias(
	TEXT("r.Shadow.CSMDepthBias"),
	10.0f,
	TEXT("Constant depth bias used by CSM"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarCSMShadowSlopeScaleDepthBias(
	TEXT("r.Shadow.CSMSlopeScaleDepthBias"),
	3.0f,
	TEXT("Slope scale depth bias used by CSM"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPerObjectDirectionalShadowDepthBias(
	TEXT("r.Shadow.PerObjectDirectionalDepthBias"),
	10.0f,
	TEXT("Constant depth bias used by per-object shadows from directional lights\n")
	TEXT("Lower values give better shadow contact, but increase self-shadowing artifacts"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPerObjectDirectionalShadowSlopeScaleDepthBias(
	TEXT("r.Shadow.PerObjectDirectionalSlopeDepthBias"),
	3.0f,
	TEXT("Slope scale depth bias used by per-object shadows from directional lights\n")
	TEXT("Lower values give better shadow contact, but increase self-shadowing artifacts"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarCSMSplitPenumbraScale(
	TEXT("r.Shadow.CSMSplitPenumbraScale"),
	0.5f,
	TEXT("Scale applied to the penumbra size of Cascaded Shadow Map splits, useful for minimizing the transition between splits"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarCSMDepthBoundsTest(
	TEXT("r.Shadow.CSMDepthBoundsTest"),
	1,
	TEXT("Whether to use depth bounds tests rather than stencil tests for the CSM bounds"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarShadowTransitionScale(
	TEXT("r.Shadow.TransitionScale"),
	60.0f,
	TEXT("This controls the 'fade in' region between a caster and where his shadow shows up.  Larger values make a smaller region which will have more self shadowing artifacts"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarCSMShadowReceiverBias(
	TEXT("r.Shadow.CSMReceiverBias"),
	0.9f,
	TEXT("Receiver bias used by CSM. Value between 0 and 1."),
	ECVF_RenderThreadSafe);


///////////////////////////////////////////////////////////////////////////////////////////////////
// Point light
static TAutoConsoleVariable<float> CVarPointLightShadowDepthBias(
	TEXT("r.Shadow.PointLightDepthBias"),
	0.02f,
	TEXT("Depth bias that is applied in the depth pass for shadows from point lights. (0.03 avoids peter paning but has some shadow acne)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarPointLightShadowSlopeScaleDepthBias(
	TEXT("r.Shadow.PointLightSlopeScaleDepthBias"),
	3.0f,
	TEXT("Slope scale depth bias that is applied in the depth pass for shadows from point lights"),
	ECVF_RenderThreadSafe);


///////////////////////////////////////////////////////////////////////////////////////////////////
// Rect light
static TAutoConsoleVariable<float> CVarRectLightShadowDepthBias(
	TEXT("r.Shadow.RectLightDepthBias"),
	0.025f,
	TEXT("Depth bias that is applied in the depth pass for shadows from rect lights. (0.03 avoids peter paning but has some shadow acne)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRectLightShadowSlopeScaleDepthBias(
	TEXT("r.Shadow.RectLightSlopeScaleDepthBias"),
	2.5f,
	TEXT("Slope scale depth bias that is applied in the depth pass for shadows from rect lights"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRectLightShadowReceiverBias(
	TEXT("r.Shadow.RectLightReceiverBias"),
	0.3f,
	TEXT("Receiver bias used by rect light. Value between 0 and 1."),
	ECVF_RenderThreadSafe);

///////////////////////////////////////////////////////////////////////////////////////////////////
// Spot light
static TAutoConsoleVariable<float> CVarSpotLightShadowDepthBias(
	TEXT("r.Shadow.SpotLightDepthBias"),
	3.0f,
	TEXT("Depth bias that is applied in the depth pass for per object projected shadows from spot lights"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSpotLightShadowSlopeScaleDepthBias(
	TEXT("r.Shadow.SpotLightSlopeDepthBias"),
	3.0f,
	TEXT("Slope scale depth bias that is applied in the depth pass for per object projected shadows from spot lights"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSpotLightShadowTransitionScale(
	TEXT("r.Shadow.SpotLightTransitionScale"),
	60.0f,
	TEXT("Transition scale for spotlights"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSpotLightShadowReceiverBias(
	TEXT("r.Shadow.SpotLightReceiverBias"),
	0.5f,
	TEXT("Receiver bias used by spotlights. Value between 0 and 1."),
	ECVF_RenderThreadSafe);


///////////////////////////////////////////////////////////////////////////////////////////////////
// General
static TAutoConsoleVariable<int32> CVarEnableModulatedSelfShadow(
	TEXT("r.Shadow.EnableModulatedSelfShadow"),
	0,
	TEXT("Allows modulated shadows to affect the shadow caster. (mobile only)"),
	ECVF_RenderThreadSafe);

static int GStencilOptimization = 1;
static FAutoConsoleVariableRef CVarStencilOptimization(
	TEXT("r.Shadow.StencilOptimization"),
	GStencilOptimization,
	TEXT("Removes stencil clears between shadow projections by zeroing the stencil during testing"),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarFilterMethod(
	TEXT("r.Shadow.FilterMethod"),
	0,
	TEXT("Chooses the shadow filtering method.\n")
	TEXT(" 0: Uniform PCF (default)\n")
	TEXT(" 1: PCSS (experimental)\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMaxSoftKernelSize(
	TEXT("r.Shadow.MaxSoftKernelSize"),
	40,
	TEXT("Mazimum size of the softening kernels in pixels."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarShadowMaxSlopeScaleDepthBias(
	TEXT("r.Shadow.ShadowMaxSlopeScaleDepthBias"),
	1.0f,
	TEXT("Max Slope depth bias used for shadows for all lights\n")
	TEXT("Higher values give better self-shadowing, but increase self-shadowing artifacts"),
	ECVF_RenderThreadSafe);

DEFINE_GPU_STAT(ShadowProjection);

// 0:off, 1:low, 2:med, 3:high, 4:very high, 5:max
uint32 GetShadowQuality()
{
	static const auto ICVarQuality = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ShadowQuality"));

	int Ret = ICVarQuality->GetValueOnRenderThread();

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	static const auto ICVarLimit = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.LimitRenderingFeatures"));
	if(ICVarLimit)
	{
		int32 Limit = ICVarLimit->GetValueOnRenderThread();

		if(Limit > 2)
		{
			Ret = 0;
		}
	}
#endif

	return FMath::Clamp(Ret, 0, 5);
}

/*-----------------------------------------------------------------------------
	FShadowVolumeBoundProjectionVS
-----------------------------------------------------------------------------*/

void FShadowVolumeBoundProjectionVS::SetParameters(FRHICommandList& RHICmdList, const FSceneView& View, const FProjectedShadowInfo* ShadowInfo)
{
	FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, GetVertexShader(),View.ViewUniformBuffer);
	
	if(ShadowInfo->IsWholeSceneDirectionalShadow())
	{
		// Calculate bounding geometry transform for whole scene directional shadow.
		// Use a pair of pre-transformed planes for stenciling.
		StencilingGeometryParameters.Set(RHICmdList, this, FVector4(0,0,0,1));
	}
	else if(ShadowInfo->IsWholeScenePointLightShadow())
	{
		// Handle stenciling sphere for point light.
		StencilingGeometryParameters.Set(RHICmdList, this, View, ShadowInfo->LightSceneInfo);
	}
	else
	{
		// Other bounding geometry types are pre-transformed.
		StencilingGeometryParameters.Set(RHICmdList, this, FVector4(0,0,0,1));
	}
}

IMPLEMENT_SHADER_TYPE(,FShadowProjectionNoTransformVS,TEXT("/Engine/Private/ShadowProjectionVertexShader.usf"),TEXT("Main"),SF_Vertex);

IMPLEMENT_SHADER_TYPE(,FShadowVolumeBoundProjectionVS,TEXT("/Engine/Private/ShadowProjectionVertexShader.usf"),TEXT("Main"),SF_Vertex);

IMPLEMENT_SHADER_TYPE(, FScreenSpaceShadowsProjectionVS, TEXT("/Engine/Private/ScreenSpaceShadowsVertexShader.usf"), TEXT("Main"), SF_Vertex);

IMPLEMENT_SHADER_TYPE(,FScreenSpaceShadowsProjectionPS, TEXT("/Engine/Private/ScreenSpaceShadowsPixelShader.usf"),TEXT("Main"),SF_Pixel);

/**
 * Implementations for TShadowProjectionPS.  
 */
#if !UE_BUILD_DOCS
#define IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(Quality,UseFadePlane,UseTransmission, SupportSubPixel) \
	typedef TShadowProjectionPS<Quality, UseFadePlane, false, UseTransmission, SupportSubPixel> FShadowProjectionPS##Quality##UseFadePlane##UseTransmission##SupportSubPixel; \
	IMPLEMENT_SHADER_TYPE(template<>,FShadowProjectionPS##Quality##UseFadePlane##UseTransmission##SupportSubPixel,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);

// Projection shaders without the distance fade, with different quality levels.
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(1,false,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(2,false,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(3,false,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(4,false,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5,false,false,false);

IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(1,false,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(2,false,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(3,false,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(4,false,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5,false,true,false);

// Projection shaders with the distance fade, with different quality levels.
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(1,true,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(2,true,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(3,true,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(4,true,false,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5,true,false,false);

IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(1,true,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(2,true,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(3,true,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(4,true,true,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5,true,true,false);

// Projection shaders without the distance fade, without transmission, with Sub-PixelSupport with different quality levels
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(1, false, false, true);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(2, false, false, true);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(3, false, false, true);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(4, false, false, true);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5, false, false, true);
													   
#undef IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER
#endif

// Implement a pixel shader for rendering modulated shadow projections.
IMPLEMENT_SHADER_TYPE(template<>, TModulatedShadowProjection<1>, TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"), TEXT("Main"), SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>, TModulatedShadowProjection<2>, TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"), TEXT("Main"), SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>, TModulatedShadowProjection<3>, TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"), TEXT("Main"), SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>, TModulatedShadowProjection<4>, TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"), TEXT("Main"), SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>, TModulatedShadowProjection<5>, TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"), TEXT("Main"), SF_Pixel);

// with different quality levels
IMPLEMENT_SHADER_TYPE(template<>,TShadowProjectionFromTranslucencyPS<1>,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>,TShadowProjectionFromTranslucencyPS<2>,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>,TShadowProjectionFromTranslucencyPS<3>,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>,TShadowProjectionFromTranslucencyPS<4>,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>,TShadowProjectionFromTranslucencyPS<5>,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);

// Implement a pixel shader for rendering one pass point light shadows with different quality levels
#define IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(Quality,UseTransmission) \
	typedef TOnePassPointShadowProjectionPS<Quality,  UseTransmission> FOnePassPointShadowProjectionPS##Quality##UseTransmission; \
	IMPLEMENT_SHADER_TYPE(template<>,FOnePassPointShadowProjectionPS##Quality##UseTransmission,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("MainOnePassPointLightPS"),SF_Pixel);

IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(1, false);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(2, false);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(3, false);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(4, false);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(5, false);

IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(1, true);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(2, true);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(3, true);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(4, true);
IMPLEMENT_ONEPASS_POINT_SHADOW_PROJECTION_PIXEL_SHADER(5, true);

// Implements a pixel shader for directional light PCSS.
#define IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(Quality,UseFadePlane) \
	typedef TDirectionalPercentageCloserShadowProjectionPS<Quality, UseFadePlane> TDirectionalPercentageCloserShadowProjectionPS##Quality##UseFadePlane; \
	IMPLEMENT_SHADER_TYPE(template<>,TDirectionalPercentageCloserShadowProjectionPS##Quality##UseFadePlane,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5,false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5,true);
#undef IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER

// Implements a pixel shader for spot light PCSS.
#define IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(Quality,UseFadePlane) \
	typedef TSpotPercentageCloserShadowProjectionPS<Quality, UseFadePlane> TSpotPercentageCloserShadowProjectionPS##Quality##UseFadePlane; \
	IMPLEMENT_SHADER_TYPE(template<>,TSpotPercentageCloserShadowProjectionPS##Quality##UseFadePlane,TEXT("/Engine/Private/ShadowProjectionPixelShader.usf"),TEXT("Main"),SF_Pixel);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5, false);
IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER(5, true);
#undef IMPLEMENT_SHADOW_PROJECTION_PIXEL_SHADER

static void GetShadowProjectionShaders(
	int32 Quality, const FViewInfo& View, const FProjectedShadowInfo* ShadowInfo, bool bMobileModulatedProjections, bool bSubPixelSupport,
	FShadowProjectionVertexShaderInterface** OutShadowProjVS, FShadowProjectionPixelShaderInterface** OutShadowProjPS)
{
	check(!*OutShadowProjVS);
	check(!*OutShadowProjPS);

	if (bSubPixelSupport)
	{
		check(!bMobileModulatedProjections);

		if (ShadowInfo->IsWholeSceneDirectionalShadow())
			*OutShadowProjVS = View.ShaderMap->GetShader<FShadowProjectionNoTransformVS>();
		else
			*OutShadowProjVS = View.ShaderMap->GetShader<FShadowVolumeBoundProjectionVS>();

		switch (Quality)
		{
		case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<1, false, false, false, true> >(); break;
		case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<2, false, false, false, true> >(); break;
		case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<3, false, false, false, true> >(); break;
		case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<4, false, false, false, true> >(); break;
		case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<5, false, false, false, true> >(); break;
		default:
			check(0);
		}
		return;
	}

	if (ShadowInfo->bTranslucentShadow)
	{
		*OutShadowProjVS = View.ShaderMap->GetShader<FShadowVolumeBoundProjectionVS>();

		switch (Quality)
		{
		case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionFromTranslucencyPS<1> >(); break;
		case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionFromTranslucencyPS<2> >(); break;
		case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionFromTranslucencyPS<3> >(); break;
		case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionFromTranslucencyPS<4> >(); break;
		case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionFromTranslucencyPS<5> >(); break;
		default:
			check(0);
		}
	}
	else if (ShadowInfo->IsWholeSceneDirectionalShadow())
	{
		*OutShadowProjVS = View.ShaderMap->GetShader<FShadowProjectionNoTransformVS>();

		if (CVarFilterMethod.GetValueOnRenderThread() == 1)
		{
			if (ShadowInfo->CascadeSettings.FadePlaneLength > 0)
				*OutShadowProjPS = View.ShaderMap->GetShader<TDirectionalPercentageCloserShadowProjectionPS<5, true> >();
			else
				*OutShadowProjPS = View.ShaderMap->GetShader<TDirectionalPercentageCloserShadowProjectionPS<5, false> >();
		}
		else if (ShadowInfo->CascadeSettings.FadePlaneLength > 0)
		{
			if (ShadowInfo->bTransmission)
			{
				switch (Quality)
				{
				case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<1, true, false, true> >(); break;
				case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<2, true, false, true> >(); break;
				case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<3, true, false, true> >(); break;
				case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<4, true, false, true> >(); break;
				case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<5, true, false, true> >(); break;
				default:
					check(0);
				}
			}
			else
			{
				switch (Quality)
				{
				case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<1, true> >(); break;
				case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<2, true> >(); break;
				case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<3, true> >(); break;
				case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<4, true> >(); break;
				case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<5, true> >(); break;
				default:
					check(0);
				}
			}
		}
		else
		{
			if (ShadowInfo->bTransmission)
			{
				switch (Quality)
				{
				case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<1, false, false, true> >(); break;
				case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<2, false, false, true> >(); break;
				case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<3, false, false, true> >(); break;
				case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<4, false, false, true> >(); break;
				case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<5, false, false, true> >(); break;
				default:
					check(0);
				}
			}
			else
			{ 
				switch (Quality)
				{
				case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<1, false> >(); break;
				case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<2, false> >(); break;
				case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<3, false> >(); break;
				case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<4, false> >(); break;
				case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<5, false> >(); break;
				default:
					check(0);
				}
			}
		}
	}
	else
	{
		*OutShadowProjVS = View.ShaderMap->GetShader<FShadowVolumeBoundProjectionVS>();

		if(bMobileModulatedProjections)
		{
			switch (Quality)
			{
			case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TModulatedShadowProjection<1> >(); break;
			case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TModulatedShadowProjection<2> >(); break;
			case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TModulatedShadowProjection<3> >(); break;
			case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TModulatedShadowProjection<4> >(); break;
			case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TModulatedShadowProjection<5> >(); break;
			default:
				check(0);
			}
		}
		else if (ShadowInfo->bTransmission)
		{
			switch (Quality)
			{
			case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<1, false, false, true> >(); break;
			case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<2, false, false, true> >(); break;
			case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<3, false, false, true> >(); break;
			case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<4, false, false, true> >(); break;
			case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<5, false, false, true> >(); break;
			default:
				check(0);
			}
		}
		else
		{
			if (CVarFilterMethod.GetValueOnRenderThread() == 1 && ShadowInfo->GetLightSceneInfo().Proxy->GetLightType() == LightType_Spot)
			{
				*OutShadowProjPS = View.ShaderMap->GetShader<TSpotPercentageCloserShadowProjectionPS<5, false> >();
			}
			else
			{
				switch (Quality)
				{
				case 1: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<1, false> >(); break;
				case 2: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<2, false> >(); break;
				case 3: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<3, false> >(); break;
				case 4: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<4, false> >(); break;
				case 5: *OutShadowProjPS = View.ShaderMap->GetShader<TShadowProjectionPS<5, false> >(); break;
				default:
					check(0);
				}
			}
		}
	}

	check(*OutShadowProjVS);
	check(*OutShadowProjPS);
}

void FProjectedShadowInfo::SetBlendStateForProjection(
	FGraphicsPipelineStateInitializer& GraphicsPSOInit,
	int32 ShadowMapChannel, 
	bool bIsWholeSceneDirectionalShadow,
	bool bUseFadePlane,
	bool bProjectingForForwardShading, 
	bool bMobileModulatedProjections)
{
	// With forward shading we are packing shadowing for all 4 possible stationary lights affecting each pixel into channels of the same texture, based on assigned shadowmap channels.
	// With deferred shading we have 4 channels for each light.  
	//	* CSM and per-object shadows are kept in separate channels to allow fading CSM out to precomputed shadowing while keeping per-object shadows past the fade distance.
	//	* Subsurface shadowing requires an extra channel for each

	if (bProjectingForForwardShading)
	{
		FRHIBlendState* BlendState = nullptr;

		if (bUseFadePlane)
		{
			if (ShadowMapChannel == 0)
			{
				// alpha is used to fade between cascades
				BlendState = TStaticBlendState<CW_RED, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
			}
			else if (ShadowMapChannel == 1)
			{
				BlendState = TStaticBlendState<CW_GREEN, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
			}
			else if (ShadowMapChannel == 2)
			{
				BlendState = TStaticBlendState<CW_BLUE, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
			}
			else if (ShadowMapChannel == 3)
			{
				BlendState = TStaticBlendState<CW_ALPHA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
			}
		}
		else
		{
			if (ShadowMapChannel == 0)
			{
				BlendState = TStaticBlendState<CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
			}
			else if (ShadowMapChannel == 1)
			{
				BlendState = TStaticBlendState<CW_GREEN, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
			}
			else if (ShadowMapChannel == 2)
			{
				BlendState = TStaticBlendState<CW_BLUE, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
			}
			else if (ShadowMapChannel == 3)
			{
				BlendState = TStaticBlendState<CW_ALPHA, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
			}
		}

		checkf(BlendState, TEXT("Only shadows whose stationary lights have a valid ShadowMapChannel can be projected with forward shading"));
		GraphicsPSOInit.BlendState = BlendState;
	}
	else
	{
		// Light Attenuation channel assignment:
		//  R:     WholeSceneShadows, non SSS
		//  G:     WholeSceneShadows,     SSS
		//  B: non WholeSceneShadows, non SSS
		//  A: non WholeSceneShadows,     SSS
		//
		// SSS: SubsurfaceScattering materials
		// non SSS: shadow for opaque materials
		// WholeSceneShadows: directional light CSM
		// non WholeSceneShadows: spotlight, per object shadows, translucency lighting, omni-directional lights

		if (bIsWholeSceneDirectionalShadow)
		{
			// Note: blend logic has to match ordering in FCompareFProjectedShadowInfoBySplitIndex.  For example the fade plane blend mode requires that shadow to be rendered first.
			// use R and G in Light Attenuation
			if (bUseFadePlane)
			{
				// alpha is used to fade between cascades, we don't don't need to do BO_Min as we leave B and A untouched which has translucency shadow
				GraphicsPSOInit.BlendState = TStaticBlendState<CW_RG, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
			}
			else
			{
				// first cascade rendered doesn't require fading (CO_Min is needed to combine multiple shadow passes)
				// RTDF shadows: CO_Min is needed to combine with far shadows which overlap the same depth range
				GraphicsPSOInit.BlendState = TStaticBlendState<CW_RG, BO_Min, BF_One, BF_One>::GetRHI();
			}
		}
		else
		{
			if (bMobileModulatedProjections)
			{
				bool bEncodedHDR = GetMobileHDRMode() == EMobileHDRMode::EnabledRGBE;
				if (bEncodedHDR)
				{
					GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
				}
				else
				{
					// Color modulate shadows, ignore alpha.
					GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_Zero, BF_SourceColor, BO_Add, BF_Zero, BF_One>::GetRHI();
				}
			}
			else
			{
				// use B and A in Light Attenuation
				// CO_Min is needed to combine multiple shadow passes
				GraphicsPSOInit.BlendState = TStaticBlendState<CW_BA, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
			}
		}
	}
}

void FProjectedShadowInfo::SetBlendStateForProjection(FGraphicsPipelineStateInitializer& GraphicsPSOInit, bool bProjectingForForwardShading, bool bMobileModulatedProjections) const
{
	SetBlendStateForProjection(
		GraphicsPSOInit,
		GetLightSceneInfo().GetDynamicShadowMapChannel(), 
		IsWholeSceneDirectionalShadow(),
		CascadeSettings.FadePlaneLength > 0 && !bRayTracedDistanceField,
		bProjectingForForwardShading, 
		bMobileModulatedProjections);
}

void FProjectedShadowInfo::SetupFrustumForProjection(const FViewInfo* View, TArray<FVector4, TInlineAllocator<8>>& OutFrustumVertices, bool& bOutCameraInsideShadowFrustum) const
{
	bOutCameraInsideShadowFrustum = true;

	// Calculate whether the camera is inside the shadow frustum, or the near plane is potentially intersecting the frustum.
	if (!IsWholeSceneDirectionalShadow())
	{
		OutFrustumVertices.AddUninitialized(8);

		// The shadow transforms and view transforms are relative to different origins, so the world coordinates need to be translated.
		const FVector PreShadowToPreViewTranslation(View->ViewMatrices.GetPreViewTranslation() - PreShadowTranslation);

		// fill out the frustum vertices (this is only needed in the non-whole scene case)
		for(uint32 vZ = 0;vZ < 2;vZ++)
		{
			for(uint32 vY = 0;vY < 2;vY++)
			{
				for(uint32 vX = 0;vX < 2;vX++)
				{
					const FVector4 UnprojectedVertex = InvReceiverMatrix.TransformFVector4(
						FVector4(
							(vX ? -1.0f : 1.0f),
							(vY ? -1.0f : 1.0f),
							(vZ ?  0.0f : 1.0f),
							1.0f
							)
						);
					const FVector ProjectedVertex = UnprojectedVertex / UnprojectedVertex.W + PreShadowToPreViewTranslation;
					OutFrustumVertices[GetCubeVertexIndex(vX,vY,vZ)] = FVector4(ProjectedVertex, 0);
				}
			}
		}

		const FVector ShadowViewOrigin = View->ViewMatrices.GetViewOrigin();
		const FVector ShadowPreViewTranslation = View->ViewMatrices.GetPreViewTranslation();

		const FVector FrontTopRight		= OutFrustumVertices[GetCubeVertexIndex(0,0,1)] - ShadowPreViewTranslation;
		const FVector FrontTopLeft		= OutFrustumVertices[GetCubeVertexIndex(1,0,1)] - ShadowPreViewTranslation;
		const FVector FrontBottomLeft	= OutFrustumVertices[GetCubeVertexIndex(1,1,1)] - ShadowPreViewTranslation;
		const FVector FrontBottomRight	= OutFrustumVertices[GetCubeVertexIndex(0,1,1)] - ShadowPreViewTranslation;
		const FVector BackTopRight		= OutFrustumVertices[GetCubeVertexIndex(0,0,0)] - ShadowPreViewTranslation;
		const FVector BackTopLeft		= OutFrustumVertices[GetCubeVertexIndex(1,0,0)] - ShadowPreViewTranslation;
		const FVector BackBottomLeft	= OutFrustumVertices[GetCubeVertexIndex(1,1,0)] - ShadowPreViewTranslation;
		const FVector BackBottomRight	= OutFrustumVertices[GetCubeVertexIndex(0,1,0)] - ShadowPreViewTranslation;

		const FPlane Front(FrontTopRight, FrontTopLeft, FrontBottomLeft);
		const float FrontDistance = Front.PlaneDot(ShadowViewOrigin);

		const FPlane Right(BackBottomRight, BackTopRight, FrontTopRight);
		const float RightDistance = Right.PlaneDot(ShadowViewOrigin);

		const FPlane Back(BackTopLeft, BackTopRight, BackBottomRight);
		const float BackDistance = Back.PlaneDot(ShadowViewOrigin);

		const FPlane Left(FrontTopLeft, BackTopLeft, BackBottomLeft);
		const float LeftDistance = Left.PlaneDot(ShadowViewOrigin);

		const FPlane Top(BackTopRight, BackTopLeft, FrontTopLeft);
		const float TopDistance = Top.PlaneDot(ShadowViewOrigin);

		const FPlane Bottom(BackBottomLeft, BackBottomRight, FrontBottomLeft);
		const float BottomDistance = Bottom.PlaneDot(ShadowViewOrigin);

		// Use a distance threshold to treat the case where the near plane is intersecting the frustum as the camera being inside
		// The near plane handling is not exact since it just needs to be conservative about saying the camera is outside the frustum
		const float DistanceThreshold = -View->NearClippingDistance * 3.0f;

		bOutCameraInsideShadowFrustum = 
			FrontDistance > DistanceThreshold && 
			RightDistance > DistanceThreshold && 
			BackDistance > DistanceThreshold && 
			LeftDistance > DistanceThreshold && 
			TopDistance > DistanceThreshold && 
			BottomDistance > DistanceThreshold;
	}
}

void FProjectedShadowInfo::SetupProjectionStencilMask(
	FRHICommandListImmediate& RHICmdList, 
	const FViewInfo* View, 
	int32 ViewIndex, 
	const FSceneRenderer* SceneRender,
	const TArray<FVector4, TInlineAllocator<8>>& FrustumVertices,
	bool bMobileModulatedProjections, 
	bool bCameraInsideShadowFrustum) const
{
	FMeshPassProcessorRenderState DrawRenderState(*View);

	// Depth test wo/ writes, no color writing.
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());

	const bool bDynamicInstancing = IsDynamicInstancingEnabled(View->FeatureLevel);

	// If this is a preshadow, mask the projection by the receiver primitives.
	if (bPreShadow || bSelfShadowOnly)
	{
		SCOPED_DRAW_EVENTF(RHICmdList, EventMaskSubjects, TEXT("Stencil Mask Subjects"));

		// If instanced stereo is enabled, we need to render each view of the stereo pair using the instanced stereo transform to avoid bias issues.
		// TODO: Support instanced stereo properly in the projection stenciling pass.
		const bool bIsInstancedStereoEmulated = View->bIsInstancedStereoEnabled && !View->bIsMultiViewEnabled && IStereoRendering::IsStereoEyeView(*View);
		if (bIsInstancedStereoEmulated)
		{
			RHICmdList.SetViewport(0, 0, 0, SceneRender->InstancedStereoWidth, View->ViewRect.Max.Y, 1);
			RHICmdList.SetScissorRect(true, View->ViewRect.Min.X, View->ViewRect.Min.Y, View->ViewRect.Max.X, View->ViewRect.Max.Y);
		}

		const FShadowMeshDrawCommandPass& ProjectionStencilingPass = ProjectionStencilingPasses[ViewIndex];
		if (ProjectionStencilingPass.VisibleMeshDrawCommands.Num() > 0)
		{
			SubmitMeshDrawCommands(ProjectionStencilingPass.VisibleMeshDrawCommands, GraphicsMinimalPipelineStateSet, ProjectionStencilingPass.PrimitiveIdVertexBuffer, 0, bDynamicInstancing, bIsInstancedStereoEmulated ? 2 : 1, RHICmdList);
		}

		// Restore viewport
		if (bIsInstancedStereoEmulated)
		{
			RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
			RHICmdList.SetViewport(View->ViewRect.Min.X, View->ViewRect.Min.Y, 0.0f, View->ViewRect.Max.X, View->ViewRect.Max.Y, 1.0f);
		}
		
	}
	else if (IsWholeSceneDirectionalShadow())
	{
		// Increment stencil on front-facing zfail, decrement on back-facing zfail.
		DrawRenderState.SetDepthStencilState(
			TStaticDepthStencilState<
			false, CF_DepthNearOrEqual,
			true, CF_Always, SO_Keep, SO_Increment, SO_Keep,
			true, CF_Always, SO_Keep, SO_Decrement, SO_Keep,
			0xff, 0xff
			>::GetRHI());

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		DrawRenderState.ApplyToPSO(GraphicsPSOInit);

		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

		checkSlow(CascadeSettings.ShadowSplitIndex >= 0);
		checkSlow(bDirectionalLight);

		// Draw 2 fullscreen planes, front facing one at the near subfrustum plane, and back facing one at the far.

		// Find the projection shaders.
		TShaderMapRef<FShadowProjectionNoTransformVS> VertexShaderNoTransform(View->ShaderMap);
		VertexShaderNoTransform->SetParameters(RHICmdList, View->ViewUniformBuffer);

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(*VertexShaderNoTransform);
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		FVector4 Near = View->ViewMatrices.GetProjectionMatrix().TransformFVector4(FVector4(0, 0, CascadeSettings.SplitNear));
		FVector4 Far = View->ViewMatrices.GetProjectionMatrix().TransformFVector4(FVector4(0, 0, CascadeSettings.SplitFar));
		float StencilNear = Near.Z / Near.W;
		float StencilFar = Far.Z / Far.W;

		FRHIResourceCreateInfo CreateInfo;
		FVertexBufferRHIRef VertexBufferRHI = RHICreateVertexBuffer(sizeof(FVector4) * 12, BUF_Volatile, CreateInfo);
		void* VoidPtr = RHILockVertexBuffer(VertexBufferRHI, 0, sizeof(FVector4) * 12, RLM_WriteOnly);

		// Generate the vertices used
		FVector4* Vertices = (FVector4*)VoidPtr;

			// Far Plane
		Vertices[0] = FVector4( 1,  1,  StencilFar);
		Vertices[1] = FVector4(-1,  1,  StencilFar);
		Vertices[2] = FVector4( 1, -1,  StencilFar);
		Vertices[3] = FVector4( 1, -1,  StencilFar);
		Vertices[4] = FVector4(-1,  1,  StencilFar);
		Vertices[5] = FVector4(-1, -1,  StencilFar);

			// Near Plane
		Vertices[6]  = FVector4(-1,  1, StencilNear);
		Vertices[7]  = FVector4( 1,  1, StencilNear);
		Vertices[8]  = FVector4(-1, -1, StencilNear);
		Vertices[9]  = FVector4(-1, -1, StencilNear);
		Vertices[10] = FVector4( 1,  1, StencilNear);
		Vertices[11] = FVector4( 1, -1, StencilNear);

		RHIUnlockVertexBuffer(VertexBufferRHI);
		RHICmdList.SetStreamSource(0, VertexBufferRHI, 0);
		RHICmdList.DrawPrimitive(0, (CascadeSettings.ShadowSplitIndex > 0) ? 4 : 2, 1);
	}
	// Not a preshadow, mask the projection to any pixels inside the frustum.
	else
	{
		if (bCameraInsideShadowFrustum)
		{
			// Use zfail stenciling when the camera is inside the frustum or the near plane is potentially clipping, 
			// Because zfail handles these cases while zpass does not.
			// zfail stenciling is somewhat slower than zpass because on modern GPUs HiZ will be disabled when setting up stencil.
			// Increment stencil on front-facing zfail, decrement on back-facing zfail.
			DrawRenderState.SetDepthStencilState(
				TStaticDepthStencilState<
				false, CF_DepthNearOrEqual,
				true, CF_Always, SO_Keep, SO_Increment, SO_Keep,
				true, CF_Always, SO_Keep, SO_Decrement, SO_Keep,
				0xff, 0xff
				>::GetRHI());
		}
		else
		{
			// Increment stencil on front-facing zpass, decrement on back-facing zpass.
			// HiZ will be enabled on modern GPUs which will save a little GPU time.
			DrawRenderState.SetDepthStencilState(
				TStaticDepthStencilState<
				false, CF_DepthNearOrEqual,
				true, CF_Always, SO_Keep, SO_Keep, SO_Increment,
				true, CF_Always, SO_Keep, SO_Keep, SO_Decrement,
				0xff, 0xff
				>::GetRHI());
		}
		
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		DrawRenderState.ApplyToPSO(GraphicsPSOInit);
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

		// Find the projection shaders.
		TShaderMapRef<FShadowVolumeBoundProjectionVS> VertexShader(View->ShaderMap);

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(*VertexShader);
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		// Set the projection vertex shader parameters
		VertexShader->SetParameters(RHICmdList, *View, this);

		FRHIResourceCreateInfo CreateInfo;
		FVertexBufferRHIRef VertexBufferRHI = RHICreateVertexBuffer(sizeof(FVector4) * FrustumVertices.Num(), BUF_Volatile, CreateInfo);
		void* VoidPtr = RHILockVertexBuffer(VertexBufferRHI, 0, sizeof(FVector4) * FrustumVertices.Num(), RLM_WriteOnly);
		FPlatformMemory::Memcpy(VoidPtr, FrustumVertices.GetData(), sizeof(FVector4) * FrustumVertices.Num());
		RHIUnlockVertexBuffer(VertexBufferRHI);

		RHICmdList.SetStreamSource(0, VertexBufferRHI, 0);
		// Draw the frustum using the stencil buffer to mask just the pixels which are inside the shadow frustum.
		RHICmdList.DrawIndexedPrimitive(GCubeIndexBuffer.IndexBufferRHI, 0, 0, 8, 0, 12, 1);
		VertexBufferRHI.SafeRelease();

		// if rendering modulated shadows mask out subject mesh elements to prevent self shadowing.
		if (bMobileModulatedProjections && !CVarEnableModulatedSelfShadow.GetValueOnRenderThread())
		{
			const FShadowMeshDrawCommandPass& ProjectionStencilingPass = ProjectionStencilingPasses[ViewIndex];
			if (ProjectionStencilingPass.VisibleMeshDrawCommands.Num() > 0)
			{
				SubmitMeshDrawCommands(ProjectionStencilingPass.VisibleMeshDrawCommands, GraphicsMinimalPipelineStateSet, ProjectionStencilingPass.PrimitiveIdVertexBuffer, 0, bDynamicInstancing, 1, RHICmdList);
			}
		}
	}
}

void FProjectedShadowInfo::RenderProjection(FRHICommandListImmediate& RHICmdList, int32 ViewIndex, const FViewInfo* View, const FSceneRenderer* SceneRender, bool bProjectingForForwardShading, bool bMobileModulatedProjections, const FHairStrandsVisibilityData* HairVisibilityData) const
{
#if WANTS_DRAW_MESH_EVENTS
	FString EventName;

	if (GetEmitDrawEvents())
	{
		GetShadowTypeNameForDrawEvent(EventName);
	}
	SCOPED_DRAW_EVENTF(RHICmdList, EventShadowProjectionActor, *EventName);
#endif

	FScopeCycleCounter Scope(bWholeSceneShadow ? GET_STATID(STAT_RenderWholeSceneShadowProjectionsTime) : GET_STATID(STAT_RenderPerObjectShadowProjectionsTime));

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	// Find the shadow's view relevance.
	const FVisibleLightViewInfo& VisibleLightViewInfo = View->VisibleLightInfos[LightSceneInfo->Id];
	{
		FPrimitiveViewRelevance ViewRelevance = VisibleLightViewInfo.ProjectedShadowViewRelevanceMap[ShadowId];

		// Don't render shadows for subjects which aren't view relevant.
		if (ViewRelevance.bShadowRelevance == false)
		{
			return;
		}
	}

	bool bCameraInsideShadowFrustum;
	TArray<FVector4, TInlineAllocator<8>> FrustumVertices;
	SetupFrustumForProjection(View, FrustumVertices, bCameraInsideShadowFrustum);

	const bool bDepthBoundsTestEnabled = IsWholeSceneDirectionalShadow() && GSupportsDepthBoundsTest && CVarCSMDepthBoundsTest.GetValueOnRenderThread() != 0;

	if (!bDepthBoundsTestEnabled)
	{
		SetupProjectionStencilMask(RHICmdList, View, ViewIndex, SceneRender, FrustumVertices, bMobileModulatedProjections, bCameraInsideShadowFrustum);
	}

	// solid rasterization w/ back-face culling.
	GraphicsPSOInit.RasterizerState = (View->bReverseCulling || IsWholeSceneDirectionalShadow()) ? TStaticRasterizerState<FM_Solid,CM_CCW>::GetRHI() : TStaticRasterizerState<FM_Solid,CM_CW>::GetRHI();

	GraphicsPSOInit.bDepthBounds = bDepthBoundsTestEnabled;
	if (bDepthBoundsTestEnabled)
	{
		// no depth test or writes
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	}
	else
	{
		if (GStencilOptimization)
		{
			// No depth test or writes, zero the stencil
			// Note: this will disable hi-stencil on many GPUs, but still seems 
			// to be faster. However, early stencil still works 
			GraphicsPSOInit.DepthStencilState =
				TStaticDepthStencilState<
				false, CF_Always,
				true, CF_NotEqual, SO_Zero, SO_Zero, SO_Zero,
				false, CF_Always, SO_Zero, SO_Zero, SO_Zero,
				0xff, 0xff
				>::GetRHI();
		}
		else
		{
			// no depth test or writes, Test stencil for non-zero.
			GraphicsPSOInit.DepthStencilState = 
				TStaticDepthStencilState<
				false, CF_Always,
				true, CF_NotEqual, SO_Keep, SO_Keep, SO_Keep,
				false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
				0xff, 0xff
				>::GetRHI();
		}
	}

	SetBlendStateForProjection(GraphicsPSOInit, bProjectingForForwardShading, bMobileModulatedProjections);

	GraphicsPSOInit.PrimitiveType = IsWholeSceneDirectionalShadow() ? PT_TriangleStrip : PT_TriangleList;

	{
		uint32 LocalQuality = GetShadowQuality();

		if (LocalQuality > 1)
		{
			if (IsWholeSceneDirectionalShadow() && CascadeSettings.ShadowSplitIndex > 0)
			{
				// adjust kernel size so that the penumbra size of distant splits will better match up with the closer ones
				const float SizeScale = CascadeSettings.ShadowSplitIndex / FMath::Max(0.001f, CVarCSMSplitPenumbraScale.GetValueOnRenderThread());
			}
			else if (LocalQuality > 2 && !bWholeSceneShadow)
			{
				static auto CVarPreShadowResolutionFactor = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.Shadow.PreShadowResolutionFactor"));
				const int32 TargetResolution = bPreShadow ? FMath::TruncToInt(512 * CVarPreShadowResolutionFactor->GetValueOnRenderThread()) : 512;

				int32 Reduce = 0;

				{
					int32 Res = ResolutionX;

					while (Res < TargetResolution)
					{
						Res *= 2;
						++Reduce;
					}
				}

				// Never drop to quality 1 due to low resolution, aliasing is too bad
				LocalQuality = FMath::Clamp((int32)LocalQuality - Reduce, 3, 5);
			}
		}

		FShadowProjectionVertexShaderInterface* ShadowProjVS = nullptr;
		FShadowProjectionPixelShaderInterface* ShadowProjPS = nullptr;

		const bool bSubPixelSupport = HairVisibilityData != nullptr;
		GetShadowProjectionShaders(LocalQuality, *View, this, bMobileModulatedProjections, bSubPixelSupport, &ShadowProjVS, &ShadowProjPS);

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(ShadowProjVS);
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = GETSAFERHISHADER_PIXEL(ShadowProjPS);

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		if (bDepthBoundsTestEnabled)
		{
			SetDepthBoundsTest(RHICmdList, CascadeSettings.SplitNear, CascadeSettings.SplitFar, View->ViewMatrices.GetProjectionMatrix());
		}

		RHICmdList.SetStencilRef(0);

		ShadowProjVS->SetParameters(RHICmdList, *View, this);
		ShadowProjPS->SetParameters(RHICmdList, ViewIndex, *View, HairVisibilityData, this);
	}

	if (IsWholeSceneDirectionalShadow())
	{
		RHICmdList.SetStreamSource(0, GClearVertexBuffer.VertexBufferRHI, 0);
		RHICmdList.DrawPrimitive(0, 2, 1);
	}
	else
	{
		FRHIResourceCreateInfo CreateInfo;
		FVertexBufferRHIRef VertexBufferRHI = RHICreateVertexBuffer(sizeof(FVector4) * FrustumVertices.Num(), BUF_Volatile, CreateInfo);
		void* VoidPtr = RHILockVertexBuffer(VertexBufferRHI, 0, sizeof(FVector4) * FrustumVertices.Num(), RLM_WriteOnly);
		FPlatformMemory::Memcpy(VoidPtr, FrustumVertices.GetData(), sizeof(FVector4) * FrustumVertices.Num());
		RHIUnlockVertexBuffer(VertexBufferRHI);

		RHICmdList.SetStreamSource(0, VertexBufferRHI, 0);
		// Draw the frustum using the projection shader..
		RHICmdList.DrawIndexedPrimitive(GCubeIndexBuffer.IndexBufferRHI, 0, 0, 8, 0, 12, 1);
		VertexBufferRHI.SafeRelease();
	}

	if (!bDepthBoundsTestEnabled)
	{
		// Clear the stencil buffer to 0.
		if (!GStencilOptimization)
		{
			DrawClearQuad(RHICmdList, false, FLinearColor::Transparent, false, 0, true, 0);
		}
	}
}


template <uint32 Quality, bool bUseTransmission>
static void SetPointLightShaderTempl(FRHICommandList& RHICmdList, FGraphicsPipelineStateInitializer& GraphicsPSOInit, int32 ViewIndex, const FViewInfo& View, const FProjectedShadowInfo* ShadowInfo)
{
	TShaderMapRef<FShadowVolumeBoundProjectionVS> VertexShader(View.ShaderMap);
	TShaderMapRef<TOnePassPointShadowProjectionPS<Quality,bUseTransmission> > PixelShader(View.ShaderMap);

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = GETSAFERHISHADER_VERTEX(*VertexShader);
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = GETSAFERHISHADER_PIXEL(*PixelShader);

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);
	
	VertexShader->SetParameters(RHICmdList, View, ShadowInfo);
	PixelShader->SetParameters(RHICmdList, ViewIndex, View, nullptr, ShadowInfo);
}

/** Render one pass point light shadow projections. */
void FProjectedShadowInfo::RenderOnePassPointLightProjection(FRHICommandListImmediate& RHICmdList, int32 ViewIndex, const FViewInfo& View, bool bProjectingForForwardShading) const
{
	SCOPE_CYCLE_COUNTER(STAT_RenderWholeSceneShadowProjectionsTime);

	checkSlow(bOnePassPointLightShadow);
	
	const FSphere LightBounds = LightSceneInfo->Proxy->GetBoundingSphere();

	bool bUseTransmission = LightSceneInfo->Proxy->Transmission();

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	SetBlendStateForProjection(GraphicsPSOInit, bProjectingForForwardShading, false);
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;

	const bool bCameraInsideLightGeometry = ((FVector)View.ViewMatrices.GetViewOrigin() - LightBounds.Center).SizeSquared() < FMath::Square(LightBounds.W * 1.05f + View.NearClippingDistance * 2.0f);

	if (bCameraInsideLightGeometry)
	{
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		// Render backfaces with depth tests disabled since the camera is inside (or close to inside) the light geometry
		GraphicsPSOInit.RasterizerState = View.bReverseCulling ? TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI() : TStaticRasterizerState<FM_Solid, CM_CCW>::GetRHI();
	}
	else
	{
		// Render frontfaces with depth tests on to get the speedup from HiZ since the camera is outside the light geometry
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI();
		GraphicsPSOInit.RasterizerState = View.bReverseCulling ? TStaticRasterizerState<FM_Solid, CM_CCW>::GetRHI() : TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI();
	}	

	{
		uint32 LocalQuality = GetShadowQuality();

		if(LocalQuality > 1)
		{
			// adjust kernel size so that the penumbra size of distant splits will better match up with the closer ones
			//const float SizeScale = ShadowInfo->ResolutionX;
			int32 Reduce = 0;

			{
				int32 Res = ResolutionX;

				while(Res < 512)
				{
					Res *= 2;
					++Reduce;
				}
			}
		}

		if (bUseTransmission)
		{
			switch (LocalQuality)
			{
				case 1: SetPointLightShaderTempl<1, true>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 2: SetPointLightShaderTempl<2, true>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 3: SetPointLightShaderTempl<3, true>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 4: SetPointLightShaderTempl<4, true>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 5: SetPointLightShaderTempl<5, true>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				default:
					check(0);
			}
		}
		else
		{
			switch (LocalQuality)
			{
				case 1: SetPointLightShaderTempl<1, false>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 2: SetPointLightShaderTempl<2, false>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 3: SetPointLightShaderTempl<3, false>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 4: SetPointLightShaderTempl<4, false>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				case 5: SetPointLightShaderTempl<5, false>(RHICmdList, GraphicsPSOInit, ViewIndex, View, this); break;
				default:
					check(0);
			}
		}
	}

	// Project the point light shadow with some approximately bounding geometry, 
	// So we can get speedups from depth testing and not processing pixels outside of the light's influence.
	StencilingGeometry::DrawSphere(RHICmdList);
}

void FProjectedShadowInfo::RenderFrustumWireframe(FPrimitiveDrawInterface* PDI) const
{
	// Find the ID of an arbitrary subject primitive to use to color the shadow frustum.
	int32 SubjectPrimitiveId = 0;
	if(DynamicSubjectPrimitives.Num())
	{
		SubjectPrimitiveId = DynamicSubjectPrimitives[0]->GetIndex();
	}

	const FMatrix InvShadowTransform = (bWholeSceneShadow || bPreShadow) ? SubjectAndReceiverMatrix.InverseFast() : InvReceiverMatrix;

	FColor Color;

	if(IsWholeSceneDirectionalShadow())
	{
		Color = FColor::White;
		switch(CascadeSettings.ShadowSplitIndex)
		{
			case 0: Color = FColor::Red; break;
			case 1: Color = FColor::Yellow; break;
			case 2: Color = FColor::Green; break;
			case 3: Color = FColor::Blue; break;
		}
	}
	else
	{
		Color = FLinearColor::MakeFromHSV8(( ( SubjectPrimitiveId + LightSceneInfo->Id ) * 31 ) & 255, 0, 255).ToFColor(true);
	}

	// Render the wireframe for the frustum derived from ReceiverMatrix.
	DrawFrustumWireframe(
		PDI,
		InvShadowTransform * FTranslationMatrix(-PreShadowTranslation),
		Color,
		SDPG_World
		);
}

FMatrix FProjectedShadowInfo::GetScreenToShadowMatrix(const FSceneView& View, uint32 TileOffsetX, uint32 TileOffsetY, uint32 TileResolutionX, uint32 TileResolutionY) const
{
	const FIntPoint ShadowBufferResolution = GetShadowBufferResolution();
	const float InvBufferResolutionX = 1.0f / (float)ShadowBufferResolution.X;
	const float ShadowResolutionFractionX = 0.5f * (float)TileResolutionX * InvBufferResolutionX;
	const float InvBufferResolutionY = 1.0f / (float)ShadowBufferResolution.Y;
	const float ShadowResolutionFractionY = 0.5f * (float)TileResolutionY * InvBufferResolutionY;
	// Calculate the matrix to transform a screenspace position into shadow map space

	FMatrix ScreenToShadow;
	FMatrix ViewDependentTransform =
		// Z of the position being transformed is actually view space Z, 
			// Transform it into post projection space by applying the projection matrix,
			// Which is the required space before applying View.InvTranslatedViewProjectionMatrix
		FMatrix(
			FPlane(1,0,0,0),
			FPlane(0,1,0,0),
			FPlane(0,0,View.ViewMatrices.GetProjectionMatrix().M[2][2],1),
			FPlane(0,0,View.ViewMatrices.GetProjectionMatrix().M[3][2],0)) *
		// Transform the post projection space position into translated world space
		// Translated world space is normal world space translated to the view's origin, 
		// Which prevents floating point imprecision far from the world origin.
		View.ViewMatrices.GetInvTranslatedViewProjectionMatrix() *
		FTranslationMatrix(-View.ViewMatrices.GetPreViewTranslation());

	FMatrix ShadowMapDependentTransform =
		// Translate to the origin of the shadow's translated world space
		FTranslationMatrix(PreShadowTranslation) *
		// Transform into the shadow's post projection space
		// This has to be the same transform used to render the shadow depths
		SubjectAndReceiverMatrix *
		// Scale and translate x and y to be texture coordinates into the ShadowInfo's rectangle in the shadow depth buffer
		// Normalize z by MaxSubjectDepth, as was done when writing shadow depths
		FMatrix(
			FPlane(ShadowResolutionFractionX,0,							0,									0),
			FPlane(0,						 -ShadowResolutionFractionY,0,									0),
			FPlane(0,						0,							InvMaxSubjectDepth,	0),
			FPlane(
				(TileOffsetX + BorderSize) * InvBufferResolutionX + ShadowResolutionFractionX,
				(TileOffsetY + BorderSize) * InvBufferResolutionY + ShadowResolutionFractionY,
				0,
				1
				)
			);

	if (View.bIsMobileMultiViewEnabled && View.Family->Views.Num() > 0)
	{
		// In Multiview, we split ViewDependentTransform out into ViewUniformShaderParameters.MobileMultiviewShadowTransform
		// So we can multiply it later in shader.
		ScreenToShadow = ShadowMapDependentTransform;
	}
	else
	{
		ScreenToShadow = ViewDependentTransform * ShadowMapDependentTransform;
	}
	return ScreenToShadow;
}

FMatrix FProjectedShadowInfo::GetWorldToShadowMatrix(FVector4& ShadowmapMinMax, const FIntPoint* ShadowBufferResolutionOverride) const
{
	FIntPoint ShadowBufferResolution = ( ShadowBufferResolutionOverride ) ? *ShadowBufferResolutionOverride : GetShadowBufferResolution();

	const float InvBufferResolutionX = 1.0f / (float)ShadowBufferResolution.X;
	const float ShadowResolutionFractionX = 0.5f * (float)ResolutionX * InvBufferResolutionX;
	const float InvBufferResolutionY = 1.0f / (float)ShadowBufferResolution.Y;
	const float ShadowResolutionFractionY = 0.5f * (float)ResolutionY * InvBufferResolutionY;

	const FMatrix WorldToShadowMatrix =
		// Translate to the origin of the shadow's translated world space
		FTranslationMatrix(PreShadowTranslation) *
		// Transform into the shadow's post projection space
		// This has to be the same transform used to render the shadow depths
		SubjectAndReceiverMatrix *
		// Scale and translate x and y to be texture coordinates into the ShadowInfo's rectangle in the shadow depth buffer
		// Normalize z by MaxSubjectDepth, as was done when writing shadow depths
		FMatrix(
			FPlane(ShadowResolutionFractionX,0,							0,									0),
			FPlane(0,						 -ShadowResolutionFractionY,0,									0),
			FPlane(0,						0,							InvMaxSubjectDepth,	0),
			FPlane(
				(X + BorderSize) * InvBufferResolutionX + ShadowResolutionFractionX,
				(Y + BorderSize) * InvBufferResolutionY + ShadowResolutionFractionY,
				0,
				1
			)
		);

	ShadowmapMinMax = FVector4(
		(X + BorderSize) * InvBufferResolutionX, 
		(Y + BorderSize) * InvBufferResolutionY,
		(X + BorderSize * 2 + ResolutionX) * InvBufferResolutionX, 
		(Y + BorderSize * 2 + ResolutionY) * InvBufferResolutionY);

	return WorldToShadowMatrix;
}

void FProjectedShadowInfo::UpdateShaderDepthBias()
{
	float DepthBias = 0;
	float SlopeScaleDepthBias = 1;

	if (IsWholeScenePointLightShadow())
	{
		const bool bIsRectLight = LightSceneInfo->Proxy->GetLightType() == LightType_Rect;
		float DeptBiasConstant = 0;
		float SlopeDepthBiasConstant = 0;
		if (bIsRectLight)
		{
			DeptBiasConstant = CVarRectLightShadowDepthBias.GetValueOnRenderThread();
			SlopeDepthBiasConstant = CVarRectLightShadowSlopeScaleDepthBias.GetValueOnRenderThread();
		}
		else
		{
			DeptBiasConstant = CVarPointLightShadowDepthBias.GetValueOnRenderThread();
			SlopeDepthBiasConstant = CVarPointLightShadowSlopeScaleDepthBias.GetValueOnRenderThread();
		}

		DepthBias = DeptBiasConstant * 512.0f / FMath::Max(ResolutionX, ResolutionY);
		// * 2.0f to be compatible with the system we had before ShadowBias
		DepthBias *= 2.0f * LightSceneInfo->Proxy->GetUserShadowBias();

		SlopeScaleDepthBias = SlopeDepthBiasConstant;
		SlopeScaleDepthBias *= LightSceneInfo->Proxy->GetUserShadowSlopeBias();
	}
	else if (IsWholeSceneDirectionalShadow())
	{
		check(CascadeSettings.ShadowSplitIndex >= 0);

		// the z range is adjusted to we need to adjust here as well
		DepthBias = CVarCSMShadowDepthBias.GetValueOnRenderThread() / (MaxSubjectZ - MinSubjectZ);
		const float WorldSpaceTexelScale = ShadowBounds.W / ResolutionX;
		DepthBias = FMath::Lerp(DepthBias, DepthBias * WorldSpaceTexelScale, CascadeSettings.CascadeBiasDistribution);
		DepthBias *= LightSceneInfo->Proxy->GetUserShadowBias();

		SlopeScaleDepthBias = CVarCSMShadowSlopeScaleDepthBias.GetValueOnRenderThread();
		SlopeScaleDepthBias *= LightSceneInfo->Proxy->GetUserShadowSlopeBias();
	}
	else if (bPreShadow)
	{
		// Preshadows don't need a depth bias since there is no self shadowing
		DepthBias = 0;
		SlopeScaleDepthBias = 0;
	}
	else
	{
		// per object shadows
		if(bDirectionalLight)
		{
			// we use CSMShadowDepthBias cvar but this is per object shadows, maybe we want to use different settings

			// the z range is adjusted to we need to adjust here as well
			DepthBias = CVarPerObjectDirectionalShadowDepthBias.GetValueOnRenderThread() / (MaxSubjectZ - MinSubjectZ);

			float WorldSpaceTexelScale = ShadowBounds.W / FMath::Max(ResolutionX, ResolutionY);
		
			DepthBias *= WorldSpaceTexelScale;
			DepthBias *= 0.5f;	// avg GetUserShadowBias, in that case we don't want this adjustable

			SlopeScaleDepthBias = CVarPerObjectDirectionalShadowSlopeScaleDepthBias.GetValueOnRenderThread();
			SlopeScaleDepthBias *= LightSceneInfo->Proxy->GetUserShadowSlopeBias();
		}
		else
		{
			// spot lights (old code, might need to be improved)
			const float LightTypeDepthBias = CVarSpotLightShadowDepthBias.GetValueOnRenderThread();
			DepthBias = LightTypeDepthBias * 512.0f / ((MaxSubjectZ - MinSubjectZ) * FMath::Max(ResolutionX, ResolutionY));
			// * 2.0f to be compatible with the system we had before ShadowBias
			DepthBias *= 2.0f * LightSceneInfo->Proxy->GetUserShadowBias();

			SlopeScaleDepthBias = CVarSpotLightShadowSlopeScaleDepthBias.GetValueOnRenderThread();
			SlopeScaleDepthBias *= LightSceneInfo->Proxy->GetUserShadowSlopeBias();
		}

		// Prevent a large depth bias due to low resolution from causing near plane clipping
		DepthBias = FMath::Min(DepthBias, .1f);
	}

	ShaderDepthBias = FMath::Max(DepthBias, 0.0f);
	ShaderSlopeDepthBias = FMath::Max(DepthBias * SlopeScaleDepthBias, 0.0f);
	ShaderMaxSlopeDepthBias = CVarShadowMaxSlopeScaleDepthBias.GetValueOnRenderThread();
}

float FProjectedShadowInfo::ComputeTransitionSize() const
{
	float TransitionSize = 1.0f;

	if (IsWholeScenePointLightShadow())
	{
		// todo: optimize
		TransitionSize = bDirectionalLight ? (1.0f / CVarShadowTransitionScale.GetValueOnRenderThread()) : (1.0f / CVarSpotLightShadowTransitionScale.GetValueOnRenderThread());
		// * 2.0f to be compatible with the system we had before ShadowBias
		TransitionSize *= 2.0f * LightSceneInfo->Proxy->GetUserShadowBias();
	}
	else if (IsWholeSceneDirectionalShadow())
	{
		check(CascadeSettings.ShadowSplitIndex >= 0);

		// todo: remove GetShadowTransitionScale()
		// make 1/ ShadowTransitionScale, SpotLightShadowTransitionScale

		// the z range is adjusted to we need to adjust here as well
		TransitionSize = CVarCSMShadowDepthBias.GetValueOnRenderThread() / (MaxSubjectZ - MinSubjectZ);

		float WorldSpaceTexelScale = ShadowBounds.W / ResolutionX;

		TransitionSize *= WorldSpaceTexelScale;
		TransitionSize *= LightSceneInfo->Proxy->GetUserShadowBias();
	}
	else if (bPreShadow)
	{
		// Preshadows don't have self shadowing, so make sure the shadow starts as close to the caster as possible
		TransitionSize = 0.0f;
	}
	else
	{
		// todo: optimize
		TransitionSize = bDirectionalLight ? (1.0f / CVarShadowTransitionScale.GetValueOnRenderThread()) : (1.0f / CVarSpotLightShadowTransitionScale.GetValueOnRenderThread());
		// * 2.0f to be compatible with the system we had before ShadowBias
		TransitionSize *= 2.0f * LightSceneInfo->Proxy->GetUserShadowBias();
	}

	// Make sure that shadow soft transition size is greater than zero so 1/TransitionSize shader parameter won't be INF.
	const float MinTransitionSize = 0.00001f;
	return FMath::Max(TransitionSize, MinTransitionSize);
}

float FProjectedShadowInfo::GetShaderReceiverDepthBias() const
{
	float ShadowReceiverBias = 1;
	{
		switch (GetLightSceneInfo().Proxy->GetLightType())
		{
		case LightType_Directional	: ShadowReceiverBias = CVarCSMShadowReceiverBias.GetValueOnRenderThread(); break;
		case LightType_Rect			: ShadowReceiverBias = CVarRectLightShadowReceiverBias.GetValueOnRenderThread(); break;
		case LightType_Spot			: ShadowReceiverBias = CVarSpotLightShadowReceiverBias.GetValueOnRenderThread(); break;
		case LightType_Point		: ShadowReceiverBias = GetShaderSlopeDepthBias(); break;
		}
	}

	// Return the min lerp value for depth biasing
	// 0 : max bias when NoL == 0
	// 1 : no bias
	return 1.0f - FMath::Clamp(ShadowReceiverBias, 0.0f, 1.0f);
}
/*-----------------------------------------------------------------------------
FDeferredShadingSceneRenderer
-----------------------------------------------------------------------------*/

/**
 * Used by RenderLights to figure out if projected shadows need to be rendered to the attenuation buffer.
 *
 * @param LightSceneInfo Represents the current light
 * @return true if anything needs to be rendered
 */
bool FSceneRenderer::CheckForProjectedShadows( const FLightSceneInfo* LightSceneInfo ) const
{
	// If light has ray-traced occlusion enabled, then it will project some shadows. No need 
	// for doing a lookup through shadow maps data
	const FLightOcclusionType LightOcclusionType = GetLightOcclusionType(*LightSceneInfo->Proxy);
	if (LightOcclusionType == FLightOcclusionType::Raytraced)
		return true;

	// Find the projected shadows cast by this light.
	const FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];
	for( int32 ShadowIndex=0; ShadowIndex<VisibleLightInfo.AllProjectedShadows.Num(); ShadowIndex++ )
	{
		const FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.AllProjectedShadows[ShadowIndex];

		// Check that the shadow is visible in at least one view before rendering it.
		bool bShadowIsVisible = false;
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];
			if (ProjectedShadowInfo->DependentView && ProjectedShadowInfo->DependentView != &View)
			{
				continue;
			}
			const FVisibleLightViewInfo& VisibleLightViewInfo = View.VisibleLightInfos[LightSceneInfo->Id];
			bShadowIsVisible |= VisibleLightViewInfo.ProjectedShadowVisibilityMap[ShadowIndex];
		}

		if(bShadowIsVisible)
		{
			return true;
		}
	}
	return false;
}

bool FDeferredShadingSceneRenderer::InjectReflectiveShadowMaps(FRHICommandListImmediate& RHICmdList, const FLightSceneInfo* LightSceneInfo)
{
	FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];

	// Inject the RSM into the LPVs
	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.RSMsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.RSMsToProject[ShadowIndex];

		check(ProjectedShadowInfo->bReflectiveShadowmap);

		if (ProjectedShadowInfo->bAllocated && ProjectedShadowInfo->DependentView)
		{
			FSceneViewState* ViewState = (FSceneViewState*)ProjectedShadowInfo->DependentView->State;

			FLightPropagationVolume* LightPropagationVolume = ViewState ? ViewState->GetLightPropagationVolume(FeatureLevel) : NULL;

			if (LightPropagationVolume)
			{
				if (ProjectedShadowInfo->bWholeSceneShadow)
				{
					LightPropagationVolume->InjectDirectionalLightRSM( 
						RHICmdList, 
						*ProjectedShadowInfo->DependentView,
						(const FTexture2DRHIRef&)ProjectedShadowInfo->RenderTargets.ColorTargets[0]->GetRenderTargetItem().ShaderResourceTexture,
						(const FTexture2DRHIRef&)ProjectedShadowInfo->RenderTargets.ColorTargets[1]->GetRenderTargetItem().ShaderResourceTexture, 
						(const FTexture2DRHIRef&)ProjectedShadowInfo->RenderTargets.DepthTarget->GetRenderTargetItem().ShaderResourceTexture,
						*ProjectedShadowInfo, 
						LightSceneInfo->Proxy->GetColor() );
				}
			}
		}
	}

	return true;
}

bool FSceneRenderer::RenderShadowProjections(FRHICommandListImmediate& RHICmdList, const FLightSceneInfo* LightSceneInfo, IPooledRenderTarget* ScreenShadowMaskTexture, IPooledRenderTarget* ScreenShadowMaskSubPixelTexture, bool bProjectingForForwardShading, bool bMobileModulatedProjections, const FHairStrandsVisibilityViews* InHairVisibilityViews)
{
	FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	// Gather up our work real quick so we can do everything in one renderpass later.
	// #todo-renderpasses How many ShadowsToProject do we have usually?
	TArray<FProjectedShadowInfo*> DistanceFieldShadows;
	TArray<FProjectedShadowInfo*> NormalShadows;

	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];
		if (ProjectedShadowInfo->bRayTracedDistanceField)
		{
			DistanceFieldShadows.Add(ProjectedShadowInfo);
		}
		else
		{
			NormalShadows.Add(ProjectedShadowInfo);
		}
	}

	if (NormalShadows.Num() > 0)
	{
		auto RenderShadowMask = [&](const FHairStrandsVisibilityViews* HairVisibilityViews)
		{
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				const FViewInfo& View = Views[ViewIndex];

				SCOPED_GPU_MASK(RHICmdList, View.GPUMask);
				SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, EventView, Views.Num() > 1, TEXT("View%d"), ViewIndex);

				const FHairStrandsVisibilityData* HairVisibilityData = nullptr;
				if (HairVisibilityViews)
				{
					HairVisibilityData = &(HairVisibilityViews->HairDatas[ViewIndex]);
				}

				// Set the device viewport for the view.
				RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

				// Set the light's scissor rectangle.
				LightSceneInfo->Proxy->SetScissorRect(RHICmdList, View, View.ViewRect);

				Scene->UniformBuffers.UpdateViewUniformBuffer(View);
				check(View.ViewUniformBuffer != nullptr);

				// Project the shadow depth buffers onto the scene.
				for (int32 ShadowIndex = 0; ShadowIndex < NormalShadows.Num(); ShadowIndex++)
				{
					FProjectedShadowInfo* ProjectedShadowInfo = NormalShadows[ShadowIndex];

					if (ProjectedShadowInfo->bAllocated)
					{
						// Only project the shadow if it's large enough in this particular view (split screen, etc... may have shadows that are large in one view but irrelevantly small in others)
						if (ProjectedShadowInfo->FadeAlphas[ViewIndex] > 1.0f / 256.0f)
						{
							if (ProjectedShadowInfo->bOnePassPointLightShadow)
							{
								ProjectedShadowInfo->RenderOnePassPointLightProjection(RHICmdList, ViewIndex, View, bProjectingForForwardShading);
							}
							else
							{
								ProjectedShadowInfo->RenderProjection(RHICmdList, ViewIndex, &View, this, bProjectingForForwardShading, bMobileModulatedProjections, HairVisibilityData);
							}
						}
					}
				}
			}
		};

		// Render normal shadows
		if (bMobileModulatedProjections)
		{
			// part of scene color rendering pass
			check(RHICmdList.IsInsideRenderPass());
			RenderShadowMask(nullptr);
			// Reset the scissor rectangle.
			RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
		}
		else
		{
			check(RHICmdList.IsOutsideRenderPass());
			// Normal deferred shadows render to the shadow mask
			FRHIRenderPassInfo RPInfo(ScreenShadowMaskTexture->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Load_Store);
			RPInfo.DepthStencilRenderTarget.Action = MakeDepthStencilTargetActions(ERenderTargetActions::Load_DontStore, ERenderTargetActions::Load_Store);
			RPInfo.DepthStencilRenderTarget.DepthStencilTarget = SceneContext.GetSceneDepthSurface();
			RPInfo.DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthRead_StencilWrite;

			TransitionRenderPassTargets(RHICmdList, RPInfo);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("RenderShadowProjection"));
			RenderShadowMask(nullptr);
			RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
			RHICmdList.EndRenderPass();
		}

		// SubPixelShadow
		if (!bMobileModulatedProjections && ScreenShadowMaskSubPixelTexture && InHairVisibilityViews)
		{			
			check(RHICmdList.IsOutsideRenderPass());
			// Normal deferred shadows render to the shadow mask
			FRHIRenderPassInfo RPInfo(ScreenShadowMaskSubPixelTexture->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::Load_Store);
			RPInfo.DepthStencilRenderTarget.Action = MakeDepthStencilTargetActions(ERenderTargetActions::Load_DontStore, ERenderTargetActions::Load_Store);
			RPInfo.DepthStencilRenderTarget.DepthStencilTarget = SceneContext.GetSceneDepthSurface();
			RPInfo.DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthRead_StencilWrite;

			TransitionRenderPassTargets(RHICmdList, RPInfo);
			RHICmdList.BeginRenderPass(RPInfo, TEXT("RenderShadowProjectionSubPixel"));
			RenderShadowMask(InHairVisibilityViews);
			RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
			RHICmdList.EndRenderPass();
		}
	}

	if (DistanceFieldShadows.Num() > 0)
	{
		// Distance field shadows need to be renderer last as they blend over far shadow cascades.
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];

			SCOPED_GPU_MASK(RHICmdList, View.GPUMask);
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, EventView, Views.Num() > 1, TEXT("DistanceFieldShadows_View%d"), ViewIndex);

			// Set the device viewport for the view.
			RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

			// Set the light's scissor rectangle.
			FIntRect ScissorRect;
			if (!LightSceneInfo->Proxy->SetScissorRect(RHICmdList, View, View.ViewRect, &ScissorRect))
			{
				ScissorRect = View.ViewRect;
			}

			if (ScissorRect.Area() > 0)
			{
				// Project the shadow depth buffers onto the scene.
				for (int32 ShadowIndex = 0; ShadowIndex < DistanceFieldShadows.Num(); ShadowIndex++)
				{
					FProjectedShadowInfo* ProjectedShadowInfo = DistanceFieldShadows[ShadowIndex];
					ProjectedShadowInfo->RenderRayTracedDistanceFieldProjection(RHICmdList, View, ScissorRect, ScreenShadowMaskTexture, bProjectingForForwardShading);
				}
			}

			// Reset the scissor rectangle.
			RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
		}
	}

	return true;
}

// Scree space shadow render function, which could be combined with shadow mapping
bool FSceneRenderer::RenderScreenSpaceShadows(FRHICommandListImmediate& RHICmdList, const FLightSceneInfo* LightSceneInfo, IPooledRenderTarget* ScreenShadowMaskTexture, IPooledRenderTarget* ScreenShadowMaskSubPixelTexture, bool bProjectingForForwardShading, bool bMobileModulatedProjections, const FHairStrandsVisibilityViews* InHairVisibilityViews) {
	FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	// Gather up our work real quick so we can do everything in one renderpass later.
	// #todo-renderpasses How many ShadowsToProject do we have usually?
	TArray<FProjectedShadowInfo*> DistanceFieldShadows;
	TArray<FProjectedShadowInfo*> NormalShadows;

	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];
		if (ProjectedShadowInfo->bRayTracedDistanceField)
		{
			DistanceFieldShadows.Add(ProjectedShadowInfo);
		}
		else
		{
			NormalShadows.Add(ProjectedShadowInfo);
		}
	}

	if (NormalShadows.Num() > 0) {
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++) 
		{
			const FViewInfo& View = Views[ViewIndex];

			for (int32 ShadowIndex = 0; ShadowIndex < NormalShadows.Num(); ShadowIndex++)
			{
				FProjectedShadowInfo* ProjectedShadowInfo = NormalShadows[ShadowIndex];

				if (ProjectedShadowInfo->bAllocated)
				{
					FRDGBuilder GraphBuilder(RHICmdList);
					// Pixel Shader
					//TShaderMapRef<FScreenSpaceShadowsProjectionPS> ScreenSpaceShadowsPixelShader(View.ShaderMap);
					//FScreenSpaceShadowsProjectionPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenSpaceShadowsProjectionPS::FParameters>();
					/*FRDGTextureRef ShadowTexture = GraphBuilder.RegisterExternalTexture(
						ScreenShadowMaskTexture,
						TEXT("ShadowMaskTexture")
					);*/

					//// create output texture，后续可能需要改为 UAV 纹理
					//FRDGTextureDesc ScreenSpaceShadowTextureDesc = FRDGTextureDesc::Create2DDesc(
					//	ShadowTexture->Desc.Extent,         // texture size
					//	PF_FloatRGBA,						// format, e.g PF_R8G8B8A8, PF_FloatRGBA
					//	FClearValueBinding::Black,			// clear value
					//	TexCreate_None,						// Flags
					//	TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV, // TargetableFlags
					//	/* bInForceSeparateTargetAndShaderResource = */ false
					//);
					//FRDGTextureRef ScreenSpaceShadowTexture = GraphBuilder.CreateTexture(
					//	ScreenSpaceShadowTextureDesc,
					//	TEXT("ScreenSpaceShadowTexture")
					//);
					//
					//// bind gbuffer, rt and light
					//PassParameters->View = View.ViewUniformBuffer; // view matrix
					//PassParameters->RenderTargets[0] = FRenderTargetBinding(ScreenSpaceShadowTexture, ERenderTargetLoadAction::EClear); // render target
					//FSceneTextureParameters SceneTextures;
					//SetupSceneTextureParameters(GraphBuilder, &SceneTextures);  // standard GBuffer Texture
					//PassParameters->SceneTextures = SceneTextures;
					//const FLightSceneProxy& LightProxy = *(ProjectedShadowInfo->GetLightSceneInfo().Proxy);

					//const FVector LightDirection = ProjectedShadowInfo->GetLightSceneInfo().Proxy->GetDirection();
					//const FVector LightPosition = ProjectedShadowInfo->GetLightSceneInfo().Proxy->GetPosition();
					//const bool bIsDirectional = ProjectedShadowInfo->GetLightSceneInfo().Proxy->GetLightType() == LightType_Directional;

					//
					//PassParameters->LightPositionOrDirection = bIsDirectional ? FVector4(LightDirection, 0) : FVector4(LightPosition, 1);
					//// end bind

					//ClearUnusedGraphResources(*ScreenSpaceShadowsPixelShader, PassParameters);

					//GraphBuilder.AddPass(
					//	RDG_EVENT_NAME("ScreenSpaceShadowsProjection"),
					//	PassParameters,
					//	ERDGPassFlags::Raster,
					//	[ScreenSpaceShadowsPixelShader, PassParameters, &View](FRHICommandListImmediate& RHICmdList)
					//	{
					//		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

					//		FGraphicsPipelineStateInitializer GraphicsPSOInit;
					//		FPixelShaderUtils::InitFullscreenPipelineState(RHICmdList, View.ShaderMap, *ScreenSpaceShadowsPixelShader, GraphicsPSOInit);
					//		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
					//		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

					//		// bind shader parameters
					//		SetShaderParameters(RHICmdList, *ScreenSpaceShadowsPixelShader, ScreenSpaceShadowsPixelShader->GetPixelShader(), *PassParameters);

					//		// draw
					//		FPixelShaderUtils::DrawFullscreenTriangle(RHICmdList);

					//	}
					//);

					//GraphBuilder.Execute();
					// end pixel shader
					
					// compute shader
					FRDGTextureRef ShadowTexture = GraphBuilder.RegisterExternalTexture(
						ScreenShadowMaskTexture,
						TEXT("ShadowMaskTexture")
					);

					TShaderMapRef<FScreenSpaceShadowsCS> ScreenSpaceShadowsCS(View.ShaderMap);
					FScreenSpaceShadowsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenSpaceShadowsCS::FParameters>();

					FIntRect ScissorRect = View.ViewRect;
					FRDGTextureDesc ScreenSpaceShadowTextureDesc = FRDGTextureDesc::Create2DDesc(
						FIntPoint(ScissorRect.Width(), ScissorRect.Height()),         // texture size
						PF_G16R16F,						// format, e.g PF_R8G8B8A8, PF_FloatRGBA
						FClearValueBinding::Black,			// clear value
						TexCreate_None,						// Flags
						TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV, // TargetableFlags
						/* bInForceSeparateTargetAndShaderResource = */ false
					);
					FRDGTextureRef ScreenSpaceShadowTexture = GraphBuilder.CreateTexture(
						ScreenSpaceShadowTextureDesc,
						TEXT("ScreenSpaceShadowTexture")
					);

					auto SSSTextureUAV = GraphBuilder.CreateUAV(ScreenSpaceShadowTexture);
					PassParameters->RWShadowFactors = SSSTextureUAV;
					PassParameters->View = View.ViewUniformBuffer;

					// test
					FRDGTextureDesc testDesc = FRDGTextureDesc::Create2DDesc(
						FIntPoint(ScissorRect.Width(), ScissorRect.Height()),         // texture size
						PF_G16R16F,						// format, e.g PF_R8G8B8A8, PF_FloatRGBA
						FClearValueBinding::Black,			// clear value
						TexCreate_None,						// Flags
						TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV, // TargetableFlags
						/* bInForceSeparateTargetAndShaderResource = */ false
					);
					FRDGTextureRef testTexture = GraphBuilder.CreateTexture(
						testDesc,
						TEXT("test")
					);

					auto testUAV = GraphBuilder.CreateUAV(testTexture);
					PassParameters->testFactors = testUAV;
					// end test

					FSceneTextureParameters SceneTextures;
					SetupSceneTextureParameters(GraphBuilder, &SceneTextures);  // standard GBuffer Texture
					PassParameters->SceneTextures = SceneTextures;
					const FVector LightDirection = ProjectedShadowInfo->GetLightSceneInfo().Proxy->GetDirection();
					const FVector LightPosition = ProjectedShadowInfo->GetLightSceneInfo().Proxy->GetPosition();
					const bool bIsDirectional = ProjectedShadowInfo->GetLightSceneInfo().Proxy->GetLightType() == LightType_Directional;

					PassParameters->LightPositionOrDirection = bIsDirectional ? FVector4(LightDirection, 0) : FVector4(LightPosition, 1);

					
					// ScreenSpaceShadowsCS always outputs at rect with min = (0,0)
					FIntRect ShadowsTextureViewRect(0, 0, ScissorRect.Width(), ScissorRect.Height());

					uint32 GroupSizeX = FMath::DivideAndRoundUp(ShadowsTextureViewRect.Width(), 8);
					uint32 GroupSizeY = FMath::DivideAndRoundUp(ShadowsTextureViewRect.Height(), 8);

					// RDG_EVENT_SCOPE(GraphBuilder, "ScreenSpaceShadows");
					if (GroupSizeX == 0 || GroupSizeY == 0) {
						GroupSizeX = 8;
						return false;
					}


					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("ScreenSpaceShadowing"),
						*ScreenSpaceShadowsCS,
						PassParameters,
						FIntVector(GroupSizeX, GroupSizeY, 1));
					GraphBuilder.Execute();

					// end compute shader

				
				}
			}
		}
	}

	return true;
	
}
	
bool FDeferredShadingSceneRenderer::RenderShadowProjections(FRHICommandListImmediate& RHICmdList, const FLightSceneInfo* LightSceneInfo, IPooledRenderTarget* ScreenShadowMaskTexture, IPooledRenderTarget* ScreenShadowMaskSubPixelTexture, const FHairStrandsDatas* HairDatas, bool& bInjectedTranslucentVolume)
{
	SCOPED_NAMED_EVENT(FDeferredShadingSceneRenderer_RenderShadowProjections, FColor::Emerald);
	SCOPE_CYCLE_COUNTER(STAT_ProjectedShadowDrawTime);
	SCOPED_DRAW_EVENT(RHICmdList, ShadowProjectionOnOpaque);
	SCOPED_GPU_STAT(RHICmdList, ShadowProjection);

	check(RHICmdList.IsOutsideRenderPass());

	FVisibleLightInfo& VisibleLightInfo = VisibleLightInfos[LightSceneInfo->Id];

	FSceneRenderer::RenderShadowProjections(RHICmdList, LightSceneInfo, ScreenShadowMaskTexture, ScreenShadowMaskSubPixelTexture, false, false, HairDatas ? &HairDatas->HairVisibilityViews : nullptr);

	// add a simple shader 
	{
		SCOPED_DRAW_EVENT(RHICmdList, ScreenSpaceShadows);		// add a event, which could be seen in RenderDoc
		FRHIRenderPassInfo RPInfo(ScreenShadowMaskTexture->GetRenderTargetItem().TargetableTexture, ERenderTargetActions::DontLoad_Store);
		RHICmdList.SetStencilRef(0);

		FSceneRenderer::RenderScreenSpaceShadows(RHICmdList, LightSceneInfo, ScreenShadowMaskTexture, ScreenShadowMaskSubPixelTexture, false, false, HairDatas ? &HairDatas->HairVisibilityViews : nullptr);
	}
	// end add

	checkSlow(RHICmdList.IsOutsideRenderPass());
	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];

		if (ProjectedShadowInfo->bAllocated
			&& ProjectedShadowInfo->bWholeSceneShadow
			// Not supported on translucency yet
			&& !ProjectedShadowInfo->bRayTracedDistanceField
			// Don't inject shadowed lighting with whole scene shadows used for previewing a light with static shadows,
			// Since that would cause a mismatch with the built lighting
			// However, stationary directional lights allow whole scene shadows that blend with precomputed shadowing
			&& (!LightSceneInfo->Proxy->HasStaticShadowing() || ProjectedShadowInfo->IsWholeSceneDirectionalShadow()))
		{
			bInjectedTranslucentVolume = true;
			SCOPED_DRAW_EVENT(RHICmdList, InjectTranslucentVolume);
			
			// Inject the shadowed light into the translucency lighting volumes
			if(ProjectedShadowInfo->DependentView != nullptr)
			{
				int32 ViewIndex = -1;
				for (int32 i = 0; i < Views.Num(); ++i)
				{
					if (ProjectedShadowInfo->DependentView == &Views[i])
					{
						ViewIndex = i;
						break;
					}
				}

				SCOPED_GPU_MASK(RHICmdList, ProjectedShadowInfo->DependentView->GPUMask);
				InjectTranslucentVolumeLighting(RHICmdList, *LightSceneInfo, ProjectedShadowInfo, *ProjectedShadowInfo->DependentView, ViewIndex);
			}
			else
			{
				for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
				{
					FViewInfo& View = Views[ViewIndex];
					SCOPED_GPU_MASK(RHICmdList, View.GPUMask);
					InjectTranslucentVolumeLighting(RHICmdList, *LightSceneInfo, ProjectedShadowInfo, View, ViewIndex);
				}
			}
		}
	}

	RenderCapsuleDirectShadows(RHICmdList, *LightSceneInfo, ScreenShadowMaskTexture, VisibleLightInfo.CapsuleShadowsToProject, false);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		SCOPED_GPU_MASK(RHICmdList, View.GPUMask);

		for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
		{
			FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];

			if (ProjectedShadowInfo->bAllocated
				&& ProjectedShadowInfo->bWholeSceneShadow)
			{
				View.HeightfieldLightingViewInfo.ComputeShadowMapShadowing(View, RHICmdList, ProjectedShadowInfo);
			}
		}
	}

	// Inject deep shadow mask
	if (HairDatas)
	{
		RenderHairStrandsShadowMask(RHICmdList, Views, LightSceneInfo, ScreenShadowMaskTexture, HairDatas);
	}

	return true;
}

void FMobileSceneRenderer::RenderModulatedShadowProjections(FRHICommandListImmediate& RHICmdList)
{
	if (IsSimpleForwardShadingEnabled(ShaderPlatform) || !ViewFamily.EngineShowFlags.DynamicShadows || (!IsMobileHDR() && (ShaderPlatform != SP_OPENGL_ES2_WEBGL)))
	{
		return;
	}
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	// render shadowmaps for relevant lights.
	for (TSparseArray<FLightSceneInfoCompact>::TConstIterator LightIt(Scene->Lights); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
		FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;
		if(LightSceneInfo->ShouldRenderLightViewIndependent() && LightSceneInfo->Proxy && LightSceneInfo->Proxy->CastsModulatedShadows())
		{
			TArray<FProjectedShadowInfo*, SceneRenderingAllocator> Shadows;
			SCOPE_CYCLE_COUNTER(STAT_ProjectedShadowDrawTime);
			FSceneRenderer::RenderShadowProjections(RHICmdList, LightSceneInfo, nullptr, nullptr, false, true, nullptr);
		}
	}
}

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FTranslucentSelfShadowUniformParameters, "TranslucentSelfShadow");

void SetupTranslucentSelfShadowUniformParameters(const FProjectedShadowInfo* ShadowInfo, FTranslucentSelfShadowUniformParameters& OutParameters)
{
	if (ShadowInfo)
	{
		FVector4 ShadowmapMinMax;
		FMatrix WorldToShadowMatrixValue = ShadowInfo->GetWorldToShadowMatrix(ShadowmapMinMax);

		OutParameters.WorldToShadowMatrix = WorldToShadowMatrixValue;
		OutParameters.ShadowUVMinMax = ShadowmapMinMax;

		const FLightSceneProxy* const LightProxy = ShadowInfo->GetLightSceneInfo().Proxy;
		OutParameters.DirectionalLightDirection = LightProxy->GetDirection();

		//@todo - support fading from both views
		const float FadeAlpha = ShadowInfo->FadeAlphas[0];
		// Incorporate the diffuse scale of 1 / PI into the light color
		OutParameters.DirectionalLightColor = FVector4(FVector(LightProxy->GetColor() * FadeAlpha / PI), FadeAlpha);

		OutParameters.Transmission0 = ShadowInfo->RenderTargets.ColorTargets[0]->GetRenderTargetItem().ShaderResourceTexture.GetReference();
		OutParameters.Transmission1 = ShadowInfo->RenderTargets.ColorTargets[1]->GetRenderTargetItem().ShaderResourceTexture.GetReference();
		OutParameters.Transmission0Sampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		OutParameters.Transmission1Sampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	}
	else
	{
		OutParameters.Transmission0 = GBlackTexture->TextureRHI;
		OutParameters.Transmission1 = GBlackTexture->TextureRHI;
		OutParameters.Transmission0Sampler = GBlackTexture->SamplerStateRHI;
		OutParameters.Transmission1Sampler = GBlackTexture->SamplerStateRHI;
		
		OutParameters.DirectionalLightColor = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
	}
}

void FEmptyTranslucentSelfShadowUniformBuffer::InitDynamicRHI()
{
	FTranslucentSelfShadowUniformParameters Parameters;
	SetupTranslucentSelfShadowUniformParameters(nullptr, Parameters);
	SetContentsNoUpdate(Parameters);

	Super::InitDynamicRHI();
}

/** */
TGlobalResource< FEmptyTranslucentSelfShadowUniformBuffer > GEmptyTranslucentSelfShadowUniformBuffer;
