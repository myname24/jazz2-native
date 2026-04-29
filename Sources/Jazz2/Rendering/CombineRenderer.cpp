#include "CombineRenderer.h"
#include "PlayerViewport.h"
#include "../PreferencesCache.h"

#include "../../nCine/Graphics/RenderQueue.h"

#include <cmath>
#include <algorithm>

namespace Jazz2::Rendering
{
#if !defined(RHI_CAP_SHADERS)
	/// Samples a pixel from a texture at integer coordinates, clamped to bounds
	static inline void SampleTexel(const RHI::Texture* tex, std::int32_t x, std::int32_t y, std::uint8_t out[4])
	{
		const std::uint8_t* pixels = tex->GetPixels(0);
		std::int32_t w = tex->GetWidth();
		std::int32_t h = tex->GetHeight();
		if (x < 0) x = 0; else if (x >= w) x = w - 1;
		if (y < 0) y = 0; else if (y >= h) y = h - 1;
		const std::uint8_t* src = pixels + (y * w + x) * 4;
		out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; out[3] = src[3];
	}

	/// SW fragment shader implementing the combine pass (lighting + water)
	static void FragmentCombine(const RHI::FragmentShaderInput& input)
	{
		std::uint8_t* rgba = input.rgba;
		auto* data = static_cast<const CombineShaderData*>(input.userData);
		const RHI::Texture* lightTex = input.textures[1];
		if (lightTex == nullptr) return;

		std::int32_t texH = lightTex->GetHeight();
		float yNorm = input.v;
		bool isBelowWater = data->hasWater && (yNorm > data->waterLevelNorm);

		float mainR = rgba[0] * (1.0f / 255.0f);
		float mainG = rgba[1] * (1.0f / 255.0f);
		float mainB = rgba[2] * (1.0f / 255.0f);

		// Water displacement: re-sample view texture at displaced X
		if (isBelowWater) {
			float invW = 1.0f / (float)input.texWidth;
			float uvWorldY = yNorm + data->camY * invW;
			float offset = 0.008f * std::sin(data->time * 16.0f + uvWorldY * 20.0f);
			std::int32_t srcX = (std::int32_t)((float)input.x + offset * (float)input.texWidth + 0.5f);
			const RHI::Texture* viewTex = input.textures[0];
			if (viewTex != nullptr) {
				std::uint8_t displaced[4];
				// TODO: Y-coords need to be flipped when accessing pixels
				SampleTexel(viewTex, srcX, texH - input.y - 1, displaced);
				mainR = displaced[0] * (1.0f / 255.0f);
				mainG = displaced[1] * (1.0f / 255.0f);
				mainB = displaced[2] * (1.0f / 255.0f);
			}
		}

		// Sample lighting texture at the same position
		std::uint8_t lightSample[4];
		// TODO: Y-coords need to be flipped when accessing pixels
		SampleTexel(lightTex, input.x, texH - input.y - 1, lightSample);
		float lightR = lightSample[0] * (1.0f / 255.0f);
		float lightG = lightSample[1] * (1.0f / 255.0f);

		// Water color tinting
		if (isBelowWater) {
			mainR = mainR * 0.7f + 0.4f * 0.3f;
			mainG = mainG * 0.7f + 0.6f * 0.3f;
			mainB = mainB * 0.7f + 0.8f * 0.3f;

			float invH = 1.0f / (float)input.texHeight;
			float topDist = std::abs(yNorm - data->waterLevelNorm);
			if (topDist < invH * 2.0f) {
				mainR = std::min(mainR + 0.2f, 1.0f);
				mainG = std::min(mainG + 0.2f, 1.0f);
				mainB = std::min(mainB + 0.2f, 1.0f);
			}
		}

		// Combine formula (simplified, no blur):
		// lit = main * (1 + light.g) + max(light.g - 0.7, 0)
		float litR = mainR * (1.0f + lightG) + std::max(lightG - 0.7f, 0.0f);
		float litG = mainG * (1.0f + lightG) + std::max(lightG - 0.7f, 0.0f);
		float litB = mainB * (1.0f + lightG) + std::max(lightG - 0.7f, 0.0f);

		// Darkness: darken lit toward black, then mix with ambient
		float darkness = (1.0f - lightR) * data->invDarknessDiv;
		if (darkness > 1.0f) darkness = 1.0f;
		else if (darkness < 0.0f) darkness = 0.0f;
		float darkened_R = litR * (1.0f - darkness);
		float darkened_G = litG * (1.0f - darkness);
		float darkened_B = litB * (1.0f - darkness);

		float ambientStrength = 1.0f - lightR;

		// Water: extra darkness above water surface
		if (data->hasWater && data->waterLevelNorm < 0.4f && !isBelowWater) {
			float aboveWaterDarkness = 0.4f - data->waterLevelNorm;
			ambientStrength = std::min(1.0f, ambientStrength + aboveWaterDarkness);
		}

		float outR = darkened_R * (1.0f - ambientStrength) + data->ambientR * ambientStrength;
		float outG = darkened_G * (1.0f - ambientStrength) + data->ambientG * ambientStrength;
		float outB = darkened_B * (1.0f - ambientStrength) + data->ambientB * ambientStrength;

		rgba[0] = (std::uint8_t)(std::min(outR, 1.0f) * 255.0f + 0.5f);
		rgba[1] = (std::uint8_t)(std::min(outG, 1.0f) * 255.0f + 0.5f);
		rgba[2] = (std::uint8_t)(std::min(outB, 1.0f) * 255.0f + 0.5f);
		rgba[3] = 255;
	}
#endif

