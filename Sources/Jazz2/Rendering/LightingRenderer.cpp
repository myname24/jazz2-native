#include "LightingRenderer.h"
#include "PlayerViewport.h"

#include "../../nCine/Graphics/RenderQueue.h"

#include <cmath>
#include <algorithm>

namespace Jazz2::Rendering
{
	LightingRenderer::LightingRenderer(PlayerViewport* owner)
		: _owner(owner)
#if defined(RHI_CAP_SHADERS) && defined(RHI_CAP_FRAMEBUFFERS)
			, _renderCommandsCount(0)
#endif
	{
		_emittedLightsCache.reserve(32);
		setVisitOrderState(SceneNode::VisitOrderState::Disabled);
	}
		
	bool LightingRenderer::OnDraw(RenderQueue& renderQueue)
	{
#if !defined(RHI_CAP_SHADERS) || !defined(RHI_CAP_FRAMEBUFFERS)
		// SW path: render lights directly into _lightingBuffer pixels
		_emittedLightsCache.clear();

		auto actors = _owner->_levelHandler->GetActors();
		std::size_t actorsCount = actors.size();
		for (std::size_t i = 0; i < actorsCount; i++) {
			actors[i]->OnEmitLights(_emittedLightsCache);
		}

		auto* lightTex = _owner->_lightingBuffer.get();
		lightTex->EnsureRenderTarget();
		std::uint8_t* pixels = lightTex->GetMutablePixels(0);
		if (pixels == nullptr) {
			return true;
		}

		std::int32_t texW = lightTex->GetWidth();
		std::int32_t texH = lightTex->GetHeight();

		// Clear with ambient light: R = ambient * 255, G = 0, B = 0, A = 255
		float ambient = _owner->_ambientLight.W;
		std::uint8_t ambientR = (std::uint8_t)std::min((std::int32_t)(ambient * 255.0f + 0.5f), 255);
		std::uint32_t clearPixel = ambientR | (0x00u << 8) | (0x00u << 16) | (0xFFu << 24);
		std::uint32_t* pixels32 = reinterpret_cast<std::uint32_t*>(pixels);
		std::int32_t totalPixels = texW * texH;
		for (std::int32_t i = 0; i < totalPixels; i++) {
			pixels32[i] = clearPixel;
		}

		// Camera transform: world position to screen pixel
		Vector2f halfView = Vector2f((float)(texW / 2), (float)(texH / 2));
		Vector2f camOrigin = _owner->_cameraPos - halfView;

		for (auto& light : _emittedLightsCache) {
			float radiusFar = light.RadiusFar;
			if (radiusFar <= 0.0f) continue;

			float radiusNearRatio = light.RadiusNear / radiusFar;
			float intensity = light.Intensity;
			float brightness = light.Brightness;

			// Light center in screen coordinates
			float cx = light.Pos.X - camOrigin.X;
			float cy = light.Pos.Y - camOrigin.Y;

			// Bounding box in pixels
			std::int32_t x0 = std::max((std::int32_t)(cx - radiusFar), 0);
			std::int32_t y0 = std::max((std::int32_t)(cy - radiusFar), 0);
			std::int32_t x1 = std::min((std::int32_t)(cx + radiusFar + 1.0f), texW);
			std::int32_t y1 = std::min((std::int32_t)(cy + radiusFar + 1.0f), texH);

			float invRadiusFar = 1.0f / radiusFar;

			for (std::int32_t py = y0; py < y1; py++) {
				float dy = ((float)py + 0.5f - cy) * invRadiusFar;
				float dy2 = dy * dy;
				std::uint8_t* row = pixels + (py * texW + x0) * 4;

				for (std::int32_t px = x0; px < x1; px++) {
					float dx = ((float)px + 0.5f - cx) * invRadiusFar;
					float dist = std::sqrt(dx * dx + dy2);

					if (dist <= 1.0f) {
						// Cubic falloff: t^3 where t = clamp(1 - (dist - radiusNear) / (1 - radiusNear), 0, 1)
						float t;
						if (radiusNearRatio >= 1.0f) {
							t = 1.0f;
						} else {
							t = (1.0f - dist) / (1.0f - radiusNearRatio);
							if (t > 1.0f) t = 1.0f;
							else if (t < 0.0f) t = 0.0f;
						}
						float strength = t * t * t;

						// Additive blend: add (strength * intensity, strength * brightness) to R, G channels
						// Clamp additions to ≥0, matching GL fixed-point framebuffer clamping behavior
						float addR = std::max(0.0f, strength * intensity * 255.0f);
						float addG = std::max(0.0f, strength * brightness * 255.0f);

						std::int32_t newR = row[0] + (std::int32_t)(addR + 0.5f);
						std::int32_t newG = row[1] + (std::int32_t)(addG + 0.5f);
						row[0] = (std::uint8_t)(newR > 255 ? 255 : newR);
						row[1] = (std::uint8_t)(newG > 255 ? 255 : newG);
					}

					row += 4;
				}
			}
		}

		return true;
#else
		_renderCommandsCount = 0;
		_emittedLightsCache.clear();

		// Collect all active light emitters
		auto actors = _owner->_levelHandler->GetActors();
		std::size_t actorsCount = actors.size();
		for (std::size_t i = 0; i < actorsCount; i++) {
			actors[i]->OnEmitLights(_emittedLightsCache);
		}

		for (auto& light : _emittedLightsCache) {
			auto command = RentRenderCommand();
			auto instanceBlock = command->GetMaterial().UniformBlock(Material::InstanceBlockName);
			instanceBlock->GetUniform(Material::TexRectUniformName)->SetFloatValue(light.Pos.X, light.Pos.Y, light.RadiusNear / light.RadiusFar, 0.0f);
			instanceBlock->GetUniform(Material::SpriteSizeUniformName)->SetFloatValue(light.RadiusFar * 2.0f, light.RadiusFar * 2.0f);
			instanceBlock->GetUniform(Material::ColorUniformName)->SetFloatValue(light.Intensity, light.Brightness, 0.0f, 0.0f);
			command->SetTransformation(Matrix4x4f::Translation(light.Pos.X, light.Pos.Y, 0));

			renderQueue.AddCommand(command);
		}

		return true;
#endif
	}

#if defined(RHI_CAP_SHADERS) && defined(RHI_CAP_FRAMEBUFFERS)
	RenderCommand* LightingRenderer::RentRenderCommand()
	{
		if (_renderCommandsCount < _renderCommands.size()) {
			RenderCommand* command = _renderCommands[_renderCommandsCount].get();
			_renderCommandsCount++;
			return command;
		} else {
			std::unique_ptr<RenderCommand>& command = _renderCommands.emplace_back(std::make_unique<RenderCommand>(RenderCommand::Type::Lighting));
			_renderCommandsCount++;
			command->GetMaterial().SetShader(_owner->_levelHandler->_lightingShader);
			command->GetMaterial().SetBlendingEnabled(true);
			command->GetMaterial().SetBlendingFactors(RHI::BlendFactor::SrcAlpha, RHI::BlendFactor::One);
			command->GetMaterial().ReserveUniformsDataMemory();
			command->GetGeometry().SetDrawParameters(RHI::PrimitiveType::TriangleStrip, 0, 4);

			auto* textureUniform = command->GetMaterial().Uniform(Material::TextureUniformName);
			if (textureUniform && textureUniform->GetIntValue(0) != 0) {
				textureUniform->SetIntValue(0); // GL_TEXTURE0
			}
			return command.get();
		}
	}
#endif
}