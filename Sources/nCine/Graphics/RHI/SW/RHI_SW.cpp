#if defined(WITH_RHI_SW)

#include "RHI_SW.h"

#include <Containers/SmallVector.h>
#include <Containers/StringView.h>

#include <Cpu.h>
#if defined(DEATH_TARGET_X86)
#	if defined(DEATH_ENABLE_AVX2)
#		include <IntrinsicsAvx.h>
#	elif defined(DEATH_ENABLE_SSE2)
#		include <IntrinsicsSse2.h>
#	endif
#elif defined(DEATH_TARGET_ARM)
#	if defined(DEATH_ENABLE_NEON)
#		include <arm_neon.h>
#	endif
#elif defined(DEATH_TARGET_WASM)
#	if defined(DEATH_ENABLE_SIMD128)
#		include <wasm_simd128.h>
#	endif
#endif

#include <algorithm>
#include <cstring>

using namespace Death;
using namespace Death::Containers;
using namespace Death::Containers::Literals;

namespace nCine::RHI
{
	SWGfxCapabilities::SWGfxCapabilities()
	{
		infoStrings_.renderer = "Software Renderer";

#if defined(DEATH_CPU_USE_RUNTIME_DISPATCH)
		auto features = Cpu::runtimeFeatures();
#else
		auto features = Cpu::compiledFeatures();
#endif

		StringView featureName;
#if defined(DEATH_TARGET_X86)
		if (features & Cpu::Avx2) {
			featureName = "AVX2"_s;
		} else if (features & Cpu::Sse2) {
			featureName = "SSE2"_s;
		} else {
			featureName = "Scalar"_s;
		}
#elif defined(DEATH_TARGET_ARM)
		if (features & Cpu::Neon) {
			featureName = "Neon"_s;
		} else if (features & Cpu::Sse2) {
			featureName = "Scalar"_s;
		}
#elif defined(DEATH_TARGET_WASM)
		if (features & Cpu::Simd128) {
			featureName = "SIMD128"_s;
		} else if (features & Cpu::Sse2) {
			featureName = "Scalar"_s;
		}
#else
		featureName = "Scalar"_s;
#endif

		LOGI("The application is using software renderer ({})", featureName);
	}

	std::int32_t SWGfxCapabilities::GetVersion(Version version) const
	{
		return 0;
	}

	const IGfxCapabilities::InfoStrings& SWGfxCapabilities::GetInfoStrings() const
	{
		return infoStrings_;
	}

	std::int32_t SWGfxCapabilities::GetValue(IntValues valueName) const
	{
		switch (valueName) {
			case IntValues::MAX_TEXTURE_SIZE:					return 4096;
			case IntValues::MAX_TEXTURE_IMAGE_UNITS:			return 8;
			case IntValues::UNIFORM_BUFFER_OFFSET_ALIGNMENT:	return 1;
			case IntValues::MAX_VERTEX_ATTRIB_STRIDE:			return 1024;
			case IntValues::MAX_COLOR_ATTACHMENTS:				return 4;
			default:											return 0;
		}
	}

	std::int32_t SWGfxCapabilities::GetArrayValue(ArrayIntValues arrayValueName, std::uint32_t index) const
	{
		return 0;
	}

	bool SWGfxCapabilities::HasExtension(Extensions extensionName) const
	{
		return false;
	}

	// =========================================================================
	// Global SW render state
	// =========================================================================
	namespace
	{
		struct SWState
		{
			// Active (current) colour buffer - may point to mainColorBuffer or a bound texture
			std::uint8_t* colorBuffer   = nullptr;
			std::int32_t  bufferWidth   = 0;
			std::int32_t  bufferHeight  = 0;

			// Heap-allocated main window output buffer (never redirected)
			std::uint8_t* mainColorBuffer  = nullptr;
			std::int32_t  mainBufferWidth  = 0;
			std::int32_t  mainBufferHeight = 0;
			bool          isFboTarget      = false;

			// Depth buffer (float, same dimensions)
			float*        depthBuffer   = nullptr;

			bool   depthTestEnabled  = false;
			bool   blendingEnabled   = false;
			BlendFactor blendSrc     = BlendFactor::SrcAlpha;
			BlendFactor blendDst     = BlendFactor::OneMinusSrcAlpha;
			bool   scissorEnabled    = false;
			std::int32_t  scissorX   = 0;
			std::int32_t  scissorY   = 0;
			std::int32_t  scissorW   = 0;
			std::int32_t  scissorH   = 0;

			float clearR = 0, clearG = 0, clearB = 0, clearA = 1;

			// Viewport
			std::int32_t viewportX = 0;
			std::int32_t viewportY = 0;
			std::int32_t viewportW = 0;
			std::int32_t viewportH = 0;

			// Active draw context for the current draw call
			const DrawContext* drawCtx = nullptr;
		};

		SWState g_state;
	}

	// =========================================================================
	// Texture implementation
	// =========================================================================
	void Texture::UploadMip(std::int32_t mipLevel, std::int32_t width, std::int32_t height, TextureFormat format,
	                        const void* data, std::size_t size)
	{
		if (mipLevel < 0 || mipLevel >= MaxMips) return;

		mips_[mipLevel].width  = width;
		mips_[mipLevel].height = height;

		std::size_t byteSize = size;
		// Only RGBA8 is directly stored; other formats converted at upload
		if (format == TextureFormat::RGBA8) {
			byteSize = static_cast<std::size_t>(width) * height * 4;
		} else if (format == TextureFormat::RGB8) {
			byteSize = static_cast<std::size_t>(width) * height * 4;
		}

		mips_[mipLevel].data = std::make_unique<std::uint8_t[]>(byteSize);

		if (data != nullptr) {
			if (format == TextureFormat::RGBA8) {
				std::memcpy(mips_[mipLevel].data.get(), data, byteSize);
			} else if (format == TextureFormat::RGB8) {
				// Convert RGB8 → RGBA8
				const std::uint8_t* src  = static_cast<const std::uint8_t*>(data);
				std::uint8_t*       dst  = mips_[mipLevel].data.get();
				std::int32_t pixels = width * height;
				for (std::int32_t i = 0; i < pixels; ++i) {
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
					dst[3] = 255;
					src += 3;
					dst += 4;
				}
			} else {
				// Generic fallback: copy raw bytes
				std::memcpy(mips_[mipLevel].data.get(), data, size);
			}
		}

		if (mipLevel == 0) {
			width_    = width;
			height_   = height;
			format_   = format;
		}
		if (mipLevel + 1 > mipCount_) {
			mipCount_ = mipLevel + 1;
		}
	}

	const std::uint8_t* Texture::GetPixels(std::int32_t mipLevel) const
	{
		if (mipLevel < 0 || mipLevel >= mipCount_) return nullptr;
		return mips_[mipLevel].data.get();
	}

	std::uint8_t* Texture::GetMutablePixels(std::int32_t mipLevel)
	{
		if (mipLevel < 0 || mipLevel >= mipCount_) return nullptr;
		return mips_[mipLevel].data.get();
	}