	CombineRenderer::CombineRenderer(PlayerViewport* owner)
		: _owner(owner)
	{
		setVisitOrderState(SceneNode::VisitOrderState::Disabled);
	}
		
	void CombineRenderer::Initialize(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
	{
		_bounds = Rectf(static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height));

#if !defined(RHI_CAP_SHADERS) || !defined(RHI_CAP_FRAMEBUFFERS)
		// Blur post-processing requires shader support and framebuffers
		_renderCommand.SetTransformation(Matrix4x4f::Translation((float)x, (float)y, 0.0f));
#else
		if (_renderCommand.GetMaterial().SetShader(_owner->_levelHandler->_combineShader)) {
			_renderCommand.GetMaterial().ReserveUniformsDataMemory();
			_renderCommand.GetGeometry().SetDrawParameters(RHI::PrimitiveType::TriangleStrip, 0, 4);
			auto* textureUniform = _renderCommand.GetMaterial().Uniform(Material::TextureUniformName);
			if (textureUniform && textureUniform->GetIntValue(0) != 0) {
				textureUniform->SetIntValue(0); // GL_TEXTURE0
			}
			auto* lightTexUniform = _renderCommand.GetMaterial().Uniform("uTextureLighting");
			if (lightTexUniform && lightTexUniform->GetIntValue(0) != 1) {
				lightTexUniform->SetIntValue(1); // GL_TEXTURE1
			}
			auto* blurHalfTexUniform = _renderCommand.GetMaterial().Uniform("uTextureBlurHalf");
			if (blurHalfTexUniform && blurHalfTexUniform->GetIntValue(0) != 2) {
				blurHalfTexUniform->SetIntValue(2); // GL_TEXTURE2
			}
			auto* blurQuarterTexUniform = _renderCommand.GetMaterial().Uniform("uTextureBlurQuarter");
			if (blurQuarterTexUniform && blurQuarterTexUniform->GetIntValue(0) != 3) {
				blurQuarterTexUniform->SetIntValue(3); // GL_TEXTURE3
			}
		}

		if (_renderCommandWithWater.GetMaterial().SetShader(_owner->_levelHandler->_combineWithWaterShader)) {
			_renderCommandWithWater.GetMaterial().ReserveUniformsDataMemory();
			_renderCommandWithWater.GetGeometry().SetDrawParameters(RHI::PrimitiveType::TriangleStrip, 0, 4);
			auto* textureUniform = _renderCommandWithWater.GetMaterial().Uniform(Material::TextureUniformName);
			if (textureUniform && textureUniform->GetIntValue(0) != 0) {
				textureUniform->SetIntValue(0); // GL_TEXTURE0
			}
			auto* lightTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureLighting");
			if (lightTexUniform && lightTexUniform->GetIntValue(0) != 1) {
				lightTexUniform->SetIntValue(1); // GL_TEXTURE1
			}
			auto* blurHalfTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureBlurHalf");
			if (blurHalfTexUniform && blurHalfTexUniform->GetIntValue(0) != 2) {
				blurHalfTexUniform->SetIntValue(2); // GL_TEXTURE2
			}
			auto* blurQuarterTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureBlurQuarter");
			if (blurQuarterTexUniform && blurQuarterTexUniform->GetIntValue(0) != 3) {
				blurQuarterTexUniform->SetIntValue(3); // GL_TEXTURE3
			}
			auto* noiseTexUniform = _renderCommandWithWater.GetMaterial().Uniform("uTextureNoise");
			if (noiseTexUniform && noiseTexUniform->GetIntValue(0) != 4) {
				noiseTexUniform->SetIntValue(4); // GL_TEXTURE4
			}
		}

		_renderCommand.SetTransformation(Matrix4x4f::Translation((float)x, (float)y, 0.0f));
		_renderCommandWithWater.SetTransformation(Matrix4x4f::Translation((float)x, (float)y, 0.0f));
#endif
	}

