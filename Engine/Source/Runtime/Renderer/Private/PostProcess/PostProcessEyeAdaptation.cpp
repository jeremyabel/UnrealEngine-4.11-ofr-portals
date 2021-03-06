// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	PostProcessEyeAdaptation.cpp: Post processing eye adaptation implementation.
=============================================================================*/

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneFilterRendering.h"
#include "PostProcessEyeAdaptation.h"
#include "PostProcessing.h"
#include "SceneUtils.h"


/**
 *   Shared functionality used in computing the eye-adaptation parameters
 *   Compute the parameters used for eye-adaptation.  These will default to values
 *   that disable eye-adaptation if the hardware doesn't support the minimum feature level
 */
inline static void ComputeEyeAdaptationValues(const ERHIFeatureLevel::Type MinFeatureLevel, const FViewInfo& View, FVector4 Out[3])
{
	const FPostProcessSettings& Settings = View.FinalPostProcessSettings;
	const FEngineShowFlags& EngineShowFlags = View.Family->EngineShowFlags;

	float EyeAdaptationMin = Settings.AutoExposureMinBrightness;
	float EyeAdaptationMax = Settings.AutoExposureMaxBrightness;

	// FLT_MAX means no override
	float LocalOverrideExposure = FLT_MAX;

	// Eye adaptation is disabled except for highend right now because the histogram is not computed.
	if (!EngineShowFlags.EyeAdaptation || View.GetFeatureLevel() < MinFeatureLevel)
	{
		LocalOverrideExposure = 0;
	}


	float LocalExposureMultipler = FMath::Pow(2.0f, Settings.AutoExposureBias);

	if (View.Family->ExposureSettings.bFixed)
	{
		// editor wants to override the setting with it's own fixed setting
		LocalOverrideExposure = View.Family->ExposureSettings.LogOffset;
		LocalExposureMultipler = 1;
	}

	if (LocalOverrideExposure != FLT_MAX)
	{
		// set the eye adaptation to a fixed value
		EyeAdaptationMin = EyeAdaptationMax = FMath::Pow(2.0f, -LocalOverrideExposure);
	}

	if (EyeAdaptationMin > EyeAdaptationMax)
	{
		EyeAdaptationMin = EyeAdaptationMax;
	}

	float LowPercent = FMath::Clamp(Settings.AutoExposureLowPercent, 1.0f, 99.0f) * 0.01f;
	float HighPercent = FMath::Clamp(Settings.AutoExposureHighPercent, 1.0f, 99.0f) * 0.01f;

	if (LowPercent > HighPercent)
	{
		LowPercent = HighPercent;
	}

	Out[0] = FVector4(LowPercent, HighPercent, EyeAdaptationMin, EyeAdaptationMax);

	// ----------

	Out[1] = FVector4(LocalExposureMultipler, View.Family->DeltaWorldTime, Settings.AutoExposureSpeedUp, Settings.AutoExposureSpeedDown);

	// ----------

	// example min/max: -8 .. 4   means a range from 1/256 to 4  pow(2,-8) .. pow(2,4)
	float HistogramLogMin = Settings.HistogramLogMin;
	float HistogramLogMax = Settings.HistogramLogMax;

	float DeltaLog = HistogramLogMax - HistogramLogMin;
	float Multiply = 1.0f / DeltaLog;
	float Add = -HistogramLogMin * Multiply;
	float MinIntensity = exp2(HistogramLogMin);
	Out[2] = FVector4(Multiply, Add, MinIntensity, 0);
}

// Basic AutoExposure requires at least ES3_1
static ERHIFeatureLevel::Type BasicEyeAdaptationMinFeatureLevel = ERHIFeatureLevel::ES3_1;

