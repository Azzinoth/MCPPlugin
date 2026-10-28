// stb_image_write uses sprintf, CRT would warn about it in every build.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
// Its functions stay internal to this file, so they never clash with other copy of stb_image_write in the same executable.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ThirdParty/stb_image_write/stb_image_write.h"

#include "ToolImageEncoder.h"
#include <algorithm>

namespace
{
	// Claude Desktop rejects tool results larger than 1 MB, image is sent as base64 text that takes 4 bytes for every 3 bytes, the rest is left for text.
	const size_t MaximumJPEGSize = 900 * 1024 / 4 * 3;
	// Claude models without high resolution support scale larger images down to this size anyway.
	const int MaximumLongerSide = 1568;
	const int MinimumLongerSide = 256;
	const int Qualities[] = { 85, 70, 55, 40 };

	void AppendToVector(void* Context, void* Data, int Size)
	{
		std::vector<unsigned char>* Output = static_cast<std::vector<unsigned char>*>(Context);
		const unsigned char* Bytes = static_cast<const unsigned char*>(Data);
		Output->insert(Output->end(), Bytes, Bytes + Size);
	}

	// Every pixel of scaled image is average of source pixels it covers, so thin lines do not disappear as with nearest pixel.
	std::vector<unsigned char> ScaleDown(const std::vector<unsigned char>& Pixels, int Width, int Height, int NewWidth, int NewHeight)
	{
		std::vector<unsigned char> Result(static_cast<size_t>(NewWidth) * NewHeight * 4);
		for (int y = 0; y < NewHeight; y++)
		{
			const int FirstRow = static_cast<int>(static_cast<long long>(y) * Height / NewHeight);
			const int EndRow = static_cast<int>(static_cast<long long>(y + 1) * Height / NewHeight);
			for (int x = 0; x < NewWidth; x++)
			{
				const int FirstColumn = static_cast<int>(static_cast<long long>(x) * Width / NewWidth);
				const int EndColumn = static_cast<int>(static_cast<long long>(x + 1) * Width / NewWidth);

				unsigned int Sums[4] = { 0, 0, 0, 0 };
				for (int SourceY = FirstRow; SourceY < EndRow; SourceY++)
				{
					for (int SourceX = FirstColumn; SourceX < EndColumn; SourceX++)
					{
						const size_t SourceIndex = (static_cast<size_t>(SourceY) * Width + SourceX) * 4;
						for (int Channel = 0; Channel < 4; Channel++)
							Sums[Channel] += Pixels[SourceIndex + Channel];
					}
				}

				const unsigned int Count = static_cast<unsigned int>((EndRow - FirstRow) * (EndColumn - FirstColumn));
				const size_t ResultIndex = (static_cast<size_t>(y) * NewWidth + x) * 4;
				for (int Channel = 0; Channel < 4; Channel++)
					Result[ResultIndex + Channel] = static_cast<unsigned char>((Sums[Channel] + Count / 2) / Count);
			}
		}

		return Result;
	}
}

bool ToolImageEncoder::EncodeAsJPEG(const std::vector<unsigned char>& Pixels, int Width, int Height, std::vector<unsigned char>& OutJPEG, std::string& OutError)
{
	OutJPEG.clear();
	if (Width <= 0 || Height <= 0 || Pixels.size() != static_cast<size_t>(Width) * Height * 4)
	{
		OutError = "it has " + std::to_string(Pixels.size()) + " bytes, but RGBA image " + std::to_string(Width) + "x" + std::to_string(Height) + " with 8 bits per channel should have " +
				   std::to_string(static_cast<size_t>(std::max(Width, 0)) * std::max(Height, 0) * 4);
		return false;
	}

	const double InitialScale = std::min(1.0, static_cast<double>(MaximumLongerSide) / std::max(Width, Height));
	for (double Scale = InitialScale; ; Scale *= 0.75)
	{
		const int ScaledWidth = std::max(1, static_cast<int>(Width * Scale));
		const int ScaledHeight = std::max(1, static_cast<int>(Height * Scale));
		std::vector<unsigned char> ScaledPixels;
		if (ScaledWidth != Width || ScaledHeight != Height)
			ScaledPixels = ScaleDown(Pixels, Width, Height, ScaledWidth, ScaledHeight);

		const unsigned char* PixelsToEncode = ScaledPixels.empty() ? Pixels.data() : ScaledPixels.data();
		for (const int Quality : Qualities)
		{
			OutJPEG.clear();
			if (stbi_write_jpg_to_func(AppendToVector, &OutJPEG, ScaledWidth, ScaledHeight, 4, PixelsToEncode, Quality) == 0)
			{
				OutJPEG.clear();
				OutError = "JPEG encoding failed";
				return false;
			}

			if (OutJPEG.size() <= MaximumJPEGSize)
				return true;
		}

		if (std::max(ScaledWidth, ScaledHeight) <= MinimumLongerSide)
			break;
	}

	OutJPEG.clear();
	OutError = "it does not fit in " + std::to_string(MaximumJPEGSize / 1024) + " KB even when it is scaled down";
	return false;
}