	Rectf CombineRenderer::GetBounds() const
	{
		return _bounds;
	}

	bool CombineRenderer::OnDraw(RenderQueue& renderQueue)
	{
#if !defined(RHI_CAP_SHADERS) || !defined(RHI_CAP_FRAMEBUFFERS)
		// SW fallback: set up blit with extended fragment shader for lighting+water combine
		if (_renderCommand.GetMaterial().SetShaderProgramType(Material::ShaderProgramType::Sprite)) {
			_renderCommand.GetMaterial().ReserveUniformsDataMemory();
			_renderCommand.GetGeometry().SetDrawParameters(RHI::PrimitiveType::TriangleStrip, 0, 4);
		}
		_renderCommand.GetMaterial().SetInstTexRect(1.0f, 0.0f, 1.0f, 0.0f);
		_renderCommand.GetMaterial().SetInstSpriteSize(_bounds.W, _bounds.H);
		_renderCommand.GetMaterial().SetInstColor(1.0f, 1.0f, 1.0f, 1.0f);
		_renderCommand.GetMaterial().SetTexture(*_owner->_viewTexture);
		_renderCommand.GetMaterial().SetTexture(1, *_owner->_lightingBuffer);

		// Populate combine shader uniform data
		float viewWaterLevel = _owner->_levelHandler->_waterLevel - _owner->_cameraPos.Y + _bounds.H * 0.5f;
		_combineData.ambientR = _owner->_ambientLight.X;
		_combineData.ambientG = _owner->_ambientLight.Y;
		_combineData.ambientB = _owner->_ambientLight.Z;
		_combineData.ambientW = _owner->_ambientLight.W;
		_combineData.invDarknessDiv = 1.0f / std::sqrt(std::max(_combineData.ambientW, 0.35f));
		_combineData.hasWater = (viewWaterLevel < _bounds.H);
		_combineData.waterLevelNorm = viewWaterLevel / _bounds.H;
		_combineData.time = _owner->_levelHandler->_elapsedFrames * 0.0018f;
		_combineData.camY = _owner->_cameraPos.Y;

		_renderCommand.GetMaterial().SetFragmentShader(FragmentCombine, &_combineData);
		renderQueue.AddCommand(&_renderCommand);
		return true;
#else
		float viewWaterLevel = _owner->_levelHandler->_waterLevel - _owner->_cameraPos.Y + _bounds.H * 0.5f;
		bool viewHasWater = (viewWaterLevel < _bounds.H);
		auto& command = (viewHasWater ? _renderCommandWithWater : _renderCommand);

		command.GetMaterial().SetTexture(0, *_owner->_viewTexture);
		command.GetMaterial().SetTexture(1, *_owner->_lightingBuffer);
		if (PreferencesCache::BlurEffects) {
			command.GetMaterial().SetTexture(2, *_owner->_blurPass2.GetTarget());
			command.GetMaterial().SetTexture(3, *_owner->_blurPass4.GetTarget());
		} else {
			command.GetMaterial().SetTexture(2, nullptr);
			command.GetMaterial().SetTexture(3, nullptr);
		}
		if (viewHasWater && !PreferencesCache::LowWaterQuality) {
			command.GetMaterial().SetTexture(4, *_owner->_levelHandler->_noiseTexture);
		}

		auto* instanceBlock = command.GetMaterial().UniformBlock(Material::InstanceBlockName);
		instanceBlock->GetUniform(Material::TexRectUniformName)->SetFloatValue(1.0f, 0.0f, 1.0f, 0.0f);
		instanceBlock->GetUniform(Material::SpriteSizeUniformName)->SetFloatValue(_bounds.W, _bounds.H);
		instanceBlock->GetUniform(Material::ColorUniformName)->SetFloatVector(Colorf::White.Data());

		command.GetMaterial().Uniform("uAmbientColor")->SetFloatVector(_owner->_ambientLight.Data());
		command.GetMaterial().Uniform("uTime")->SetFloatValue(_owner->_levelHandler->_elapsedFrames * 0.0018f);

		if (viewHasWater) {
			command.GetMaterial().Uniform("uWaterLevel")->SetFloatValue(viewWaterLevel / _bounds.H);
			command.GetMaterial().Uniform("uCameraPos")->SetFloatVector(_owner->_cameraPos.Data());
		}

		renderQueue.AddCommand(&command);
		return true;
#endif
	}
}