	void Texture::EnsureRenderTarget()
	{
		if (width_ <= 0 || height_ <= 0) return;
		MipLevel& m = mips_[0];
		if (m.data == nullptr || m.width != width_ || m.height != height_) {
			m.width  = width_;
			m.height = height_;
			m.data   = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(width_) * height_ * 4);
			if (mipCount_ < 1) mipCount_ = 1;
		}
	}

	Colorf Texture::Sample(float u, float v, std::int32_t mipLevel) const
	{
		if (mipLevel < 0 || mipLevel >= mipCount_) {
			return Colorf(1, 1, 1, 1);
		}

		const MipLevel& mip = mips_[mipLevel];
		if (mip.data == nullptr || mip.width == 0 || mip.height == 0) {
			return Colorf(1, 1, 1, 1);
		}

		// Apply wrapping
		auto wrapCoord = [](float t, SamplerWrapping mode) -> float {
			switch (mode) {
				case SamplerWrapping::Repeat:
					t -= std::floor(t);
					return t;
				case SamplerWrapping::MirroredRepeat: {
					float f = std::floor(t);
					t -= f;
					if (static_cast<std::int32_t>(f) & 1) t = 1.0f - t;
					return t;
				}
				case SamplerWrapping::ClampToEdge:
				default:
					return std::fmax(0.0f, std::fmin(1.0f, t));
			}
		};

		u = wrapCoord(u, wrapS_);
		v = wrapCoord(v, wrapT_);

		const std::int32_t px = static_cast<std::int32_t>(u * (mip.width  - 1) + 0.5f);
		const std::int32_t py = static_cast<std::int32_t>(v * (mip.height - 1) + 0.5f);
		const std::int32_t clampedX = std::max(0, std::min(mip.width  - 1, px));
		const std::int32_t clampedY = std::max(0, std::min(mip.height - 1, py));

		const std::uint8_t* pixel = mip.data.get() + (clampedY * mip.width + clampedX) * 4;
		return Colorf(pixel[0] / 255.0f, pixel[1] / 255.0f, pixel[2] / 255.0f, pixel[3] / 255.0f);
	}

	// =========================================================================
	// Render-state setters
	// =========================================================================
	void SetBlending(bool enabled, BlendFactor src, BlendFactor dst)
	{
		g_state.blendingEnabled = enabled;
		g_state.blendSrc = src;
		g_state.blendDst = dst;
	}

	void SetDepthTest(bool enabled)
	{
		g_state.depthTestEnabled = enabled;
	}

	void SetScissorTest(bool enabled, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
	{
		g_state.scissorEnabled = enabled;
		g_state.scissorX = x;
		g_state.scissorY = y;
		g_state.scissorW = width;
		g_state.scissorH = height;
	}

	void SetScissorTest(bool enabled, const Recti& rect)
	{
		SetScissorTest(enabled, rect.X, rect.Y, rect.W, rect.H);
	}

	void SetViewport(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
	{
		g_state.viewportX = x;
		g_state.viewportY = y;
		g_state.viewportW = width;
		g_state.viewportH = height;
	}

	void SetClearColor(float r, float g, float b, float a)
	{
		g_state.clearR = r;
		g_state.clearG = g;
		g_state.clearB = b;
		g_state.clearA = a;
	}

	void Clear(ClearFlags flags)
	{
		const bool doColor = (static_cast<std::uint32_t>(flags & ClearFlags::Color) != 0);
		const bool doDepth = (static_cast<std::uint32_t>(flags & ClearFlags::Depth) != 0);

		if (doColor && g_state.colorBuffer != nullptr) {
			const std::uint8_t r = static_cast<std::uint8_t>(g_state.clearR * 255.0f);
			const std::uint8_t gv = static_cast<std::uint8_t>(g_state.clearG * 255.0f);
			const std::uint8_t b = static_cast<std::uint8_t>(g_state.clearB * 255.0f);
			const std::uint8_t a = static_cast<std::uint8_t>(g_state.clearA * 255.0f);
			const std::int32_t totalPixels = g_state.bufferWidth * g_state.bufferHeight;
			if (r == gv && gv == b && b == a) {
				// All channels identical: single memset
				std::memset(g_state.colorBuffer, r, static_cast<std::size_t>(totalPixels) * 4);
			} else {
				// Build 32-bit RGBA pattern and fill using 32-bit writes
				const std::uint32_t pattern = static_cast<std::uint32_t>(r)
					| (static_cast<std::uint32_t>(gv) << 8)
					| (static_cast<std::uint32_t>(b) << 16)
					| (static_cast<std::uint32_t>(a) << 24);
				std::uint32_t* dst32 = reinterpret_cast<std::uint32_t*>(g_state.colorBuffer);
				for (std::int32_t i = 0; i < totalPixels; ++i) {
					dst32[i] = pattern;
				}
			}
		}

		if (doDepth && g_state.depthBuffer != nullptr) {
			const std::int32_t totalPixels = g_state.bufferWidth * g_state.bufferHeight;
			// 1.0f = 0x3F800000 — all floats identical, but can't memset floats
			std::fill(g_state.depthBuffer, g_state.depthBuffer + totalPixels, 1.0f);
		}
	}

	ScissorState GetScissorState()
	{
		return { g_state.scissorEnabled, g_state.scissorX, g_state.scissorY, g_state.scissorW, g_state.scissorH };
	}

	void SetScissorState(const ScissorState& state)
	{
		g_state.scissorEnabled = state.enabled;
		g_state.scissorX = state.x;
		g_state.scissorY = state.y;
		g_state.scissorW = state.w;
		g_state.scissorH = state.h;
	}

	ViewportState GetViewportState()
	{
		return { g_state.viewportX, g_state.viewportY, g_state.viewportW, g_state.viewportH };
	}

	void SetViewportState(const ViewportState& s)
	{
		g_state.viewportX = s.x;
		g_state.viewportY = s.y;
		g_state.viewportW = s.w;
		g_state.viewportH = s.h;
	}

	ClearColorState GetClearColorState()
	{
		return { g_state.clearR, g_state.clearG, g_state.clearB, g_state.clearA };
	}

	void SetClearColorState(const ClearColorState& s)
	{
		g_state.clearR = s.r;
		g_state.clearG = s.g;
		g_state.clearB = s.b;
		g_state.clearA = s.a;
	}

	// =========================================================================
	// SIMD-dispatched scanline blending (SrcAlpha / OneMinusSrcAlpha)
	// =========================================================================
	namespace
	{
		extern void DEATH_CPU_DISPATCHED_DECLARATION(blendScanlineSrcAlpha)(std::uint8_t* DEATH_RESTRICT dst, const std::uint8_t* DEATH_RESTRICT src, std::int32_t count);
		DEATH_CPU_DISPATCHER_DECLARATION(blendScanlineSrcAlpha)

		// Scalar fallback
		DEATH_CPU_MAYBE_UNUSED typename std::decay<decltype(blendScanlineSrcAlpha)>::type blendScanlineSrcAlphaImplementation(Cpu::ScalarT) {
			return [](std::uint8_t* DEATH_RESTRICT dst, const std::uint8_t* DEATH_RESTRICT src, std::int32_t count) {
				for (std::int32_t i = 0; i < count; ++i, dst += 4, src += 4) {
					const std::int32_t sA = src[3];
					if (sA == 0) continue;
					if (sA >= 255) {
						std::memcpy(dst, src, 4);
						continue;
					}
					const std::int32_t inv = 255 - sA;
					dst[0] = static_cast<std::uint8_t>((src[0] * sA + dst[0] * inv) >> 8);
					dst[1] = static_cast<std::uint8_t>((src[1] * sA + dst[1] * inv) >> 8);
					dst[2] = static_cast<std::uint8_t>((src[2] * sA + dst[2] * inv) >> 8);
					dst[3] = static_cast<std::uint8_t>((sA * 255 + dst[3] * inv) >> 8);
				}
			};
		}

#if defined(DEATH_ENABLE_SSE2)
		DEATH_CPU_MAYBE_UNUSED DEATH_ENABLE_SSE2 typename std::decay<decltype(blendScanlineSrcAlpha)>::type blendScanlineSrcAlphaImplementation(Cpu::Sse2T) {
			return [](std::uint8_t* DEATH_RESTRICT dst, const std::uint8_t* DEATH_RESTRICT src, std::int32_t count) DEATH_ENABLE_SSE2 {
				const __m128i zero = _mm_setzero_si128();
				const __m128i c255 = _mm_set1_epi16(255);

				std::int32_t i = 0;
				for (; i + 4 <= count; i += 4, dst += 16, src += 16) {
					__m128i srcPx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
					__m128i dstPx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst));

					// Low 2 pixels: unpack bytes to 16-bit
					__m128i sLo = _mm_unpacklo_epi8(srcPx, zero);
					__m128i dLo = _mm_unpacklo_epi8(dstPx, zero);
					// Broadcast alpha of each pixel: word[3]→[0..3], word[7]→[4..7]
					__m128i aLo = _mm_shufflelo_epi16(sLo, _MM_SHUFFLE(3, 3, 3, 3));
					aLo = _mm_shufflehi_epi16(aLo, _MM_SHUFFLE(3, 3, 3, 3));
					__m128i iaLo = _mm_sub_epi16(c255, aLo);
					__m128i rLo = _mm_add_epi16(_mm_mullo_epi16(sLo, aLo), _mm_mullo_epi16(dLo, iaLo));
					rLo = _mm_srli_epi16(rLo, 8);

					// High 2 pixels
					__m128i sHi = _mm_unpackhi_epi8(srcPx, zero);
					__m128i dHi = _mm_unpackhi_epi8(dstPx, zero);
					__m128i aHi = _mm_shufflelo_epi16(sHi, _MM_SHUFFLE(3, 3, 3, 3));
					aHi = _mm_shufflehi_epi16(aHi, _MM_SHUFFLE(3, 3, 3, 3));
					__m128i iaHi = _mm_sub_epi16(c255, aHi);
					__m128i rHi = _mm_add_epi16(_mm_mullo_epi16(sHi, aHi), _mm_mullo_epi16(dHi, iaHi));
					rHi = _mm_srli_epi16(rHi, 8);

					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_packus_epi16(rLo, rHi));
				}
				// Scalar tail
				for (; i < count; ++i, dst += 4, src += 4) {
					const std::int32_t sA = src[3];
					if (sA == 0) continue;
					if (sA >= 255) { std::memcpy(dst, src, 4); continue; }
					const std::int32_t inv = 255 - sA;
					dst[0] = static_cast<std::uint8_t>((src[0] * sA + dst[0] * inv) >> 8);
					dst[1] = static_cast<std::uint8_t>((src[1] * sA + dst[1] * inv) >> 8);
					dst[2] = static_cast<std::uint8_t>((src[2] * sA + dst[2] * inv) >> 8);
					dst[3] = static_cast<std::uint8_t>((sA * 255 + dst[3] * inv) >> 8);
				}
			};
		}
#endif