// Initialize the static CVar
TAutoConsoleVariable<int32> CVarEyeAdaptationMethodOveride(
	TEXT("r.EyeAdaptation.MethodOveride"),
	-1,
	TEXT("-1: no override\n")
	TEXT("1: Histogram-based. \n")
	TEXT("2: Basic \n"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

/** Encapsulates the histogram-based post processing eye adaptation pixel shader. */
class FPostProcessEyeAdaptationPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPostProcessEyeAdaptationPS, Global);

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM4);
	}

	static void ModifyCompilationEnvironment( EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment )
	{
		FGlobalShader::ModifyCompilationEnvironment( Platform, OutEnvironment );
		OutEnvironment.SetRenderTargetOutputFormat(0, PF_A32B32G32R32F);
	}

	/** Default constructor. */
	FPostProcessEyeAdaptationPS() {}

public:
	FPostProcessPassParameters PostprocessParameter;
	FShaderParameter EyeAdaptationParams;

	/** Initialization constructor. */
	FPostProcessEyeAdaptationPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		PostprocessParameter.Bind(Initializer.ParameterMap);
		EyeAdaptationParams.Bind(Initializer.ParameterMap, TEXT("EyeAdaptationParams"));
	}

	void SetPS(const FRenderingCompositePassContext& Context)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();

		FGlobalShader::SetParameters(Context.RHICmdList, ShaderRHI, Context.View);

		PostprocessParameter.SetPS(ShaderRHI, Context, TStaticSamplerState<SF_Bilinear,AM_Clamp,AM_Clamp,AM_Clamp>::GetRHI());

		{
			FVector4 Temp[3];

			FRCPassPostProcessEyeAdaptation::ComputeEyeAdaptationParamsValue(Context.View, Temp);
			SetShaderValueArray(Context.RHICmdList, ShaderRHI, EyeAdaptationParams, Temp, 3);
		}
	}

	// FShader interface.
	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << PostprocessParameter << EyeAdaptationParams;
		return bShaderHasOutdatedParameters;
	}
};

IMPLEMENT_SHADER_TYPE(,FPostProcessEyeAdaptationPS,TEXT("PostProcessEyeAdaptation"),TEXT("MainPS"),SF_Pixel);

void FRCPassPostProcessEyeAdaptation::Process(FRenderingCompositePassContext& Context)
{
	SCOPED_DRAW_EVENT(Context.RHICmdList, PostProcessEyeAdaptation);

	const FSceneView& View = Context.View;
	const FSceneViewFamily& ViewFamily = *(View.Family);

	IPooledRenderTarget* EyeAdaptation = Context.View.GetEyeAdaptation(Context.RHICmdList);
	check(EyeAdaptation);

	FIntPoint DestSize = EyeAdaptation->GetDesc().Extent;

	// we render to our own output render target, not the intermediate one created by the compositing system
	// Set the view family's render target/viewport.
	SetRenderTarget(Context.RHICmdList, EyeAdaptation->GetRenderTargetItem().TargetableTexture, FTextureRHIRef(), true);
	Context.SetViewportAndCallRHI(0, 0, 0.0f, DestSize.X, DestSize.Y, 1.0f );

	// set the state
	Context.RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
	Context.RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());
	Context.RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

	TShaderMapRef<FPostProcessVS> VertexShader(Context.GetShaderMap());
	TShaderMapRef<FPostProcessEyeAdaptationPS> PixelShader(Context.GetShaderMap());

	static FGlobalBoundShaderState BoundShaderState;
	

	SetGlobalBoundShaderState(Context.RHICmdList, Context.GetFeatureLevel(), BoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader);

	PixelShader->SetPS(Context);

	// Draw a quad mapping scene color to the view's render target
	DrawRectangle(
		Context.RHICmdList,
		0, 0,
		DestSize.X, DestSize.Y,
		0, 0,
		DestSize.X, DestSize.Y,
		DestSize,
		DestSize,
		*VertexShader,
		EDRF_UseTriangleOptimization);

	Context.RHICmdList.CopyToResolveTarget(EyeAdaptation->GetRenderTargetItem().TargetableTexture, EyeAdaptation->GetRenderTargetItem().ShaderResourceTexture, false, FResolveParams());

	Context.View.SetValidEyeAdaptation();
}

