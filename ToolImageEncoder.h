#pragma once
#include <string>
#include <vector>

// Makes images of tool results small enough for MCP clients.
class ToolImageEncoder
{
public:
	// Pixels are RGBA with 8 bits per channel and rows from top to bottom, alpha is ignored, because JPEG has none.
	// Image is scaled down and compressed with lower quality until it fits size limit.
	static bool EncodeAsJPEG(const std::vector<unsigned char>& Pixels, int Width, int Height, std::vector<unsigned char>& OutJPEG, std::string& OutError);
};