#if defined(DEATH_ENABLE_AVX2)
		DEATH_CPU_MAYBE_UNUSED DEATH_ENABLE_AVX2 typename std::decay<decltype(blendScanlineSrcAlpha)>::type blendScanlineSrcAlphaImplementation(Cpu::Avx2T) {
			return [](std::uint8_t* DEATH_RESTRICT dst, const std::uint8_t* DEATH_RESTRICT src, std::int32_t count) DEATH_ENABLE_AVX2 {
				const __m256i zero = _mm256_setzero_si256();
				const __m256i c255 = _mm256_set1_epi16(255);

				std::int32_t i = 0;
				for (; i + 8 <= count; i += 8, dst += 32, src += 32) {
					__m256i srcPx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
					__m256i dstPx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst));

					// Within each 128-bit lane: unpacklo processes first 2 pixels, unpackhi the next 2
					__m256i sLo = _mm256_unpacklo_epi8(srcPx, zero);
					__m256i dLo = _mm256_unpacklo_epi8(dstPx, zero);
					__m256i aLo = _mm256_shufflelo_epi16(sLo, _MM_SHUFFLE(3, 3, 3, 3));
					aLo = _mm256_shufflehi_epi16(aLo, _MM_SHUFFLE(3, 3, 3, 3));
					__m256i iaLo = _mm256_sub_epi16(c255, aLo);
					__m256i rLo = _mm256_add_epi16(_mm256_mullo_epi16(sLo, aLo), _mm256_mullo_epi16(dLo, iaLo));
					rLo = _mm256_srli_epi16(rLo, 8);

					__m256i sHi = _mm256_unpackhi_epi8(srcPx, zero);
					__m256i dHi = _mm256_unpackhi_epi8(dstPx, zero);
					__m256i aHi = _mm256_shufflelo_epi16(sHi, _MM_SHUFFLE(3, 3, 3, 3));
					aHi = _mm256_shufflehi_epi16(aHi, _MM_SHUFFLE(3, 3, 3, 3));
					__m256i iaHi = _mm256_sub_epi16(c255, aHi);
					__m256i rHi = _mm256_add_epi16(_mm256_mullo_epi16(sHi, aHi), _mm256_mullo_epi16(dHi, iaHi));
					rHi = _mm256_srli_epi16(rHi, 8);

					_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), _mm256_packus_epi16(rLo, rHi));
				}
				// SSE2 tail (4 pixels at a time)
				const __m128i zero128 = _mm_setzero_si128();
				const __m128i c255_128 = _mm_set1_epi16(255);
				for (; i + 4 <= count; i += 4, dst += 16, src += 16) {
					__m128i srcPx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
					__m128i dstPx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst));
					__m128i sLo = _mm_unpacklo_epi8(srcPx, zero128);
					__m128i dLo = _mm_unpacklo_epi8(dstPx, zero128);
					__m128i aLo = _mm_shufflelo_epi16(sLo, _MM_SHUFFLE(3, 3, 3, 3));
					aLo = _mm_shufflehi_epi16(aLo, _MM_SHUFFLE(3, 3, 3, 3));
					__m128i iaLo = _mm_sub_epi16(c255_128, aLo);
					__m128i rLo = _mm_add_epi16(_mm_mullo_epi16(sLo, aLo), _mm_mullo_epi16(dLo, iaLo));
					rLo = _mm_srli_epi16(rLo, 8);
					__m128i sHi = _mm_unpackhi_epi8(srcPx, zero128);
					__m128i dHi = _mm_unpackhi_epi8(dstPx, zero128);
					__m128i aHi = _mm_shufflelo_epi16(sHi, _MM_SHUFFLE(3, 3, 3, 3));
					aHi = _mm_shufflehi_epi16(aHi, _MM_SHUFFLE(3, 3, 3, 3));
					__m128i iaHi = _mm_sub_epi16(c255_128, aHi);
					__m128i rHi = _mm_add_epi16(_mm_mullo_epi16(sHi, aHi), _mm_mullo_epi16(dHi, iaHi));
					rHi = _mm_srli_epi16(rHi, 8);
					_mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_packus_epi16(rLo, rHi));
				}
				// Scalar tail
				for (; i < count; ++i, dst += 4, src += 4) {
					const std::int32_t sA = src[3];
					if (sA == 0) continue;
					if (sA >= 255) { std::memcpy(dst, src, 4); continue; }
					const std::int32_t inv = 255 - sA;
					dst[0] = static_cast<std::uint8_t>((src[0] * sA + dst[0] * inv) >> 8);
					dst[1] = static_cast<std::uint8_t>((src[1] * sA + dst[1] * inv) >> 8);
					dst[2] = static_cast<std::uint8_t>((src[2] * sA + dst[2] * inv) >> 8);
					dst[3] = static_cast<std::uint8_t>((sA * 255 + dst[3] * inv) >> 8);
				}
			};
		}
#endif

#if defined(DEATH_ENABLE_NEON)
		DEATH_CPU_MAYBE_UNUSED DEATH_ENABLE_NEON typename std::decay<decltype(blendScanlineSrcAlpha)>::type blendScanlineSrcAlphaImplementation(Cpu::NeonT) {
			return [](std::uint8_t* DEATH_RESTRICT dst, const std::uint8_t* DEATH_RESTRICT src, std::int32_t count) DEATH_ENABLE_NEON {
				static const std::uint8_t alphaIdxData[8] = { 3, 3, 3, 3, 7, 7, 7, 7 };
				const uint8x8_t alphaIdx = vld1_u8(alphaIdxData);
				const uint8x8_t c255x8 = vdup_n_u8(255);

				std::int32_t i = 0;
				for (; i + 4 <= count; i += 4, dst += 16, src += 16) {
					uint8x16_t srcPx = vld1q_u8(src);
					uint8x16_t dstPx = vld1q_u8(dst);

					// Low 2 pixels
					uint8x8_t srcLo = vget_low_u8(srcPx);
					uint8x8_t dstLo = vget_low_u8(dstPx);
					uint8x8_t aLo = vtbl1_u8(srcLo, alphaIdx);
					uint8x8_t iaLo = vsub_u8(c255x8, aLo);
					uint16x8_t rLo = vaddq_u16(vmull_u8(srcLo, aLo), vmull_u8(dstLo, iaLo));

					// High 2 pixels
					uint8x8_t srcHi = vget_high_u8(srcPx);
					uint8x8_t dstHi = vget_high_u8(dstPx);
					uint8x8_t aHi = vtbl1_u8(srcHi, alphaIdx);
					uint8x8_t iaHi = vsub_u8(c255x8, aHi);
					uint16x8_t rHi = vaddq_u16(vmull_u8(srcHi, aHi), vmull_u8(dstHi, iaHi));

					vst1q_u8(dst, vcombine_u8(vshrn_n_u16(rLo, 8), vshrn_n_u16(rHi, 8)));
				}
				// Scalar tail
				for (; i < count; ++i, dst += 4, src += 4) {
					const std::int32_t sA = src[3];
					if (sA == 0) continue;
					if (sA >= 255) { std::memcpy(dst, src, 4); continue; }
					const std::int32_t inv = 255 - sA;
					dst[0] = static_cast<std::uint8_t>((src[0] * sA + dst[0] * inv) >> 8);
					dst[1] = static_cast<std::uint8_t>((src[1] * sA + dst[1] * inv) >> 8);
					dst[2] = static_cast<std::uint8_t>((src[2] * sA + dst[2] * inv) >> 8);
					dst[3] = static_cast<std::uint8_t>((sA * 255 + dst[3] * inv) >> 8);
				}
			};
		}
#endif

#if defined(DEATH_ENABLE_SIMD128)
		DEATH_CPU_MAYBE_UNUSED DEATH_ENABLE_SIMD128 typename std::decay<decltype(blendScanlineSrcAlpha)>::type blendScanlineSrcAlphaImplementation(Cpu::Simd128T) {
			return [](std::uint8_t* DEATH_RESTRICT dst, const std::uint8_t* DEATH_RESTRICT src, std::int32_t count) DEATH_ENABLE_SIMD128 {
				const v128_t c255 = wasm_i16x8_splat(255);

				std::int32_t i = 0;
				for (; i + 4 <= count; i += 4, dst += 16, src += 16) {
					v128_t srcPx = wasm_v128_load(src);
					v128_t dstPx = wasm_v128_load(dst);

					// Low 2 pixels: extend to 16-bit
					v128_t sLo = wasm_u16x8_extend_low_u8x16(srcPx);
					v128_t dLo = wasm_u16x8_extend_low_u8x16(dstPx);
					// Broadcast alpha: byte shuffle on 16-bit data
					v128_t aLo = wasm_i8x16_shuffle(sLo, sLo, 6, 7, 6, 7, 6, 7, 6, 7, 14, 15, 14, 15, 14, 15, 14, 15);
					v128_t iaLo = wasm_i16x8_sub(c255, aLo);
					v128_t rLo = wasm_u16x8_shr(wasm_i16x8_add(wasm_i16x8_mul(sLo, aLo), wasm_i16x8_mul(dLo, iaLo)), 8);

					// High 2 pixels
					v128_t sHi = wasm_u16x8_extend_high_u8x16(srcPx);
					v128_t dHi = wasm_u16x8_extend_high_u8x16(dstPx);
					v128_t aHi = wasm_i8x16_shuffle(sHi, sHi, 6, 7, 6, 7, 6, 7, 6, 7, 14, 15, 14, 15, 14, 15, 14, 15);
					v128_t iaHi = wasm_i16x8_sub(c255, aHi);
					v128_t rHi = wasm_u16x8_shr(wasm_i16x8_add(wasm_i16x8_mul(sHi, aHi), wasm_i16x8_mul(dHi, iaHi)), 8);

					wasm_v128_store(dst, wasm_u8x16_narrow_i16x8(rLo, rHi));
				}
				// Scalar tail
				for (; i < count; ++i, dst += 4, src += 4) {
					const std::int32_t sA = src[3];
					if (sA == 0) continue;
					if (sA >= 255) { std::memcpy(dst, src, 4); continue; }
					const std::int32_t inv = 255 - sA;
					dst[0] = static_cast<std::uint8_t>((src[0] * sA + dst[0] * inv) >> 8);
					dst[1] = static_cast<std::uint8_t>((src[1] * sA + dst[1] * inv) >> 8);
					dst[2] = static_cast<std::uint8_t>((src[2] * sA + dst[2] * inv) >> 8);
					dst[3] = static_cast<std::uint8_t>((sA * 255 + dst[3] * inv) >> 8);
				}
			};
		}