void FRCPassPostProcessEyeAdaptation::ComputeEyeAdaptationParamsValue(const FViewInfo& View, FVector4 Out[3])
{
	ComputeEyeAdaptationValues(ERHIFeatureLevel::SM5, View, Out);
}

float FRCPassPostProcessEyeAdaptation::ComputeExposureScaleValue(const FViewInfo& View)
{
	FVector4 EyeAdaptationParams[3];

	FRCPassPostProcessEyeAdaptation::ComputeEyeAdaptationParamsValue(View, EyeAdaptationParams);
	
	// like in PostProcessEyeAdaptation.usf
	float Exposure = (EyeAdaptationParams[0].Z + EyeAdaptationParams[0].W) * 0.5f;
	float ExposureScale = 1.0f / FMath::Max(0.0001f, Exposure);

	float ExposureOffsetMultipler = EyeAdaptationParams[1].X;

	return ExposureScale * ExposureOffsetMultipler;
}

FPooledRenderTargetDesc FRCPassPostProcessEyeAdaptation::ComputeOutputDesc(EPassOutputId InPassOutputId) const
{
	// Specify invalid description to avoid getting intermediate rendertargets created.
	// We want to use ViewState->GetEyeAdaptation() instead
	FPooledRenderTargetDesc Ret;

	Ret.DebugName = TEXT("EyeAdaptation");

	return Ret;
}


/** Encapsulates the post process computation of Log2 Luminance pixel shader. */
class FPostProcessBasicEyeAdaptationSetupPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPostProcessBasicEyeAdaptationSetupPS, Global);

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, BasicEyeAdaptationMinFeatureLevel);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
	}

	/** Default constructor. */
	FPostProcessBasicEyeAdaptationSetupPS() {}

public:
	FPostProcessPassParameters PostprocessParameter;
	FShaderParameter EyeAdaptationParams;

	/** Initialization constructor. */
	FPostProcessBasicEyeAdaptationSetupPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		PostprocessParameter.Bind(Initializer.ParameterMap);
		EyeAdaptationParams.Bind(Initializer.ParameterMap, TEXT("EyeAdaptationParams"));
	}

	void SetPS(const FRenderingCompositePassContext& Context)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();

		FGlobalShader::SetParameters(Context.RHICmdList, ShaderRHI, Context.View);
		PostprocessParameter.SetPS(ShaderRHI, Context, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());

		{
			FVector4 Temp[3];

			ComputeEyeAdaptationValues(BasicEyeAdaptationMinFeatureLevel, Context.View, Temp);
			SetShaderValueArray(Context.RHICmdList, ShaderRHI, EyeAdaptationParams, Temp, 3);
		}
	}

	// FShader interface.
	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << PostprocessParameter << EyeAdaptationParams;
		return bShaderHasOutdatedParameters;
	}
};

IMPLEMENT_SHADER_TYPE(, FPostProcessBasicEyeAdaptationSetupPS, TEXT("PostProcessEyeAdaptation"), TEXT("MainBasicEyeAdaptationSetupPS"), SF_Pixel);

