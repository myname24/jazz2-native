#pragma once

#include "../../Main.h"

#include "../../nCine/Graphics/RenderCommand.h"
#include "../../nCine/Graphics/RenderQueue.h"
#include "../../nCine/Graphics/SceneNode.h"
#include "../../nCine/Primitives/Rect.h"

using namespace nCine;

namespace Jazz2::Rendering
{
	class PlayerViewport;

#if !defined(RHI_CAP_SHADERS)
	/// Uniform data passed to the SW combine fragment shader
	struct CombineShaderData
	{
		float ambientR, ambientG, ambientB, ambientW;
		float invDarknessDiv;
		float waterLevelNorm;
		bool hasWater;
		float time;
		float camY;
	};
#endif

	/** @brief Combines all previous passes of a scene into a resulting image */
	class CombineRenderer : public SceneNode
	{
	public:
		CombineRenderer(PlayerViewport* owner);

		void Initialize(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
		Rectf GetBounds() const;

		bool OnDraw(RenderQueue& renderQueue) override;

	private:
		PlayerViewport* _owner;
		RenderCommand _renderCommand;
		RenderCommand _renderCommandWithWater;
		Rectf _bounds;

#if !defined(RHI_CAP_SHADERS)
		struct CombineShaderData _combineData;
#endif
	};
}