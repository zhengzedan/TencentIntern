
#include "ScreenSpaceShadows.h"

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

IMPLEMENT_GLOBAL_SHADER(FScreenSpaceShadowsCS, "/Engine/Private/ScreenSpaceShadowsCS.usf", "Main", SF_Compute);
const int32 GScreenSpaceShadowsTileSizeX = 8;
const int32 GScreenSpaceShadowsTileSizeY = 8;