void FRCPassPostProcessBasicEyeAdaptationSetUp::Process(FRenderingCompositePassContext& Context)
{
	const FPooledRenderTargetDesc* InputDesc = GetInputDesc(ePId_Input0);

	if (!InputDesc)
	{
		// input is not hooked up correctly
		return;
	}

	const FSceneView& View = Context.View;
	const FSceneViewFamily& ViewFamily = *(View.Family);

	FIntPoint SrcSize = InputDesc->Extent;
	FIntPoint DestSize = PassOutputs[0].RenderTargetDesc.Extent;

	// e.g. 4 means the input texture is 4x smaller than the buffer size
	uint32 ScaleFactor = FSceneRenderTargets::Get(Context.RHICmdList).GetBufferSizeXY().X / SrcSize.X;

	FIntRect SrcRect = View.ViewRect / ScaleFactor;
	FIntRect DestRect = SrcRect;

	SCOPED_DRAW_EVENTF(Context.RHICmdList, PostProcessBasicEyeAdaptationSetup, TEXT("PostProcessBasicEyeAdaptationSetup %dx%d"), DestRect.Width(), DestRect.Height());

	const FSceneRenderTargetItem& DestRenderTarget = PassOutputs[0].RequestSurface(Context);

	// Set the view family's render target/viewport.
	SetRenderTarget(Context.RHICmdList, DestRenderTarget.TargetableTexture, FTextureRHIRef());

	// is optimized away if possible (RT size=view size, )
	Context.RHICmdList.Clear(true, FLinearColor::Black, false, 1.0f, false, 0, DestRect);

	Context.SetViewportAndCallRHI(0, 0, 0.0f, DestSize.X, DestSize.Y, 1.0f);

	// set the state
	Context.RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
	Context.RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());
	Context.RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

	TShaderMapRef<FPostProcessVS> VertexShader(Context.GetShaderMap());
	TShaderMapRef<FPostProcessBasicEyeAdaptationSetupPS> PixelShader(Context.GetShaderMap());

	static FGlobalBoundShaderState BoundShaderState;

	SetGlobalBoundShaderState(Context.RHICmdList, Context.GetFeatureLevel(), BoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader);

	PixelShader->SetPS(Context);

	DrawPostProcessPass(
		Context.RHICmdList,
		DestRect.Min.X, DestRect.Min.Y,
		DestRect.Width(), DestRect.Height(),
		SrcRect.Min.X, SrcRect.Min.Y,
		SrcRect.Width(), SrcRect.Height(),
		DestSize,
		SrcSize,
		*VertexShader,
		View.StereoPass,
		Context.HasHmdMesh(),
		EDRF_UseTriangleOptimization);

	Context.RHICmdList.CopyToResolveTarget(DestRenderTarget.TargetableTexture, DestRenderTarget.ShaderResourceTexture, false, FResolveParams());
}

FPooledRenderTargetDesc FRCPassPostProcessBasicEyeAdaptationSetUp::ComputeOutputDesc(EPassOutputId InPassOutputId) const
{
	FPooledRenderTargetDesc Ret = GetInput(ePId_Input0)->GetOutput()->RenderTargetDesc;

	Ret.Reset();
	Ret.DebugName = TEXT("EyeAdaptationBasicSetup");
	// Require alpha channel for log2 information.
	Ret.Format = PF_FloatRGBA;
	return Ret;
}



/** Encapsulates the post process computation of the exposure scale  pixel shader. */
class FPostProcessLogLuminance2ExposureScalePS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPostProcessLogLuminance2ExposureScalePS, Global);



public:
	/** Default constructor. */
	FPostProcessLogLuminance2ExposureScalePS() {}

	/** Initialization constructor. */
	FPostProcessLogLuminance2ExposureScalePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		PostprocessParameter.Bind(Initializer.ParameterMap);
		EyeAdaptationTexture.Bind(Initializer.ParameterMap, TEXT("EyeAdaptationTexture"));
		EyeAdaptationParams.Bind(Initializer.ParameterMap, TEXT("EyeAdaptationParams"));
	}

