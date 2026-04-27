#pragma once

#include "RHI/RenderTypes.h"
#include "../Primitives/Vector2.h"

#include <memory>

#include <Containers/StringView.h>
#include <IO/Stream.h>

namespace nCine
{
	/// Texture loader interface class
	class ITextureLoader
	{
	public:
		virtual ~ITextureLoader() { }

		/// Returns true if the texture has been correctly loaded
		inline bool hasLoaded() const {
			return hasLoaded_;
		}

		/// Returns texture width
		inline std::int32_t width() const {
			return width_;
		}
		/// Returns texture height
		inline std::int32_t height() const {
			return height_;
		}
		/// Returns texture size as a `Vector2<int>` class
		inline Vector2i size() const {
			return Vector2i(width_, height_);
		}
		/// Returns the number of MIP maps stored in the texture file
		inline std::int32_t mipMapCount() const {
			return mipMapCount_;
		}
		/// Returns texture data size in bytes
		inline std::uint32_t dataSize() const {
			return dataSize_;
		}
		/// Returns the texture data size in bytes for the specified MIP map level
		std::int32_t dataSize(std::uint32_t mipMapLevel) const;
		/// Returns the texture format
		inline RHI::TextureFormat texFormat() const {
			return texFormat_;
		}
		/// Returns the number of colour channels
		inline std::uint32_t numChannels() const {
			return RHI::NumChannels(texFormat_);
		}
		/// Returns true if the format holds compressed data
		inline bool isCompressed() const {
			return RHI::IsCompressed(texFormat_);
		}
		/// Returns the pointer to pixel data
		inline const std::uint8_t* pixels() const {
			return pixels_.get();
		}
		/// Returns the pointer to pixel data for the specified MIP map level
		const std::uint8_t* pixels(std::uint32_t mipMapLevel) const;

		/// Returns the proper texture loader according to the file extension
		static std::unique_ptr<ITextureLoader> createFromFile(const Death::Containers::StringView filename);

	protected:
#ifndef DOXYGEN_GENERATING_OUTPUT
		/// A flag indicating if the loading process has been successful
		bool hasLoaded_;
		/// Texture file handle
		std::unique_ptr<Death::IO::Stream> fileHandle_;

		std::int32_t width_;
		std::int32_t height_;
		std::int32_t headerSize_;
		std::uint32_t dataSize_;
		std::int32_t mipMapCount_;
		std::unique_ptr<std::uint32_t[]> mipDataOffsets_;
		std::unique_ptr<std::uint32_t[]> mipDataSizes_;
		RHI::TextureFormat texFormat_;
		std::unique_ptr<std::uint8_t[]> pixels_;
#endif

		/// An empty constructor only used by `TextureLoaderRaw`
		ITextureLoader();
		explicit ITextureLoader(std::unique_ptr<Death::IO::Stream> fileHandle);

		static std::unique_ptr<ITextureLoader> createLoader(std::unique_ptr<Death::IO::Stream> fileHandle, const Death::Containers::StringView path);
		/// Loads pixel data from a texture file
		void loadPixels(RHI::TextureFormat format);
	};

#ifndef DOXYGEN_GENERATING_OUTPUT
	/// A class created when the texture file extension is not recognized
	class InvalidTextureLoader : public ITextureLoader
	{
	public:
		explicit InvalidTextureLoader(std::unique_ptr<Death::IO::Stream> fileHandle)
			: ITextureLoader(std::move(fileHandle)) { }
	};
#endif
}