#endif

		DEATH_CPU_DISPATCHER_BASE(blendScanlineSrcAlphaImplementation)
		DEATH_CPU_DISPATCHED(blendScanlineSrcAlphaImplementation, void DEATH_CPU_DISPATCHED_DECLARATION(blendScanlineSrcAlpha)(std::uint8_t* DEATH_RESTRICT dst, const std::uint8_t* DEATH_RESTRICT src, std::int32_t count))({
			return blendScanlineSrcAlphaImplementation(Cpu::DefaultBase)(dst, src, count);
		})
	}

	// =========================================================================
	// Rasterization helpers
	// =========================================================================
	namespace
	{
		// UV wrapping for float coordinates [0..1] — shared by both rasterizers
		inline float WrapUV(float t, SamplerWrapping mode)
		{
			switch (mode) {
				case SamplerWrapping::Repeat:
					t -= std::floor(t);
					return t;
				case SamplerWrapping::MirroredRepeat: {
					float f = std::floor(t);
					t -= f;
					if (static_cast<std::int32_t>(f) & 1) t = 1.0f - t;
					return t;
				}
				default: // ClampToEdge
					if (t < 0.0f) return 0.0f;
					if (t > 1.0f) return 1.0f;
					return t;
			}
		}

		// Fixed-point UV wrapping for the axis-aligned quad rasterizer.
		// coordFix is in 16.16 format representing pixel index (u * texW * 65536).
		// Returns pixel index in [0, texDim).
		inline std::int32_t WrapTexelFix(std::int32_t coordFix, std::int32_t texDim, SamplerWrapping mode)
		{
			std::int32_t idx = coordFix >> 16;
			switch (mode) {
				case SamplerWrapping::Repeat:
					idx = idx % texDim;
					if (idx < 0) idx += texDim;
					return idx;
				case SamplerWrapping::MirroredRepeat: {
					std::int32_t period = texDim * 2;
					idx = idx % period;
					if (idx < 0) idx += period;
					if (idx >= texDim) idx = period - 1 - idx;
					return idx;
				}
				default: // ClampToEdge
					if (idx < 0) return 0;
					if (idx >= texDim) return texDim - 1;
					return idx;
			}
		}

		// Normalize a fixed-point UV to [0, texDimFix) for Repeat wrapping.
		// This is called once per scanline to allow branch-based wrap per pixel.
		inline std::int32_t NormalizeRepeatFix(std::int32_t coordFix, std::int32_t texDimFix)
		{
			coordFix = coordFix % texDimFix;
			if (coordFix < 0) coordFix += texDimFix;
			return coordFix;
		}

		// Advance a Repeat-normalized fixed-point UV by one step.
		inline std::int32_t AdvanceRepeatFix(std::int32_t coordFix, std::int32_t step, std::int32_t texDimFix)
		{
			coordFix += step;
			if (coordFix >= texDimFix) coordFix -= texDimFix;
			else if (coordFix < 0) coordFix += texDimFix;
			return coordFix;
		}

		// Generic integer blend for arbitrary BlendFactor (used by triangle rasterizer)
		inline void BlendPixelGeneric(std::uint8_t* dst, std::int32_t sR, std::int32_t sG,
		                              std::int32_t sB, std::int32_t sA,
		                              BlendFactor sFactor, BlendFactor dFactor)
		{
			if (sFactor == BlendFactor::SrcAlpha && dFactor == BlendFactor::OneMinusSrcAlpha) {
				// Fast path: most common blend mode
				const std::int32_t inv = 255 - sA;
				dst[0] = static_cast<std::uint8_t>((sR * sA + dst[0] * inv) >> 8);
				dst[1] = static_cast<std::uint8_t>((sG * sA + dst[1] * inv) >> 8);
				dst[2] = static_cast<std::uint8_t>((sB * sA + dst[2] * inv) >> 8);
				dst[3] = static_cast<std::uint8_t>((sA * 255 + dst[3] * inv) >> 8);
				return;
			}
			// General fallback using float math
			const float srcRf = sR / 255.0f, srcGf = sG / 255.0f, srcBf = sB / 255.0f, srcAf = sA / 255.0f;
			const float dstRf = dst[0] / 255.0f, dstGf = dst[1] / 255.0f, dstBf = dst[2] / 255.0f, dstAf = dst[3] / 255.0f;
			auto bf = [&](BlendFactor f, float c, bool isSrc) -> float {
				switch (f) {
					case BlendFactor::Zero:             return 0.0f;
					case BlendFactor::One:              return c;
					case BlendFactor::SrcAlpha:         return c * srcAf;
					case BlendFactor::OneMinusSrcAlpha: return c * (1.0f - srcAf);
					case BlendFactor::DstAlpha:         return c * dstAf;
					case BlendFactor::OneMinusDstAlpha: return c * (1.0f - dstAf);
					case BlendFactor::SrcColor:         return c * (isSrc ? 1.0f : srcRf);
					case BlendFactor::DstColor:         return c * (isSrc ? dstRf : 1.0f);
					default:                            return c;
				}
			};
			auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
			dst[0] = static_cast<std::uint8_t>(clamp01(bf(sFactor, srcRf, true)  + bf(dFactor, dstRf, false)) * 255.0f);
			dst[1] = static_cast<std::uint8_t>(clamp01(bf(sFactor, srcGf, true)  + bf(dFactor, dstGf, false)) * 255.0f);
			dst[2] = static_cast<std::uint8_t>(clamp01(bf(sFactor, srcBf, true)  + bf(dFactor, dstBf, false)) * 255.0f);
			dst[3] = static_cast<std::uint8_t>(clamp01(bf(sFactor, srcAf, true)  + bf(dFactor, dstAf, false)) * 255.0f);
		}

		// Wrap a signed integer texel coordinate into [0, texDim)
		inline std::int32_t WrapTexelCoord(std::int32_t idx, std::int32_t texDim, SamplerWrapping mode)
		{
			switch (mode) {
				case SamplerWrapping::Repeat:
					idx = idx % texDim;
					if (idx < 0) idx += texDim;
					return idx;
				case SamplerWrapping::MirroredRepeat: {
					std::int32_t period = texDim * 2;
					idx = idx % period;
					if (idx < 0) idx += period;
					if (idx >= texDim) idx = period - 1 - idx;
					return idx;
				}
				default: // ClampToEdge
					if (idx < 0) return 0;
					if (idx >= texDim) return texDim - 1;
					return idx;
			}
		}

		// Bilinear texture sample from 16.16 fixed-point UV coordinates.
		// uFix/vFix represent pixel coords (u * texW * 65536, v * texH * 65536).
		// Result is written to out[0..3] as RGBA bytes.
		inline void SampleBilinearFix(const std::uint8_t* texPixels, std::int32_t texW, std::int32_t texH,
		                              std::int32_t uFix, std::int32_t vFix,
		                              SamplerWrapping wrapS, SamplerWrapping wrapT,
		                              std::uint8_t* out)
		{
			// Half-pixel offset for correct bilinear centering
			const std::int32_t uf = uFix - (1 << 15);
			const std::int32_t vf = vFix - (1 << 15);

			std::int32_t x0 = uf >> 16;
			std::int32_t y0 = vf >> 16;

			// 8-bit fractional weights
			const std::int32_t fx = (uf & 0xFFFF) >> 8;
			const std::int32_t fy = (vf & 0xFFFF) >> 8;

			// Wrap all four sample coordinates
			std::int32_t x1 = WrapTexelCoord(x0 + 1, texW, wrapS);
			std::int32_t y1 = WrapTexelCoord(y0 + 1, texH, wrapT);
			x0 = WrapTexelCoord(x0, texW, wrapS);
			y0 = WrapTexelCoord(y0, texH, wrapT);

			const std::uint8_t* c00 = texPixels + (static_cast<std::size_t>(y0) * texW + x0) * 4;
			const std::uint8_t* c10 = texPixels + (static_cast<std::size_t>(y0) * texW + x1) * 4;
			const std::uint8_t* c01 = texPixels + (static_cast<std::size_t>(y1) * texW + x0) * 4;
			const std::uint8_t* c11 = texPixels + (static_cast<std::size_t>(y1) * texW + x1) * 4;

			const std::int32_t ifx = 255 - fx;
			const std::int32_t ify = 255 - fy;
			const std::int32_t w00 = ifx * ify;
			const std::int32_t w10 = fx * ify;
			const std::int32_t w01 = ifx * fy;
			const std::int32_t w11 = fx * fy;

			out[0] = static_cast<std::uint8_t>((c00[0] * w00 + c10[0] * w10 + c01[0] * w01 + c11[0] * w11) >> 16);
			out[1] = static_cast<std::uint8_t>((c00[1] * w00 + c10[1] * w10 + c01[1] * w01 + c11[1] * w11) >> 16);
			out[2] = static_cast<std::uint8_t>((c00[2] * w00 + c10[2] * w10 + c01[2] * w01 + c11[2] * w11) >> 16);
			out[3] = static_cast<std::uint8_t>((c00[3] * w00 + c10[3] * w10 + c01[3] * w01 + c11[3] * w11) >> 16);
		}

		// Pre-computed bilinear Y context for scanline-optimized sampling.
		// Y coordinates and weights are constant across a scanline, so precompute once.
		struct BilinearRowCtx
		{
			const std::uint8_t* row0;
			const std::uint8_t* row1;
			std::int32_t ify; // 255 - fy (top row weight)
			std::int32_t fy;  // fy (bottom row weight)
		};

		inline BilinearRowCtx PrepareBilinearRow(const std::uint8_t* texPixels, std::int32_t texW, std::int32_t texH,
		                                         std::int32_t vFix, SamplerWrapping wrapT)
		{
			const std::int32_t vf = vFix - (1 << 15);
			std::int32_t y0 = vf >> 16;
			const std::int32_t fy = (vf & 0xFFFF) >> 8;
			std::int32_t y1 = WrapTexelCoord(y0 + 1, texH, wrapT);
			y0 = WrapTexelCoord(y0, texH, wrapT);
			return {
				texPixels + static_cast<std::size_t>(y0) * texW * 4,
				texPixels + static_cast<std::size_t>(y1) * texW * 4,
				255 - fy, fy
			};
		}

		inline void SampleBilinearFixRow(const BilinearRowCtx& row, std::int32_t texW,
		                                 std::int32_t uFix, SamplerWrapping wrapS,
		                                 std::uint8_t* out)
		{
			const std::int32_t uf = uFix - (1 << 15);
			std::int32_t x0 = uf >> 16;
			const std::int32_t fx = (uf & 0xFFFF) >> 8;
			std::int32_t x1 = WrapTexelCoord(x0 + 1, texW, wrapS);
			x0 = WrapTexelCoord(x0, texW, wrapS);

			const std::uint8_t* c00 = row.row0 + x0 * 4;
			const std::uint8_t* c10 = row.row0 + x1 * 4;
			const std::uint8_t* c01 = row.row1 + x0 * 4;
			const std::uint8_t* c11 = row.row1 + x1 * 4;

			const std::int32_t ifx = 255 - fx;
			const std::int32_t w00 = ifx * row.ify;
			const std::int32_t w10 = fx * row.ify;
			const std::int32_t w01 = ifx * row.fy;
			const std::int32_t w11 = fx * row.fy;

			out[0] = static_cast<std::uint8_t>((c00[0] * w00 + c10[0] * w10 + c01[0] * w01 + c11[0] * w11) >> 16);
			out[1] = static_cast<std::uint8_t>((c00[1] * w00 + c10[1] * w10 + c01[1] * w01 + c11[1] * w11) >> 16);
			out[2] = static_cast<std::uint8_t>((c00[2] * w00 + c10[2] * w10 + c01[2] * w01 + c11[2] * w11) >> 16);
			out[3] = static_cast<std::uint8_t>((c00[3] * w00 + c10[3] * w10 + c01[3] * w01 + c11[3] * w11) >> 16);
		}

		// Bilinear texture sample from float UV coordinates in [0..1] range.
		// Used by the triangle rasterizer.
		inline void SampleBilinearFloat(const std::uint8_t* texPixels, std::int32_t texW, std::int32_t texH,
		                                float u, float v,
		                                SamplerWrapping wrapS, SamplerWrapping wrapT,
		                                std::uint8_t* out)
		{
			const float uf = u * texW - 0.5f;
			const float vf = v * texH - 0.5f;

			std::int32_t x0 = static_cast<std::int32_t>(std::floor(uf));
			std::int32_t y0 = static_cast<std::int32_t>(std::floor(vf));

			std::int32_t fx = static_cast<std::int32_t>((uf - x0) * 255.0f + 0.5f);
			std::int32_t fy = static_cast<std::int32_t>((vf - y0) * 255.0f + 0.5f);
			if (fx < 0) fx = 0; if (fx > 255) fx = 255;
			if (fy < 0) fy = 0; if (fy > 255) fy = 255;

			std::int32_t x1 = WrapTexelCoord(x0 + 1, texW, wrapS);
			std::int32_t y1 = WrapTexelCoord(y0 + 1, texH, wrapT);
			x0 = WrapTexelCoord(x0, texW, wrapS);
			y0 = WrapTexelCoord(y0, texH, wrapT);

			const std::uint8_t* c00 = texPixels + (static_cast<std::size_t>(y0) * texW + x0) * 4;
			const std::uint8_t* c10 = texPixels + (static_cast<std::size_t>(y0) * texW + x1) * 4;
			const std::uint8_t* c01 = texPixels + (static_cast<std::size_t>(y1) * texW + x0) * 4;
			const std::uint8_t* c11 = texPixels + (static_cast<std::size_t>(y1) * texW + x1) * 4;

			const std::int32_t ifx = 255 - fx;
			const std::int32_t ify = 255 - fy;
			const std::int32_t w00 = ifx * ify;
			const std::int32_t w10 = fx * ify;
			const std::int32_t w01 = ifx * fy;
			const std::int32_t w11 = fx * fy;

			out[0] = static_cast<std::uint8_t>((c00[0] * w00 + c10[0] * w10 + c01[0] * w01 + c11[0] * w11) >> 16);
			out[1] = static_cast<std::uint8_t>((c00[1] * w00 + c10[1] * w10 + c01[1] * w01 + c11[1] * w11) >> 16);
			out[2] = static_cast<std::uint8_t>((c00[2] * w00 + c10[2] * w10 + c01[2] * w01 + c11[2] * w11) >> 16);
			out[3] = static_cast<std::uint8_t>((c00[3] * w00 + c10[3] * w10 + c01[3] * w01 + c11[3] * w11) >> 16);
		}

		// =====================================================================
		// Vertex fetch (screen-space, after MVP + viewport transform)
		// =====================================================================
		struct Vertex2D { float x, y, u, v, r, g, b, a; };

		Vertex2D FetchVertex(const DrawContext& ctx, std::int32_t index)
		{
			Vertex2D out = { 0, 0, 0, 0, 1, 1, 1, 1 };

			if (ctx.vertexFormat == nullptr) {
				// Procedural sprite quad — replicate the sprite_vs.glsl vertex synthesis
				const float ax = ((index & ~1) == 0) ? 1.0f : 0.0f;
				const float ay = (index & 1) ? 1.0f : 0.0f;

				const float wx = ax * ctx.ff.spriteSize[0];
				const float wy = ay * ctx.ff.spriteSize[1];

				const float* m = ctx.ff.mvpMatrix;
				out.x = m[0] * wx + m[4] * wy + m[12];
				out.y = m[1] * wx + m[5] * wy + m[13];

				out.u = ax * ctx.ff.texRect[0] + ctx.ff.texRect[1];
				out.v = ay * ctx.ff.texRect[2] + ctx.ff.texRect[3];

				out.r = ctx.ff.color[0];
				out.g = ctx.ff.color[1];
				out.b = ctx.ff.color[2];
				out.a = ctx.ff.color[3];
			} else {
				const std::uint32_t attrCount = ctx.vertexFormat->GetAttributeCount();
				for (std::uint32_t i = 0; i < attrCount; ++i) {
					const VertexFormatAttribute& attr = ctx.vertexFormat->GetAttribute(i);
					if (!attr.enabled || attr.vbo == nullptr) continue;

					const std::uint8_t* base = static_cast<const std::uint8_t*>(attr.vbo->GetData());
					if (base == nullptr) continue;

					std::int32_t stride = attr.stride;
					if (stride == 0) stride = attr.size * 4;

					const std::uint8_t* ptr = base + ctx.vboByteOffset + static_cast<std::size_t>(index) * stride + attr.offset;

					if (i == 0 && attr.size >= 2) {
						const float* f = reinterpret_cast<const float*>(ptr);
						out.x = f[0];
						out.y = f[1];
					} else if (i == 1 && attr.size >= 2) {
						const float* f = reinterpret_cast<const float*>(ptr);
						out.u = f[0];
						out.v = f[1];
					}
				}

				out.r = ctx.ff.color[0];
				out.g = ctx.ff.color[1];
				out.b = ctx.ff.color[2];
				out.a = ctx.ff.color[3];
			}

			// Viewport transform: NDC [-1,+1] → screen pixel coordinates
			out.x = (out.x + 1.0f) * 0.5f * static_cast<float>(g_state.viewportW) + static_cast<float>(g_state.viewportX);
			out.y = (1.0f - out.y) * 0.5f * static_cast<float>(g_state.viewportH) + static_cast<float>(g_state.viewportY);

			// Snap to pixel grid when within epsilon of an integer.
			// Eliminates 1-scanline flicker caused by float precision drift in MVP calculation.
			constexpr float SnapEps = 1.0f / 128.0f;
			float rxSnap = std::round(out.x);
			float rySnap = std::round(out.y);
			if (std::fabs(out.x - rxSnap) < SnapEps) out.x = rxSnap;
			if (std::fabs(out.y - rySnap) < SnapEps) out.y = rySnap;

			return out;
		}

		// =====================================================================
		// Axis-aligned quad rasterizer (fast path for non-rotated sprites)
		// Supports all SamplerWrapping modes and SIMD blending.
		// =====================================================================
		void DrawAxisAlignedQuad(const DrawContext& ctx, Vertex2D v0, Vertex2D v1, Vertex2D v2, Vertex2D v3)
		{
			if DEATH_UNLIKELY(g_state.colorBuffer == nullptr) return;

			// Screen-space bounding box from all four vertices
			const float fxMin = std::min({v0.x, v1.x, v2.x, v3.x});
			const float fxMax = std::max({v0.x, v1.x, v2.x, v3.x});
			const float fyMin = std::min({v0.y, v1.y, v2.y, v3.y});
			const float fyMax = std::max({v0.y, v1.y, v2.y, v3.y});

			const float fullW = fxMax - fxMin;
			const float fullH = fyMax - fyMin;
			if (fullW < 0.5f || fullH < 0.5f) return;

			// UV corners
			const float uLeft  = (v2.x <= v0.x) ? v2.u : v0.u;
			const float uRight = (v2.x <= v0.x) ? v0.u : v2.u;
			const float vTop   = (v0.y <= v1.y) ? v0.v : v1.v;
			const float vBot   = (v0.y <= v1.y) ? v1.v : v0.v;

			// Pixel bbox clamped to buffer
			std::int32_t xMin = std::max(0, static_cast<std::int32_t>(fxMin));
			std::int32_t xMax = std::min(g_state.bufferWidth  - 1, static_cast<std::int32_t>(fxMax - 0.5f));
			std::int32_t yMin = std::max(0, static_cast<std::int32_t>(fyMin));
			std::int32_t yMax = std::min(g_state.bufferHeight - 1, static_cast<std::int32_t>(fyMax - 0.5f));

			// Scissor pre-clip
			if (g_state.scissorEnabled) {
				xMin = std::max(xMin, g_state.scissorX);
				xMax = std::min(xMax, g_state.scissorX + g_state.scissorW - 1);
				if (g_state.isFboTarget) {
					// TODO: Y-coords need to be flipped when accessing pixels
					const std::int32_t pyMin = g_state.bufferHeight - g_state.scissorY - g_state.scissorH;
					const std::int32_t pyMax = g_state.bufferHeight - 1 - g_state.scissorY;
					yMin = std::max(yMin, pyMin);
					yMax = std::min(yMax, pyMax);
				} else {
					yMin = std::max(yMin, g_state.scissorY);
					yMax = std::min(yMax, g_state.scissorY + g_state.scissorH - 1);
				}
			}
			if (xMin > xMax || yMin > yMax) return;

			// Texture info
			Texture* tex = (ctx.ff.hasTexture && ctx.ff.textureUnit < MaxTextureUnits)
			             ? ctx.textures[ctx.ff.textureUnit] : nullptr;
			const std::uint8_t* texPixels = (tex != nullptr) ? tex->GetPixels(0) : nullptr;
			const std::int32_t texW = (tex != nullptr) ? tex->GetWidth()  : 0;
			const std::int32_t texH = (tex != nullptr) ? tex->GetHeight() : 0;
			const SamplerWrapping wrapS = (tex != nullptr) ? tex->GetWrapS() : SamplerWrapping::ClampToEdge;
			const SamplerWrapping wrapT = (tex != nullptr) ? tex->GetWrapT() : SamplerWrapping::ClampToEdge;
			const bool useLinear = (tex != nullptr && tex->GetMagFilter() == SamplerFilter::Linear && texW > 1 && texH > 1);

			// Tint color as [0..255]
			const std::int32_t tR = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[0]) * 255.0f + 0.5f);
			const std::int32_t tG = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[1]) * 255.0f + 0.5f);
			const std::int32_t tB = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[2]) * 255.0f + 0.5f);
			const std::int32_t tA = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[3]) * 255.0f + 0.5f);
			const bool whiteTint = (tR >= 255 && tG >= 255 && tB >= 255 && tA >= 255);

			const bool useBlend = g_state.blendingEnabled;
			const bool useFastBlend = useBlend &&
				g_state.blendSrc == BlendFactor::SrcAlpha &&
				g_state.blendDst == BlendFactor::OneMinusSrcAlpha;

			// 16.16 fixed-point UV steps
			const std::int32_t texWFix = texW << 16;
			const std::int32_t texHFix = texH << 16;
			const std::int32_t dtxFix = (texPixels != nullptr) ? static_cast<std::int32_t>((uRight - uLeft) * texW * 65536.0f / fullW) : 0;
			const std::int32_t dtyFix = (texPixels != nullptr) ? static_cast<std::int32_t>((vBot - vTop) * texH * 65536.0f / fullH) : 0;

			const std::int32_t txBase = (texPixels != nullptr) ? static_cast<std::int32_t>((uLeft + (xMin + 0.5f - fxMin) * (uRight - uLeft) / fullW) * texW * 65536.0f) : 0;
			std::int32_t tyFix = (texPixels != nullptr) ? static_cast<std::int32_t>((vTop + (yMin + 0.5f - fyMin) * (vBot - vTop) / fullH) * texH * 65536.0f) : 0;

			const bool useRepeatS = (wrapS == SamplerWrapping::Repeat);
			const bool useRepeatT = (wrapT == SamplerWrapping::Repeat);
			const bool useClampS = (wrapS == SamplerWrapping::ClampToEdge);
			// Check if all UVs are safely within texture bounds (skip per-pixel wrapping)
			const bool uvSafeX = useClampS && txBase >= 0 && (txBase + dtxFix * (xMax - xMin)) >= 0 &&
			                     (txBase >> 16) < texW && ((txBase + dtxFix * (xMax - xMin)) >> 16) < texW;

			const std::int32_t scanWidth = xMax - xMin + 1;

			// Stack-allocated scanline buffer for SIMD blending or direct copy
			constexpr std::int32_t MaxScanBuf = 4096;
			alignas(32) std::uint8_t scanBuf[MaxScanBuf * 4];
			const bool useScanBuf = (useFastBlend || !useBlend) && scanWidth <= MaxScanBuf && texPixels != nullptr;

			for (std::int32_t py = yMin; py <= yMax; ++py, tyFix += dtyFix) {
				// TODO: Y-coords need to be flipped when accessing pixels
				//std::uint8_t* dstRow = g_state.colorBuffer + (py * g_state.bufferWidth + xMin) * 4;
				const std::int32_t storeY = g_state.isFboTarget ? (g_state.bufferHeight - 1 - py) : py;
				std::uint8_t* dstRow = g_state.colorBuffer + (storeY * g_state.bufferWidth + xMin) * 4;

				// Get source Y with wrapping
				std::int32_t srcY;
				if DEATH_LIKELY(texPixels != nullptr) {
					if (useRepeatT) {
						srcY = WrapTexelFix(tyFix, texH, SamplerWrapping::Repeat);
					} else {
						srcY = WrapTexelFix(tyFix, texH, wrapT);
					}
				} else {
					srcY = 0;
				}
				const std::uint8_t* texRow = (texPixels != nullptr) ? texPixels + static_cast<std::size_t>(srcY) * texW * 4 : nullptr;

				if DEATH_LIKELY(useScanBuf) {
					// === Scanline buffer path: gather raw texels → callback/tint → SIMD blend ===
					std::int32_t txFix = txBase;

					if (useRepeatS && texPixels != nullptr && !useLinear) {
						txFix = NormalizeRepeatFix(txFix, texWFix);
					}

					// Phase 1: gather RAW texels into scanBuf (no tint applied)
					if (useLinear && texPixels != nullptr) {
						const BilinearRowCtx brow = PrepareBilinearRow(texPixels, texW, texH, tyFix, wrapT);
						for (std::int32_t i = 0; i < scanWidth; ++i) {
							SampleBilinearFixRow(brow, texW, txFix, wrapS, &scanBuf[i * 4]);
							txFix += dtxFix;
						}
					} else if DEATH_LIKELY(texRow != nullptr) {
						if (uvSafeX) {
							for (std::int32_t i = 0; i < scanWidth; ++i) {
								const std::int32_t srcX = txFix >> 16;
								std::memcpy(&scanBuf[i * 4], &texRow[srcX * 4], 4);
								txFix += dtxFix;
							}
						} else if (useRepeatS) {
							for (std::int32_t i = 0; i < scanWidth; ++i) {
								const std::int32_t srcX = txFix >> 16;
								std::memcpy(&scanBuf[i * 4], &texRow[srcX * 4], 4);
								txFix = AdvanceRepeatFix(txFix, dtxFix, texWFix);
							}
						} else {
							for (std::int32_t i = 0; i < scanWidth; ++i) {
								const std::int32_t srcX = WrapTexelFix(txFix, texW, wrapS);
								std::memcpy(&scanBuf[i * 4], &texRow[srcX * 4], 4);
								txFix += dtxFix;
							}
						}
					}

					// Phase 2: apply fragment shader callback or vertex-color tint
					if DEATH_UNLIKELY(ctx.fragmentShader != nullptr) {
						FragmentShaderInput fsInput;
						fsInput.v = tyFix / 65536.0f / static_cast<float>(texH > 0 ? texH : 1);
						fsInput.texWidth = texW;
						fsInput.texHeight = texH;
						fsInput.textures = ctx.textures;
						fsInput.color = ctx.ff.color;
						fsInput.userData = ctx.fragmentShaderUserData;
						const float invTexW = 1.0f / static_cast<float>(texW > 0 ? texW : 1);
						std::int32_t txFixShader = txBase;
						for (std::int32_t i = 0; i < scanWidth; ++i) {
							fsInput.rgba = &scanBuf[i * 4];
							fsInput.u = txFixShader / 65536.0f * invTexW;
							fsInput.x = xMin + i;
							fsInput.y = py;
							ctx.fragmentShader(fsInput);
							txFixShader += dtxFix;
						}
					} else if (!whiteTint) {
						for (std::int32_t i = 0; i < scanWidth; ++i) {
							scanBuf[i * 4 + 0] = static_cast<std::uint8_t>((scanBuf[i * 4 + 0] * tR) >> 8);
							scanBuf[i * 4 + 1] = static_cast<std::uint8_t>((scanBuf[i * 4 + 1] * tG) >> 8);
							scanBuf[i * 4 + 2] = static_cast<std::uint8_t>((scanBuf[i * 4 + 2] * tB) >> 8);
							scanBuf[i * 4 + 3] = static_cast<std::uint8_t>((scanBuf[i * 4 + 3] * tA) >> 8);
						}
					}

					// Phase 3: blend or direct copy to framebuffer
					if (useFastBlend) {
						blendScanlineSrcAlpha(dstRow, scanBuf, scanWidth);
					} else {
						// No blending - direct copy
						std::memcpy(dstRow, scanBuf, static_cast<std::size_t>(scanWidth) * 4);
					}
				} else {
					// === Direct per-pixel path (no SIMD, supports all blend modes) ===
					std::int32_t txFix = txBase;
					if (useRepeatS && texPixels != nullptr && !useLinear) {
						txFix = NormalizeRepeatFix(txFix, texWFix);
					}

					for (std::int32_t px = xMin; px <= xMax; ++px, dstRow += 4) {
						std::int32_t sR, sG, sB, sA;
						if (useLinear && texPixels != nullptr) {
							std::uint8_t raw[4];
							SampleBilinearFix(texPixels, texW, texH, txFix, tyFix, wrapS, wrapT, raw);
							txFix += dtxFix;
							sR = raw[0]; sG = raw[1]; sB = raw[2]; sA = raw[3];
						} else if (texRow != nullptr) {
							std::int32_t srcX;
							if (useRepeatS) {
								srcX = txFix >> 16;
								txFix = AdvanceRepeatFix(txFix, dtxFix, texWFix);
							} else if (uvSafeX) {
								srcX = txFix >> 16;
								txFix += dtxFix;
							} else {
								srcX = WrapTexelFix(txFix, texW, wrapS);
								txFix += dtxFix;
							}
							const std::uint8_t* src = &texRow[srcX * 4];
							sR = src[0]; sG = src[1]; sB = src[2]; sA = src[3];
						} else {
							sR = 255; sG = 255; sB = 255; sA = 255;
							txFix += dtxFix;
						}

						// Apply fragment shader callback or vertex-color tint
						if DEATH_UNLIKELY(ctx.fragmentShader != nullptr) {
							std::uint8_t px4[4] = {
								static_cast<std::uint8_t>(sR), static_cast<std::uint8_t>(sG),
								static_cast<std::uint8_t>(sB), static_cast<std::uint8_t>(sA)
							};
							FragmentShaderInput fsInput;
							fsInput.rgba = px4;
							fsInput.u = (txFix - dtxFix) / 65536.0f / static_cast<float>(texW > 0 ? texW : 1);
							fsInput.v = tyFix / 65536.0f / static_cast<float>(texH > 0 ? texH : 1);
							fsInput.x = px;
							fsInput.y = py;
							fsInput.texWidth = texW;
							fsInput.texHeight = texH;
							fsInput.textures = ctx.textures;
							fsInput.color = ctx.ff.color;
							fsInput.userData = ctx.fragmentShaderUserData;
							ctx.fragmentShader(fsInput);
							sR = px4[0]; sG = px4[1]; sB = px4[2]; sA = px4[3];
						} else if (!whiteTint) {
							sR = (sR * tR) >> 8;
							sG = (sG * tG) >> 8;
							sB = (sB * tB) >> 8;
							sA = (sA * tA) >> 8;
						}

						if (useBlend) {
							if (sA == 0) continue;
							if (useFastBlend) {
								if (sA >= 255) {
									dstRow[0] = static_cast<std::uint8_t>(sR);
									dstRow[1] = static_cast<std::uint8_t>(sG);
									dstRow[2] = static_cast<std::uint8_t>(sB);
									dstRow[3] = static_cast<std::uint8_t>(sA);
								} else {
									const std::int32_t inv = 255 - sA;
									dstRow[0] = static_cast<std::uint8_t>((sR * sA + dstRow[0] * inv) >> 8);
									dstRow[1] = static_cast<std::uint8_t>((sG * sA + dstRow[1] * inv) >> 8);
									dstRow[2] = static_cast<std::uint8_t>((sB * sA + dstRow[2] * inv) >> 8);
									dstRow[3] = static_cast<std::uint8_t>((sA * 255 + dstRow[3] * inv) >> 8);
								}
							} else {
								BlendPixelGeneric(dstRow, sR, sG, sB, sA, g_state.blendSrc, g_state.blendDst);
							}
						} else {
							dstRow[0] = static_cast<std::uint8_t>(sR);
							dstRow[1] = static_cast<std::uint8_t>(sG);
							dstRow[2] = static_cast<std::uint8_t>(sB);
							dstRow[3] = static_cast<std::uint8_t>(sA);
						}
					}
				}
			}
		}

		// =====================================================================
		// Triangle rasterizer - incremental edge functions, pre-clipped
		// Supports all SamplerWrapping modes and blend modes.
		// =====================================================================
		void RasterizeTriangle(const DrawContext& ctx, Vertex2D v0, Vertex2D v1, Vertex2D v2)
		{
			// Bounding box in screen space
			std::int32_t minX = std::max(0, static_cast<std::int32_t>(std::min({v0.x, v1.x, v2.x})));
			std::int32_t maxX = std::min(g_state.bufferWidth  - 1, static_cast<std::int32_t>(std::max({v0.x, v1.x, v2.x})));
			std::int32_t minY = std::max(0, static_cast<std::int32_t>(std::min({v0.y, v1.y, v2.y})));
			std::int32_t maxY = std::min(g_state.bufferHeight - 1, static_cast<std::int32_t>(std::max({v0.y, v1.y, v2.y})));

			// Pre-clip to scissor
			if (g_state.scissorEnabled) {
				if (g_state.isFboTarget) {
					// TODO: Y-coords need to be flipped when accessing pixels
					const std::int32_t pyMin = g_state.bufferHeight - g_state.scissorY - g_state.scissorH;
					const std::int32_t pyMax = g_state.bufferHeight - 1 - g_state.scissorY;
					minX = std::max(minX, g_state.scissorX);
					maxX = std::min(maxX, g_state.scissorX + g_state.scissorW - 1);
					minY = std::max(minY, pyMin);
					maxY = std::min(maxY, pyMax);
				} else {
					minX = std::max(minX, g_state.scissorX);
					maxX = std::min(maxX, g_state.scissorX + g_state.scissorW - 1);
					minY = std::max(minY, g_state.scissorY);
					maxY = std::min(maxY, g_state.scissorY + g_state.scissorH - 1);
				}
			}
			if (minX > maxX || minY > maxY) return;

			// Edge function: E(A→B, P) = (B.x-A.x)*(P.y-A.y) - (B.y-A.y)*(P.x-A.x)
			// Incremental: dE/dx = -(B.y-A.y),  dE/dy = (B.x-A.x)
			const float w0_dx = v1.y - v2.y, w0_dy = v2.x - v1.x;
			const float w1_dx = v2.y - v0.y, w1_dy = v0.x - v2.x;
			const float w2_dx = v0.y - v1.y, w2_dy = v1.x - v0.x;

			const float area = (v2.x - v1.x) * (v0.y - v1.y) - (v2.y - v1.y) * (v0.x - v1.x);
			if (std::fabs(area) < 1e-6f) return; // degenerate
			const float invArea = 1.0f / area;
			const bool signPos = (area > 0.0f);

			// Edge functions at pixel center (minX+0.5, minY+0.5)
			const float px0 = minX + 0.5f, py0 = minY + 0.5f;
			float w0_row = (v2.x - v1.x) * (py0 - v1.y) - (v2.y - v1.y) * (px0 - v1.x);
			float w1_row = (v0.x - v2.x) * (py0 - v2.y) - (v0.y - v2.y) * (px0 - v2.x);
			float w2_row = (v1.x - v0.x) * (py0 - v0.y) - (v1.y - v0.y) * (px0 - v0.x);

			Texture* tex = (ctx.ff.hasTexture && ctx.ff.textureUnit < MaxTextureUnits)
			             ? ctx.textures[ctx.ff.textureUnit] : nullptr;
			const std::uint8_t* texPixels = (tex != nullptr) ? tex->GetPixels(0) : nullptr;
			const std::int32_t texW = (tex != nullptr) ? tex->GetWidth()  : 0;
			const std::int32_t texH = (tex != nullptr) ? tex->GetHeight() : 0;
			const SamplerWrapping wrapS = (tex != nullptr) ? tex->GetWrapS() : SamplerWrapping::ClampToEdge;
			const SamplerWrapping wrapT = (tex != nullptr) ? tex->GetWrapT() : SamplerWrapping::ClampToEdge;
			const bool useLinear = (tex != nullptr && tex->GetMagFilter() == SamplerFilter::Linear && texW > 1 && texH > 1);

			const std::int32_t tR = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[0]) * 255.0f + 0.5f);
			const std::int32_t tG = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[1]) * 255.0f + 0.5f);
			const std::int32_t tB = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[2]) * 255.0f + 0.5f);
			const std::int32_t tA = static_cast<std::int32_t>(std::min(1.0f, ctx.ff.color[3]) * 255.0f + 0.5f);

			const bool useBlend = g_state.blendingEnabled;
			const bool useFastBlend = useBlend &&
				g_state.blendSrc == BlendFactor::SrcAlpha &&
				g_state.blendDst == BlendFactor::OneMinusSrcAlpha;

			for (std::int32_t py = minY; py <= maxY; ++py) {
				float w0 = w0_row, w1 = w1_row, w2 = w2_row;
				// TODO: Y-coords need to be flipped when accessing pixels
				//std::uint8_t* dstRow = g_state.colorBuffer + (py * g_state.bufferWidth + minX) * 4;
				const std::int32_t storeY = g_state.isFboTarget ? (g_state.bufferHeight - 1 - py) : py;
				std::uint8_t* dstRow = g_state.colorBuffer + (storeY * g_state.bufferWidth + minX) * 4;

				for (std::int32_t px = minX; px <= maxX; ++px, dstRow += 4, w0 += w0_dx, w1 += w1_dx, w2 += w2_dx) {
					if ((w0 >= 0.0f) != signPos || (w1 >= 0.0f) != signPos || (w2 >= 0.0f) != signPos)
						continue;

					const float b0 = w0 * invArea;
					const float b1 = w1 * invArea;
					const float b2 = w2 * invArea;

					float u = b0 * v0.u + b1 * v1.u + b2 * v2.u;
					float vv = b0 * v0.v + b1 * v1.v + b2 * v2.v;

					std::int32_t sR, sG, sB, sA;
					if (texPixels != nullptr && useLinear) {
						u = WrapUV(u, wrapS);
						vv = WrapUV(vv, wrapT);
						std::uint8_t raw[4];
						SampleBilinearFloat(texPixels, texW, texH, u, vv, wrapS, wrapT, raw);
						sR = raw[0]; sG = raw[1]; sB = raw[2]; sA = raw[3];
					} else if (texPixels != nullptr) {
						u = WrapUV(u, wrapS);
						vv = WrapUV(vv, wrapT);
						const std::int32_t srcX = std::max(0, std::min(texW - 1, static_cast<std::int32_t>(u * (texW - 1) + 0.5f)));
						const std::int32_t srcY = std::max(0, std::min(texH - 1, static_cast<std::int32_t>(vv * (texH - 1) + 0.5f)));
						const std::uint8_t* src = texPixels + (srcY * texW + srcX) * 4;
						sR = src[0]; sG = src[1]; sB = src[2]; sA = src[3];
					} else {
						sR = 255; sG = 255; sB = 255; sA = 255;
					}

					// Apply fragment shader callback or vertex-color tint
					if DEATH_UNLIKELY(ctx.fragmentShader != nullptr) {
						std::uint8_t px4[4] = {
							static_cast<std::uint8_t>(sR), static_cast<std::uint8_t>(sG),
							static_cast<std::uint8_t>(sB), static_cast<std::uint8_t>(sA)
						};
						FragmentShaderInput fsInput;
						fsInput.rgba = px4;
						fsInput.u = u;
						fsInput.v = vv;
						fsInput.x = px;
						fsInput.y = py;
						fsInput.texWidth = texW;
						fsInput.texHeight = texH;
						fsInput.textures = ctx.textures;
						fsInput.color = ctx.ff.color;
						fsInput.userData = ctx.fragmentShaderUserData;
						ctx.fragmentShader(fsInput);
						sR = px4[0]; sG = px4[1]; sB = px4[2]; sA = px4[3];
					} else {
						sR = (sR * tR) >> 8;
						sG = (sG * tG) >> 8;
						sB = (sB * tB) >> 8;
						sA = (sA * tA) >> 8;
					}

					if (useBlend) {
						if (sA == 0) continue;
						if (useFastBlend) {
							if (sA >= 255) {
								dstRow[0] = static_cast<std::uint8_t>(sR);
								dstRow[1] = static_cast<std::uint8_t>(sG);
								dstRow[2] = static_cast<std::uint8_t>(sB);
								dstRow[3] = static_cast<std::uint8_t>(sA);
							} else {
								const std::int32_t inv = 255 - sA;
								dstRow[0] = static_cast<std::uint8_t>((sR * sA + dstRow[0] * inv) >> 8);
								dstRow[1] = static_cast<std::uint8_t>((sG * sA + dstRow[1] * inv) >> 8);
								dstRow[2] = static_cast<std::uint8_t>((sB * sA + dstRow[2] * inv) >> 8);
								dstRow[3] = static_cast<std::uint8_t>((sA * 255 + dstRow[3] * inv) >> 8);
							}
						} else {
							BlendPixelGeneric(dstRow, sR, sG, sB, sA, g_state.blendSrc, g_state.blendDst);
						}
					} else {
						dstRow[0] = static_cast<std::uint8_t>(sR);
						dstRow[1] = static_cast<std::uint8_t>(sG);
						dstRow[2] = static_cast<std::uint8_t>(sB);
						dstRow[3] = static_cast<std::uint8_t>(sA);
					}
				}

				w0_row += w0_dy;
				w1_row += w1_dy;
				w2_row += w2_dy;
			}
		}

		void DrawPrimitive(const DrawContext& ctx, PrimitiveType type, const SmallVectorImpl<std::int32_t>& indices)
		{
			switch (type) {
				case PrimitiveType::Triangles: {
					for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
						RasterizeTriangle(ctx,
						    FetchVertex(ctx, indices[i]),
						    FetchVertex(ctx, indices[i + 1]),
						    FetchVertex(ctx, indices[i + 2]));
					}
					break;
				}
				case PrimitiveType::TriangleStrip: {
					for (std::size_t i = 0; i + 2 < indices.size(); ++i) {
						if (i & 1) {
							RasterizeTriangle(ctx,
							    FetchVertex(ctx, indices[i]),
							    FetchVertex(ctx, indices[i + 2]),
							    FetchVertex(ctx, indices[i + 1]));
						} else {
							RasterizeTriangle(ctx,
							    FetchVertex(ctx, indices[i]),
							    FetchVertex(ctx, indices[i + 1]),
							    FetchVertex(ctx, indices[i + 2]));
						}
					}
					break;
				}
				case PrimitiveType::TriangleFan: {
					for (std::size_t i = 1; i + 1 < indices.size(); ++i) {
						RasterizeTriangle(ctx,
						    FetchVertex(ctx, indices[0]),
						    FetchVertex(ctx, indices[i]),
						    FetchVertex(ctx, indices[i + 1]));
					}
					break;
				}
			}
		}
	}

	// =========================================================================
	// Public draw entry-points
	// =========================================================================
	void SetDrawContext(const DrawContext& ctx)
	{
		static DrawContext s_ctx;
		s_ctx = ctx;
		g_state.drawCtx = &s_ctx;
		// Mirror blend / scissor state into global state so rasterizer picks them up
		g_state.blendingEnabled = ctx.blendingEnabled;
		g_state.blendSrc = ctx.blendSrc;
		g_state.blendDst = ctx.blendDst;
		g_state.scissorEnabled = ctx.scissorEnabled;
		g_state.scissorX = ctx.scissorRect.X;
		g_state.scissorY = ctx.scissorRect.Y;
		g_state.scissorW = ctx.scissorRect.W;
		g_state.scissorH = ctx.scissorRect.H;
	}

	void ClearDrawContext()
	{
		g_state.drawCtx = nullptr;
	}

	void FramebufferBind(Framebuffer& fbo)
	{
		Texture* target = fbo.GetColorTarget();
		if (target == nullptr) return;

		target->EnsureRenderTarget();
		std::uint8_t* texPixels = target->GetMutablePixels(0);
		if (texPixels == nullptr) return;

		// Redirect rasterizer output to the texture
		g_state.mainColorBuffer  = g_state.colorBuffer;
		g_state.mainBufferWidth  = g_state.bufferWidth;
		g_state.mainBufferHeight = g_state.bufferHeight;
		g_state.isFboTarget      = true;

		g_state.colorBuffer  = texPixels;
		g_state.bufferWidth  = target->GetWidth();
		g_state.bufferHeight = target->GetHeight();
	}

	void FramebufferUnbind()
	{
		if (g_state.isFboTarget) {
			g_state.colorBuffer  = g_state.mainColorBuffer;
			g_state.bufferWidth  = g_state.mainBufferWidth;
			g_state.bufferHeight = g_state.mainBufferHeight;
			g_state.isFboTarget  = false;
		}
	}

	void Draw(PrimitiveType type, std::int32_t firstVertex, std::int32_t count)
	{
		if DEATH_UNLIKELY(g_state.drawCtx == nullptr) return;

		// Fast path: procedural 4-vertex quad (TriangleStrip, no VBO)
		if (type == PrimitiveType::TriangleStrip && count == 4 && firstVertex == 0 &&
		    g_state.drawCtx->vertexFormat == nullptr) {
			const DrawContext& ctx = *g_state.drawCtx;
			Vertex2D v0 = FetchVertex(ctx, 0), v1 = FetchVertex(ctx, 1);
			Vertex2D v2 = FetchVertex(ctx, 2), v3 = FetchVertex(ctx, 3);
			// Verify axis-aligned (no rotation: same x for vertical pairs, same y for horizontal pairs)
			if (std::fabs(v0.x - v1.x) < 0.5f && std::fabs(v2.x - v3.x) < 0.5f &&
			    std::fabs(v0.y - v2.y) < 0.5f && std::fabs(v1.y - v3.y) < 0.5f) {
				DrawAxisAlignedQuad(ctx, v0, v1, v2, v3);
				return;
			}
		}

		SmallVector<std::int32_t> indices(static_cast<std::size_t>(count));
		for (std::int32_t i = 0; i < count; ++i) indices[i] = firstVertex + i;
		DrawPrimitive(*g_state.drawCtx, type, indices);
	}

	void DrawInstanced(PrimitiveType type, std::int32_t firstVertex, std::int32_t count, std::int32_t instanceCount)
	{
		for (std::int32_t inst = 0; inst < instanceCount; ++inst) {
			Draw(type, firstVertex, count);
		}
	}

	void DrawIndexed(PrimitiveType type, std::int32_t count, const void* indexOffset, std::int32_t baseVertex)
	{
		if DEATH_UNLIKELY(g_state.drawCtx == nullptr) return;

		const std::uint16_t* iboPtr = static_cast<const std::uint16_t*>(indexOffset);
		SmallVector<std::int32_t> indices(static_cast<std::size_t>(count));
		for (std::int32_t i = 0; i < count; ++i) {
			indices[i] = static_cast<std::int32_t>(iboPtr[i]) + baseVertex;
		}
		DrawPrimitive(*g_state.drawCtx, type, indices);
	}

	void DrawIndexedInstanced(PrimitiveType type, std::int32_t count, const void* indexOffset,
	                          std::int32_t instanceCount, std::int32_t baseVertex)
	{
		for (std::int32_t inst = 0; inst < instanceCount; ++inst) {
			DrawIndexed(type, count, indexOffset, baseVertex);
		}
	}

	// =========================================================================
	// Framebuffer management
	// =========================================================================

	void ResizeColorBuffer(std::int32_t width, std::int32_t height)
	{
		if (g_state.mainBufferWidth == width && g_state.mainBufferHeight == height) {
			return;
		}

		g_state.mainBufferWidth  = width;
		g_state.mainBufferHeight = height;

		const std::size_t colorBytes = static_cast<std::size_t>(width) * height * 4;
		const std::size_t depthBytes = static_cast<std::size_t>(width) * height;

		delete[] g_state.mainColorBuffer;
		delete[] g_state.depthBuffer;

		g_state.mainColorBuffer = new std::uint8_t[colorBytes]();
		g_state.depthBuffer = new float[depthBytes];

		// Redirect active buffer to main (we're not in an FBO when this is called)
		g_state.colorBuffer  = g_state.mainColorBuffer;
		g_state.bufferWidth  = width;
		g_state.bufferHeight = height;

		// Clear depth to far plane
		const float farValue = 1.0f;
		for (std::size_t i = 0; i < depthBytes; ++i) {
			g_state.depthBuffer[i] = farValue;
		}

		// Mirror viewport to full buffer
		g_state.viewportX = 0;
		g_state.viewportY = 0;
		g_state.viewportW = width;
		g_state.viewportH = height;
	}

	const std::uint8_t* GetColorBuffer()
	{
		return g_state.mainColorBuffer;
	}

	std::int32_t GetColorBufferWidth()
	{
		return g_state.bufferWidth;
	}

	std::int32_t GetColorBufferHeight()
	{
		return g_state.bufferHeight;
	}

}

#endif