public:

	/** Static Shader boilerplate */
	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, BasicEyeAdaptationMinFeatureLevel);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
		OutEnvironment.SetRenderTargetOutputFormat(0, PF_A32B32G32R32F);
	}


	void SetPS(const FRenderingCompositePassContext& Context, IPooledRenderTarget* EyeAdaptationLastFrameRT)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();

		FGlobalShader::SetParameters(Context.RHICmdList, ShaderRHI, Context.View);

		PostprocessParameter.SetPS(ShaderRHI, Context, TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());

		// Associate the eye adaptation buffer from the previous frame with a texture to be read in this frame. 
		
		if (Context.View.HasValidEyeAdaptation())
		{
			SetTextureParameter(Context.RHICmdList, ShaderRHI, EyeAdaptationTexture, EyeAdaptationLastFrameRT->GetRenderTargetItem().TargetableTexture);
		}
		else
		{
			// some views don't have a state, thumbnail rendering?
			SetTextureParameter(Context.RHICmdList, ShaderRHI, EyeAdaptationTexture, GWhiteTexture->TextureRHI);
		}

		// Pack the eye adaptation parameters for the shader
		{
			FVector4 Temp[3];
			// static computation function
			ComputeEyeAdaptationValues(BasicEyeAdaptationMinFeatureLevel, Context.View, Temp);
			// Log-based computation of the exposure scale has a built in scaling.
			//Temp[1].X *= 0.16;  
			SetShaderValueArray(Context.RHICmdList, ShaderRHI, EyeAdaptationParams, Temp, 3);
		}
	}

	// FShader interface.
	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << PostprocessParameter << EyeAdaptationTexture << EyeAdaptationParams;
		return bShaderHasOutdatedParameters;
	}

private:
	FPostProcessPassParameters PostprocessParameter;
	FShaderResourceParameter EyeAdaptationTexture;
	FShaderParameter EyeAdaptationParams;
};

IMPLEMENT_SHADER_TYPE(, FPostProcessLogLuminance2ExposureScalePS, TEXT("PostProcessEyeAdaptation"), TEXT("MainLogLuminance2ExposureScalePS"), SF_Pixel);

void FRCPassPostProcessBasicEyeAdaptation::Process(FRenderingCompositePassContext& Context)
{
	SCOPED_DRAW_EVENT(Context.RHICmdList, PostProcessBasicEyeAdaptation);

	const FSceneView& View = Context.View;
	const FSceneViewFamily& ViewFamily = *(View.Family);

	// Get the custom 1x1 target used to store exposure value and Toggle the two render targets used to store new and old.
	Context.View.SwapEyeAdaptationRTs();
	IPooledRenderTarget* EyeAdaptationThisFrameRT = Context.View.GetEyeAdaptationRT(Context.RHICmdList);
	IPooledRenderTarget* EyeAdaptationLastFrameRT = Context.View.GetLastEyeAdaptationRT(Context.RHICmdList);

	check(EyeAdaptationThisFrameRT && EyeAdaptationLastFrameRT);

	FIntPoint DestSize = EyeAdaptationThisFrameRT->GetDesc().Extent;

	// we render to our own output render target, not the intermediate one created by the compositing system
	// Set the view family's render target/viewport.
	SetRenderTarget(Context.RHICmdList, EyeAdaptationThisFrameRT->GetRenderTargetItem().TargetableTexture, FTextureRHIRef(), true);
	Context.SetViewportAndCallRHI(0, 0, 0.0f, DestSize.X, DestSize.Y, 1.0f);

	// set the state
	Context.RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
	Context.RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());
	Context.RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

	TShaderMapRef<FPostProcessVS> VertexShader(Context.GetShaderMap());
	TShaderMapRef<FPostProcessLogLuminance2ExposureScalePS> PixelShader(Context.GetShaderMap());

	static FGlobalBoundShaderState BoundShaderState;


	SetGlobalBoundShaderState(Context.RHICmdList, Context.GetFeatureLevel(), BoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader);

	// Set the parameters used by the pixel shader.

	PixelShader->SetPS(Context, EyeAdaptationLastFrameRT);

	// Draw a quad mapping scene color to the view's render target
	DrawRectangle(
		Context.RHICmdList,
		0, 0,
		DestSize.X, DestSize.Y,
		0, 0,
		DestSize.X, DestSize.Y,
		DestSize,
		DestSize,
		*VertexShader,
		EDRF_UseTriangleOptimization);

	Context.RHICmdList.CopyToResolveTarget(EyeAdaptationThisFrameRT->GetRenderTargetItem().TargetableTexture, EyeAdaptationThisFrameRT->GetRenderTargetItem().ShaderResourceTexture, false, FResolveParams());

	Context.View.SetValidEyeAdaptation();
}


FPooledRenderTargetDesc FRCPassPostProcessBasicEyeAdaptation::ComputeOutputDesc(EPassOutputId InPassOutputId) const
{
	// Specify invalid description to avoid getting intermediate rendertargets created.
	// We want to use ViewState->GetEyeAdaptation() instead
	FPooledRenderTargetDesc Ret;

	Ret.DebugName = TEXT("EyeAdaptationBasic");

	return Ret;
}

/** Encapsulates the post processing reduction pixel shader. */
class FPostProcessReductionPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPostProcessReductionPS, Global);
public:

	/** Default constructor. */
	FPostProcessReductionPS() {}

	/** Initialization constructor. */
	FPostProcessReductionPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		PostprocessParameter.Bind(Initializer.ParameterMap);
		DeferredParameters.Bind(Initializer.ParameterMap);
	}

	// FShader interface.
	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << PostprocessParameter << DeferredParameters;
		return bShaderHasOutdatedParameters;
	}

	void SetParameters(const FRenderingCompositePassContext& Context, const FPooledRenderTargetDesc* InputDesc)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();

		FGlobalShader::SetParameters(Context.RHICmdList, ShaderRHI, Context.View);
		DeferredParameters.Set(Context.RHICmdList, ShaderRHI, Context.View);

		// filter only if needed for better performance
		FSamplerStateRHIParamRef Filter = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		PostprocessParameter.SetPS(ShaderRHI, Context, Filter);
	}

public:

	/**Static Shader boilerplate */
	static bool ShouldCache(EShaderPlatform Platform)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
	}


private:

	FPostProcessPassParameters PostprocessParameter;
	FDeferredPixelShaderParameters DeferredParameters;
};

IMPLEMENT_SHADER_TYPE(, FPostProcessReductionPS, TEXT("PostProcessEyeAdaptation"), TEXT("MainReductionPS"), SF_Pixel);


/** Encapsulates the post processing down sample vertex shader. */
class FPostProcessReductionVS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPostProcessReductionVS, Global);
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return true;
	}

	/** Default constructor. */
	FPostProcessReductionVS() {}

	/** Initialization constructor. */
	FPostProcessReductionVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
	}

	/** Serializer */
	virtual bool Serialize(FArchive& Ar) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		return bShaderHasOutdatedParameters;
	}

	void SetParameters(const FRenderingCompositePassContext& Context)
	{
		const FVertexShaderRHIParamRef ShaderRHI = GetVertexShader();

		FGlobalShader::SetParameters(Context.RHICmdList, ShaderRHI, Context.View);

		const FPooledRenderTargetDesc* InputDesc = Context.Pass->GetInputDesc(ePId_Input0);

		if (!InputDesc)
		{
			// input is not hooked up correctly
			return;
		}
	}
};

IMPLEMENT_SHADER_TYPE(, FPostProcessReductionVS, TEXT("PostProcessEyeAdaptation"), TEXT("MainReductionVS"), SF_Vertex);


void FRCPassPostProcessExposureReduction::Process(FRenderingCompositePassContext& Context)
{
	const FPooledRenderTargetDesc* InputDesc = GetInputDesc(ePId_Input0);

	if (!InputDesc)
	{
		// input is not hooked up correctly
		return;
	}

	const FSceneView& View = Context.View;
	const FSceneViewFamily& ViewFamily = *(View.Family);

	FIntPoint SrcSize = InputDesc->Extent;
	FIntPoint DestSize = PassOutputs[0].RenderTargetDesc.Extent;

	// e.g. 4 means the input texture is 4x smaller than the buffer size
	uint32 ScaleFactor = FSceneRenderTargets::Get(Context.RHICmdList).GetBufferSizeXY().X / SrcSize.X;

	FIntRect SrcRect = View.ViewRect / ScaleFactor;
	FIntRect DestRect = SrcRect / 2;  // FIntRect::DivideAndRoundUp(SrcRect, 2);

	SCOPED_DRAW_EVENTF(Context.RHICmdList, Reduction, TEXT("Eye Reduction %dx%d"), DestRect.Width(), DestRect.Height());

	const FSceneRenderTargetItem& DestRenderTarget = PassOutputs[0].RequestSurface(Context);

	// Set the view family's render target/viewport.
	SetRenderTarget(Context.RHICmdList, DestRenderTarget.TargetableTexture, FTextureRHIRef(), ESimpleRenderTargetMode::EExistingColorAndDepth);

	Context.SetViewportAndCallRHI(0, 0, 0.0f, DestSize.X, DestSize.Y, 1.0f);

	// set the state
	Context.RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
	Context.RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());
	Context.RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());


	auto ShaderMap = Context.GetShaderMap();
	TShaderMapRef<FPostProcessReductionVS> VertexShader(ShaderMap);
	TShaderMapRef<FPostProcessReductionPS> PixelShader(ShaderMap);

	static FGlobalBoundShaderState BoundShaderState;

	SetGlobalBoundShaderState(Context.RHICmdList, Context.GetFeatureLevel(), BoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader);

	PixelShader->SetParameters(Context, InputDesc);
	VertexShader->SetParameters(Context);

	// check if we have to clear the whole surface.
	// Otherwise perform the clear when the dest rectangle has been computed.
	auto FeatureLevel = Context.View.GetFeatureLevel();
	if (FeatureLevel == ERHIFeatureLevel::ES2 || FeatureLevel == ERHIFeatureLevel::ES3_1)
	{
		Context.RHICmdList.Clear(true, FLinearColor(0, 0, 0, 0), false, 1.0f, false, 0, FIntRect());
	}
	else
	{
		Context.RHICmdList.Clear(true, FLinearColor(0, 0, 0, 0), false, 1.0f, false, 0, DestRect);
	}

	DrawPostProcessPass(
		Context.RHICmdList,
		DestRect.Min.X, DestRect.Min.Y,
		DestRect.Width(), DestRect.Height(),
		SrcRect.Min.X, SrcRect.Min.Y,
		SrcRect.Width(), SrcRect.Height(),
		DestSize,
		SrcSize,
		*VertexShader,
		View.StereoPass,
		Context.HasHmdMesh(),
		EDRF_UseTriangleOptimization);

	Context.RHICmdList.CopyToResolveTarget(DestRenderTarget.TargetableTexture, DestRenderTarget.ShaderResourceTexture, false, FResolveParams());
}

FPooledRenderTargetDesc FRCPassPostProcessExposureReduction::ComputeOutputDesc(EPassOutputId InPassOutputId) const
{
	FPooledRenderTargetDesc Ret = GetInput(ePId_Input0)->GetOutput()->RenderTargetDesc;

	// Reset the format information.
	Ret.Reset();

	// Down sample by a factor of 2.
	// NB: the standard PostProcessDownsample would round up the extent, 
	// introducing a black vignette which we avoid here.
	Ret.Extent /= int32(2);

	Ret.Extent.X = FMath::Max(1, Ret.Extent.X);
	Ret.Extent.Y = FMath::Max(1, Ret.Extent.Y);

	Ret.Format = PF_FloatRGBA;

	Ret.TargetableFlags &= ~TexCreate_UAV;
	Ret.TargetableFlags |= TexCreate_RenderTargetable;
	Ret.DebugName = DebugName;

	return Ret;
}